# WineHua 音频架构

> 更新日期: 2026-09-04

## 概览

音频方案做三件事:

```text
1. 把 Win32 程序输出的 PCM 音频稳定地桥接到宿主扬声器 (render)
2. 把宿主麦克风采集的 PCM 桥接给 Win32 程序 (capture)
3. MIDI 软合成 (winehua-gm.sf2 SoundFont)
```

主链路如下:

```mermaid
flowchart LR
    A["Win32 App"] --> B["winmm / dsound / xaudio2 / WASAPI"]
    B --> C["Wine mmdevapi"]
    C --> D["wineohos.drv"]
    D --> E["控制面: AudioBroker IPC"]
    D --> F["数据面: Shared Memory Ring"]
    E -->     G["Host AudioBroker"]
    F --> G
    G --> H["Per-endpoint OH_AudioRenderer / OH_AudioCapturer"]
    H --> I["HarmonyOS mix / Speaker / Microphone"]
```

## 设计原则

- Wine 子进程不直接创建设备。
- Wine 每个 render endpoint 对应一路宿主 `OH_AudioRenderer`，最终混频由 HarmonyOS Audio Service 完成。
- 控制面走 IPC，数据面走共享内存。
- callback 只做本路取数，不做跨流求和、不做阻塞操作。

## 分层

### Wine 侧

- `thirdparty/wine/dlls/wineohos.drv/ohos.c`
- `thirdparty/wine/dlls/wineohos.drv/ohos_audio_client.c`
- `thirdparty/wine/dlls/mmdevapi/client.c`

职责:

- 作为 `mmdevapi` backend 暴露 `wineohos.drv`
- 接收 Windows 共享模式音频流
- 将输入统一转换到固定混音格式
- 写入共享内存 ring buffer
- 通过控制协议完成 `OPEN / START / STOP / RESET / CLOSE / GET_STATUS`

### 宿主侧

- `entry/src/main/cpp/audio_broker.h`
- `entry/src/main/cpp/audio_broker.cpp`
- `entry/src/main/cpp/audio_ipc_server.h`
- `entry/src/main/cpp/audio_ipc_server.cpp`
- `entry/src/main/cpp/audio_stream.h`
- `entry/src/main/cpp/audio_stream.cpp`
- `entry/src/main/cpp/ring_buffer.h`
- `entry/src/main/cpp/ring_buffer.cpp`
- `entry/src/main/cpp/include/audio_ipc_protocol.h`

职责:

- 管理 broker 生命周期
- 创建和管理 stream（render / capture 双向，`AudioStreamDirection`）
- 为每个 stream 创建 memfd ring buffer
- 为每个 render endpoint 创建并启停独立的 `OH_AudioRenderer`
- 在 OHAudio callback 中只读取对应 ring，或写入采集数据（capture）

## 控制面和数据面

### 控制面

控制面使用两级 fd 协议:

```text
App 启动前创建 bootstrap fd
  -> 注入到 Wine 环境变量
  -> Wine 初始化音频时创建 private socketpair
  -> 通过 bootstrap fd 把 private broker fd 发给宿主
  -> 后续命令全部走 private control channel
```

协议定义在:

- `entry/src/main/cpp/include/audio_ipc_protocol.h`

当前命令:

- `HELLO`
- `OPEN_STREAM`
- `START`
- `STOP`
- `RESET`
- `CLOSE`
- `GET_STATUS`

stream 方向由 `WINEHUA_AUDIO_STREAM_FLAG_CAPTURE` 标记区分（`AudioStreamDirection::Capture`）。

### 数据面

每个 render stream 对应一块 ring buffer:

```mermaid
flowchart LR
    A["Wine stream writer"] --> B["write_index"]
    B --> C["Ring Buffer"]
    C --> D["read_index"]
    D --> E["AudioBroker callback"]
```

关键点:

- producer 只有 Wine 写端
- consumer 只有宿主读端
- `read_index / write_index` 用原子字段同步
- callback 不经过 socket，不经过额外拷贝链

## 固定混音格式

宿主 Renderer 固定格式:

```text
48000 Hz
stereo
s16le
```

对外行为:

- Wine `GetMixFormat()` 广告 `48k / stereo / IEEE float32`，避免 BASS 把 float 写进 4 字节 S16 帧
- Wine 把共享模式输入转换成 ring 上的 `48k / stereo / s16le` 再交给宿主 Renderer

当前接受的输入范围:

- 采样率: `22050 / 44100 / 48000`
- 声道: `mono / stereo`
- 样本格式: `s16 / float32`

## Endpoint 映射

当前默认路径（B3）是一条 Wine `IAudioClient` 对应一路 `OH_AudioRenderer`，最终混频由 HarmonyOS Audio Service 完成：

```mermaid
flowchart TD
    S1["Endpoint 1 Ring"] --> R1["OH_AudioRenderer 1 GAME"]
    S2["Endpoint 2 Ring"] --> R2["OH_AudioRenderer 2 GAME"]
    R1 --> S["HarmonyOS Audio Service"]
    R2 --> S
```

实现方式:

- 每个 Wine render stream 一块独立 ring，OPEN 时创建独立 Renderer 对象但不 Start
- Wine START：等 Ring 达到 `2 × callbackFrames`（超时 80 ms）再 Start，并做 3 ms fade-in
- Wine STOP：3 ms fade-out 后 Stop，对象保留；CLOSE 再 Release
- 来电/PAUSE 记住 `wantStarted`，RESUME 后重新 priming 再 Start
- 两路都保持 `USAGE_GAME` + `LATENCY_NORMAL`，48 kHz stereo S16
- callback 只读本路 ring；空数据返回 `INVALID`，不回退到全局 Mixer
- 最终混频、音量和限幅交给 HarmonyOS Audio Service
- WASAPI event 唤醒和 IAudioClock 仍按 Ring 水位近似，尚未改 Wine 侧

对应代码:

- stream 与 Renderer 生命周期: `entry/src/main/cpp/audio/audio_broker.cpp`
- 单路 callback: `AudioBroker::FillRendererCallback()` / `AudioBroker::OnWriteData()`

## 当前边界

当前版本覆盖:

- render（每 endpoint 一路 Renderer，共享模式，默认播放设备，系统混频）
- capture（`OH_AudioCapturer`，`WINEHUA_AUDIO_STREAM_FLAG_CAPTURE`）
- MIDI 软合成（`wineohos.drv/ohos_midi.c` + `MIDI_SOUNDFONT_PATH`）

当前不做:

- exclusive mode
- multichannel output
- broader format negotiation

## TODO

- [ ] Padding / IAudioClock: Ring 消费水位还不等于硬件已播放
- [ ] WASAPI event 改为按 Ring 消费水位唤醒，而不是固定 period
- [ ] Format matrix: `WAV / MP3 / video audio track`
- [ ] Confirm non-audio Wine processes are not slowed by broker init
- [ ] Reduce control-plane overhead on the event-callback path
- [ ] exclusive mode / multichannel output / broader PCM support

## 调试入口

默认日志保留关键事件:

- broker 启动
- renderer ready
- open / start / stop / close
- 失败日志
- close / cleanup 统计

如需更详细的 Wine 日志，可通过:

- `WINEHUA_WINEDEBUG`

覆盖默认 `WINEDEBUG`。
