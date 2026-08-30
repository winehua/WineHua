# Wine 进程启动参数与环境变量基线

> 自动生成: `scripts/proc_baseline_summarize.py`; 采集侧见 `wine_child.cpp` / `virgl_child.cpp` 的 `dump_proc_snapshot`

- **采集时间**: 2026-08-30
- **设备**: `192.168.1.8:33363`
- **代码位点**: master `90edaae` + 进程快照采集补丁 (`wine_child.cpp` + `virgl_child.cpp` 的 `dump_proc_snapshot`)
- **采集原理**: wine 的全部 guest 进程创建 (UI 直启 / 应用库点击 / 文件管理 explorer 里双击 /
  ShellExecute) 统一经 Process Broker → 系统 `StartNativeChildProcess` → NCP 发射点
  (`wine_child.cpp` `Main`/`WineserverMain`, `virgl_child.cpp` `Main`/`NativeChildProcess_MainProc`),
  发射瞬间把真实 `argv` + `environ` 逐条追加到沙箱 `temp/proc_snapshot.log`。
- **去重规则**: 同名进程在会话中可能多次启动, 本文档**以最后一次启动为准**;
  完整时序记录在 `.temp/proc_baseline/proc_snapshot.log`。

共采集 **37** 条发射记录, 去重后 **17** 个进程.  

## 进程清单 (按启动顺序)

| 序号 | 进程 tag | pid | argv |
|------|----------|-----|------|
| 1 | `wine` | `47065` | `box64 /data/storage/el2/base/files/wine/bin/wine wine wineboot --init` |
| 2 | `wineserver` | `47051` | `box64 /data/storage/el2/base/files/wine/bin/wineserver wineserver -f -p` |
| 3 | `wineboot.exe` | `47130` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\wineboot.exe --init` |
| 4 | `winemenubuilder.exe` | `47156` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winemenubuilder.exe -a -r` |
| 5 | `services.exe` | `47161` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\services.exe` |
| 6 | `winedevice.exe` | `47348` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winedevice.exe` |
| 7 | `plugplay.exe` | `47185` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\plugplay.exe` |
| 8 | `svchost.exe` | `47261` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\svchost.exe -k LocalServiceNetworkRestric` |
| 9 | `explorer` | `47647` | `box64 /data/storage/el2/base/files/wine/bin/wine explorer /desktop=shell,1400x920 C:\windows\system32\winehua_` |
| 10 | `explorer.exe` | `47855` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\explorer.exe /desktop` |
| 11 | `winehua_keep.exe` | `47879` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winehua_keep.exe` |
| 12 | `rpcss.exe` | `47923` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\rpcss.exe` |
| 13 | `winehua_d3d_switch_cube.exe` | `53953` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe` |
| 14 | `virgl-ipc-proc` | `46992` | `NativeChildProcess_MainProc` |
| 15 | `virgl-ipc-main` | `46992` | `/data/storage/el1/bundle/libs/arm64/libwinehua_vtest_server.so\|/data/storage/el2/base/files/.wine/graphics/vir` |
| 16 | `winehua_d3d12_smoke.exe` | `49136` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d12_smoke.exe --frames 1000 --width 64` |
| 17 | `winehua_graphics_smoke.exe` | `53155` | `box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_graphics_smoke.exe` |

### `wine` (pid `47065`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine wine wineboot --init
```

**environ (57 项)**

**Wine** (10 项):
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (14 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `USE_LIBBOX64=1`
**System** (29 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `wineserver` (pid `47051`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wineserver wineserver -f -p
```

**environ (41 项)**

**Wine** (2 项):
- `WINEDEBUG=-all`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
**Audio** (2 项):
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (13 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
**System** (24 项):
- `AppSpawnCheckUnexpectedExitCall=47051`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser`
- `LANG=en_US.UTF-8`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `PROCESS_START_TIME=84708789`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `wineboot.exe` (pid `47130`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\wineboot.exe --init
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=16`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=15`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winemenubuilder.exe` (pid `47156`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winemenubuilder.exe -a -r
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=20`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=16`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `services.exe` (pid `47161`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\services.exe
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=21`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=20`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winedevice.exe` (pid `47348`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winedevice.exe
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=24`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=23`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `plugplay.exe` (pid `47185`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\plugplay.exe
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=23`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=22`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `svchost.exe` (pid `47261`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\svchost.exe -k LocalServiceNetworkRestricted
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=23`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=16`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `explorer` (pid `47647`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine explorer /desktop=shell,1400x920 C:\windows\system32\winehua_keep.exe
```

**environ (127 项)**

**Wine** (65 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
- `DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
- `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
- `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
- `DXVK_WINEHUA_PRECISE_SHADOW=1`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=legacy`
- `WINEHUA_DXVK_RELAXED_FEATURES=1`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
- `WINEHUA_DXVK_VERSION=1.10.3`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=15`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (12 项):
- `DXVK_LOG_LEVEL=warn`
- `DXVK_LOG_PATH=C:\windows\temp`
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (15 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `USE_LIBBOX64=1`
**System** (32 项):
- `AppSpawnCheckUnexpectedExitCall=47647`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84713379`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `explorer.exe` (pid `47855`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\explorer.exe /desktop
```

**environ (134 项)**

**Wine** (67 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=24`
- `DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
- `DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
- `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
- `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
- `DXVK_WINEHUA_PRECISE_SHADOW=1`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=legacy`
- `WINEHUA_DXVK_RELAXED_FEATURES=1`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
- `WINEHUA_DXVK_VERSION=1.10.3`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=26`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=25`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (12 项):
- `DXVK_LOG_LEVEL=warn`
- `DXVK_LOG_PATH=C:\windows\temp`
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (19 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (33 项):
- `AppSpawnCheckUnexpectedExitCall=47647`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84713379`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winehua_keep.exe` (pid `47879`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\winehua_keep.exe
```

**environ (134 项)**

**Wine** (67 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=24`
- `DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
- `DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
- `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
- `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
- `DXVK_WINEHUA_PRECISE_SHADOW=1`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=legacy`
- `WINEHUA_DXVK_RELAXED_FEATURES=1`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
- `WINEHUA_DXVK_VERSION=1.10.3`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=27`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=26`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (12 项):
- `DXVK_LOG_LEVEL=warn`
- `DXVK_LOG_PATH=C:\windows\temp`
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (19 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (33 项):
- `AppSpawnCheckUnexpectedExitCall=47647`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84713379`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `rpcss.exe` (pid `47923`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\windows\system32\rpcss.exe
```

**environ (68 项)**

**Wine** (16 项):
- `APPSPAWN_FD_wine_audio_bootstrap=15`
- `APPSPAWN_FD_wineserver_sock=16`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINESERVERSOCKET=28`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=27`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (1 项):
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (18 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=2`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `BOX64_SYSINFO_CACHED=1`
- `BOX64_SYSINFO_CPUNAME=Unknown CPU`
- `BOX64_SYSINFO_FREQUENCY=2400000000`
- `BOX64_SYSINFO_NCPU=12`
- `USE_LIBBOX64=1`
**System** (30 项):
- `AppSpawnCheckUnexpectedExitCall=47065`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84708821`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winehua_d3d_switch_cube.exe` (pid `53953`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe
```

**environ (125 项)**

**Wine** (65 项):
- `APPSPAWN_FD_wine_audio_bootstrap=28`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_AUTOMATION=0`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP=shell`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=modern-2.6`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `WINEHUA_DXVK_VERSION=2.6.2`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_HOST_ARCH=aarch64`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_PRESENT_BACKEND=venus_broker_present`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_WINE_UNIX_ARCH=x86_64`
- `WINEHUA_WORKING_DIRECTORY=/data/storage/el2/base/files/.wine/drive_c/smoke/x64`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=28`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (10 项):
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (15 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `USE_LIBBOX64=1`
**System** (32 项):
- `AppSpawnCheckUnexpectedExitCall=53953`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84785567`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `virgl-ipc-proc` (pid `46992`)

**argv**:

```
NativeChildProcess_MainProc
```

**environ (23 项)**

**System** (23 项):
- `AppSpawnCheckUnexpectedExitCall=46992`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser`
- `LANG=en_US.UTF-8`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_AppSpawn=14`
- `OLDPWD=/`
- `PATH=/data/app/bin:/data/service/hnp/bin:/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `PROCESS_START_TIME=84708556`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `__LIBACE_ENTRY_POINT=5be0bd5590`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `virgl-ipc-main` (pid `46992`)

**argv**:

```
/data/storage/el1/bundle/libs/arm64/libwinehua_vtest_server.so|/data/storage/el2/base/files/.wine/graphics/virgl.sock|__env=LD_LIBRARY_PATH=/data/storage/el1/bundle/libs/arm64|__env=VTEST_USE_GLES=1|__env=VTEST_USE_EGL_SURFACELESS=1|__env=VTEST_SYNC_GL_FINISH=1|__env=WINEHUA_VIRGL_SYNC_MODE=egl-main|__env=WINEHUA_VIRGL_LOG_PATH=/data/storage/el2/base/cache/winehua_virgl_host.log|__env=WINEHUA_VKD3D_GATE_C_TRACE=0|__env=WINEHUA_VKR_TRACE_SAMPLED=0|__env=WINEHUA_VKR_TRACE_CAPTURE=0|__env=WINEHUA_VKR_TRACE_CAPTURE_LIMIT=512|__env=WINEHUA_RESOURCE_TRACE=0|__env=WINEHUA_VKR_TRACE_UBO_IDENTITY=0|__env=WINEHUA_VKR_TRACE_PRESENT_IMAGE=0|__env=WINEHUA_VK_PRESENT_TRACE=0|__env=WINEHUA_VENUS_FORCE_SOURCE_CLEAR=0|__env=WINEHUA_VKR_TRACE_PIPELINE=0|__env=VKR_WINEHUA_SHADOW_FROM_HOST=precise|__env=VKR_WINEHUA_SHADOW_TO_HOST=explicit|__env=VKR_WINEHUA_SHADOW_TRACE=0|__env=VKR_WINEHUA_BGRA_ARRAY_RGBA=0|__env=VKR_WINEHUA_PERF_SUMMARY=0|__env=VKR_WINEHUA_PERF_SAMPLE_INTERVAL=0|__env=VKR_WINEHUA_FRAME_TIMELINE_INTERVAL=0|__env=WINEHUA_VENUS_GPU_FRAME_PROFILE=0|__env=WINEHUA_VTEST_PRESENT_PERF_SUMMARY=0|__env=VKR_WINEHUA_GPU_UPLOAD=auto|__env=VKR_WINEHUA_GPU_UPLOAD_WAIT=0|__env=VKR_WINEHUA_GPU_UPLOAD_INLINE=0|__env=VKR_WINEHUA_COVERAGE_SORT=0|__env=VKR_WINEHUA_GPU_UPLOAD_SERIALIZE=0|__env=VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE=0|__env=VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=0|__env=VKR_WINEHUA_SHADOW_DIRTY_LIST=1|__env=VKR_WINEHUA_BOUND_BUFFER_LIST=0|__env=VKR_WINEHUA_BATCH_FLUSH=1|__env=VKR_WINEHUA_SHADOW_MERGE_RANGES=1|__env=VKR_WINEHUA_SHADOW_COVER_UPLOAD=0|__env=WINEHUA_VKR_PRESENT_STAGE_TRACE=0|__env=WINEHUA_VKR_PRESENT_PREWAIT=0|__env=WINEHUA_VKR_SUBMIT_POSTWAIT=0|__env=WINEHUA_VENUS_PRESENT_MODE=fifo|__env=EGL_PLATFORM=surfaceless
```

**environ (24 项)**

**Graphics** (1 项):
- `EGL_PLATFORM=surfaceless`
**System** (23 项):
- `AppSpawnCheckUnexpectedExitCall=46992`
- `DOWNLOAD_CACHE=/data/cache`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser`
- `LANG=en_US.UTF-8`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_AppSpawn=14`
- `OLDPWD=/`
- `PATH=/data/app/bin:/data/service/hnp/bin:/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `PROCESS_START_TIME=84708556`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `__LIBACE_ENTRY_POINT=5be0bd5590`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winehua_d3d12_smoke.exe` (pid `49136`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d12_smoke.exe --frames 1000 --width 640 --height 480 --sync-interval 0 --hold-ms 30000 --result C:\smoke\results\vkd3d-manual.json --checkpoint C:\smoke\results\vkd3d-manual.checkpoint.json
```

**environ (127 项)**

**Wine** (67 项):
- `APPSPAWN_FD_wine_audio_bootstrap=25`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_PRESENT_TRACE=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_AUTOMATION=0`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP=shell`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=modern-2.6`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `WINEHUA_DXVK_TRACE_PRESENT_IMAGE=1`
- `WINEHUA_DXVK_VERSION=2.6.2`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_HOST_ARCH=aarch64`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_PRESENT_BACKEND=venus_broker_present`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_WINE_UNIX_ARCH=x86_64`
- `WINEHUA_WORKING_DIRECTORY=C:\smoke\x64`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=25`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (10 项):
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (15 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `USE_LIBBOX64=1`
**System** (32 项):
- `AppSpawnCheckUnexpectedExitCall=49136`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84730757`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

### `winehua_graphics_smoke.exe` (pid `53155`)

**argv**:

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_graphics_smoke.exe
```

**environ (125 项)**

**Wine** (65 项):
- `APPSPAWN_FD_wine_audio_bootstrap=25`
- `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
- `VKR_WINEHUA_SHADOW_FROM_HOST=precise`
- `VN_WINEHUA_DIRECT_FENCE_WAIT=1`
- `VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`
- `WINEBINDIR=/data/storage/el2/base/files/wine/bin`
- `WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
- `WINEDEBUG=-all`
- `WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
- `WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
- `WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
- `WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
- `WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
- `WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `WINEHUA_AUTOMATION=0`
- `WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
- `WINEHUA_DESKTOP=shell`
- `WINEHUA_DESKTOP_MODE=1`
- `WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
- `WINEHUA_DXVK_PROFILE=modern-2.6`
- `WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `WINEHUA_DXVK_VERSION=2.6.2`
- `WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
- `WINEHUA_FRAME_ZERO_COPY=1`
- `WINEHUA_GL_STALL_DIAG=1`
- `WINEHUA_GRAPHICS_ACTIVE=virgl`
- `WINEHUA_GRAPHICS_BACKEND=virgl`
- `WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
- `WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
- `WINEHUA_GUEST_GFX_PLATFORM=wayland`
- `WINEHUA_GUEST_GFX_READY=1`
- `WINEHUA_HOST_ARCH=aarch64`
- `WINEHUA_PERF_PROFILE=shadow-precise`
- `WINEHUA_PRESENT_BACKEND=venus_broker_present`
- `WINEHUA_SHM_FALLBACK=0`
- `WINEHUA_VENUS_ICD_ARCH=x86_64`
- `WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
- `WINEHUA_VIRGL_LIBRARY_READY=1`
- `WINEHUA_VIRGL_READY=1`
- `WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WINEHUA_VIRGL_SOCKET_READY=1`
- `WINEHUA_VKD3D_PROFILE=limited-500k`
- `WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `WINEHUA_VKD3D_VERSION=2.6`
- `WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `WINEHUA_VTEST_PRESENT=surface-queue`
- `WINEHUA_VULKAN_LOADER_ARCH=x86_64`
- `WINEHUA_VULKAN_PRESENT=1`
- `WINEHUA_VULKAN_RUNTIME=1`
- `WINEHUA_WAYLAND_READBACK=1`
- `WINEHUA_WINE_UNIX_ARCH=x86_64`
- `WINEHUA_WORKING_DIRECTORY=/data/storage/el2/base/files/.wine/drive_c/smoke/x64`
- `WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
- `WINEPREFIX=/data/storage/el2/base/files/.wine`
- `WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
- `WINEWAYLAND_ENTER_SILENT=1`
- `WINE_OHOS_AUDIO_BOOTSTRAP_FD=25`
- `WINE_OHOS_AUDIO_ENABLE=1`
- `WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
**Graphics** (10 项):
- `EGL_PLATFORM=wayland`
- `LIBGL_ALWAYS_SOFTWARE=1`
- `LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `MESA_LOADER_DRIVER_OVERRIDE=swrast`
- `VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `VN_DEBUG=vtest`
- `VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
- `VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `WAYLAND_DISPLAY=wine-wayland`
**Audio** (3 项):
- `MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
- `PULSE_STATE_PATH=/data/data/.pulse_dir/state`
**Box64** (15 项):
- `BOX64_AES=0`
- `BOX64_AVX=0`
- `BOX64_DYNAREC_BIGBLOCK=3`
- `BOX64_DYNAREC_CALLRET=2`
- `BOX64_DYNAREC_FORWARD=1024`
- `BOX64_DYNAREC_SAFEFLAGS=1`
- `BOX64_DYNAREC_VOLATILE_METADATA=0`
- `BOX64_DYNAREC_WEAKBARRIER=0`
- `BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
- `BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
- `BOX64_LOG=0`
- `BOX64_NOBANNER=1`
- `BOX64_PCLMULQDQ=0`
- `BOX64_SHOWSEGV=1`
- `USE_LIBBOX64=1`
**System** (32 项):
- `AppSpawnCheckUnexpectedExitCall=53155`
- `DOWNLOAD_CACHE=/data/cache`
- `GALLIUM_DRIVER=virpipe`
- `GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `HAP_DEBUGGABLE=true`
- `HNP_PRIVATE_HOME=/data/app`
- `HNP_PUBLIC_HOME=/data/service/hnp`
- `HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `LANG=zh_CN.UTF-8`
- `LC_ALL=zh_CN.UTF-8`
- `LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `LD_PRELOAD=libappspawn_helper.z.so`
- `MALI_REPORT_MEM_USAGE=1`
- `OHOS_SOCKET_NativeSpawn=18`
- `OLDPWD=/`
- `PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
- `PROCESS_START_TIME=84779917`
- `PWD=/storage/Users/currentUser`
- `SHELL=/bin/sh`
- `SHLVL=1`
- `TERM=ansi`
- `TMP=/data/local/mtp_tmp/`
- `TMPDIR=/data/storage/el2/base/cache`
- `UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `USER=100`
- `XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
- `XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `hwasanEnabled=0`
- `tsanEnabled=0`
- `ubsanEnabled=0`

## 附录 A — 环境变量矩阵 (去重后进程, 按 KEY 分组)

### `APPSPAWN_FD_wine_audio_bootstrap`

- `wineboot.exe@pid47130`: `15`
- `winemenubuilder.exe@pid47156`: `15`
- `services.exe@pid47161`: `15`
- `winedevice.exe@pid47348`: `15`
- `plugplay.exe@pid47185`: `15`
- `svchost.exe@pid47261`: `15`
- `explorer@pid47647`: `15`
- `explorer.exe@pid47855`: `15`
- `winehua_keep.exe@pid47879`: `15`
- `rpcss.exe@pid47923`: `15`
- `winehua_d3d_switch_cube.exe@pid53953`: `28`
- `winehua_d3d12_smoke.exe@pid49136`: `25`
- `winehua_graphics_smoke.exe@pid53155`: `25`

### `APPSPAWN_FD_wineserver_sock`

- `wineboot.exe@pid47130`: `16`
- `winemenubuilder.exe@pid47156`: `16`
- `services.exe@pid47161`: `16`
- `winedevice.exe@pid47348`: `16`
- `plugplay.exe@pid47185`: `16`
- `svchost.exe@pid47261`: `16`
- `explorer.exe@pid47855`: `24`
- `winehua_keep.exe@pid47879`: `24`
- `rpcss.exe@pid47923`: `16`

### `AppSpawnCheckUnexpectedExitCall`

- `wine@pid47065`: `47065`
- `wineserver@pid47051`: `47051`
- `wineboot.exe@pid47130`: `47065`
- `winemenubuilder.exe@pid47156`: `47065`
- `services.exe@pid47161`: `47065`
- `winedevice.exe@pid47348`: `47065`
- `plugplay.exe@pid47185`: `47065`
- `svchost.exe@pid47261`: `47065`
- `explorer@pid47647`: `47647`
- `explorer.exe@pid47855`: `47647`
- `winehua_keep.exe@pid47879`: `47647`
- `rpcss.exe@pid47923`: `47065`
- `winehua_d3d_switch_cube.exe@pid53953`: `53953`
- `virgl-ipc-proc@pid46992`: `46992`
- `virgl-ipc-main@pid46992`: `46992`
- `winehua_d3d12_smoke.exe@pid49136`: `49136`
- `winehua_graphics_smoke.exe@pid53155`: `53155`

### `BOX64_AES`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_AVX`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_DYNAREC_BIGBLOCK`

- `wine@pid47065`: `3`
- `wineserver@pid47051`: `3`
- `wineboot.exe@pid47130`: `3`
- `winemenubuilder.exe@pid47156`: `3`
- `services.exe@pid47161`: `3`
- `winedevice.exe@pid47348`: `3`
- `plugplay.exe@pid47185`: `3`
- `svchost.exe@pid47261`: `3`
- `explorer@pid47647`: `3`
- `explorer.exe@pid47855`: `3`
- `winehua_keep.exe@pid47879`: `3`
- `rpcss.exe@pid47923`: `3`
- `winehua_d3d_switch_cube.exe@pid53953`: `3`
- `winehua_d3d12_smoke.exe@pid49136`: `3`
- `winehua_graphics_smoke.exe@pid53155`: `3`

### `BOX64_DYNAREC_CALLRET`

- `wine@pid47065`: `2`
- `wineserver@pid47051`: `2`
- `wineboot.exe@pid47130`: `2`
- `winemenubuilder.exe@pid47156`: `2`
- `services.exe@pid47161`: `2`
- `winedevice.exe@pid47348`: `2`
- `plugplay.exe@pid47185`: `2`
- `svchost.exe@pid47261`: `2`
- `explorer@pid47647`: `2`
- `explorer.exe@pid47855`: `2`
- `winehua_keep.exe@pid47879`: `2`
- `rpcss.exe@pid47923`: `2`
- `winehua_d3d_switch_cube.exe@pid53953`: `2`
- `winehua_d3d12_smoke.exe@pid49136`: `2`
- `winehua_graphics_smoke.exe@pid53155`: `2`

### `BOX64_DYNAREC_FORWARD`

- `wine@pid47065`: `1024`
- `wineserver@pid47051`: `1024`
- `wineboot.exe@pid47130`: `1024`
- `winemenubuilder.exe@pid47156`: `1024`
- `services.exe@pid47161`: `1024`
- `winedevice.exe@pid47348`: `1024`
- `plugplay.exe@pid47185`: `1024`
- `svchost.exe@pid47261`: `1024`
- `explorer@pid47647`: `1024`
- `explorer.exe@pid47855`: `1024`
- `winehua_keep.exe@pid47879`: `1024`
- `rpcss.exe@pid47923`: `1024`
- `winehua_d3d_switch_cube.exe@pid53953`: `1024`
- `winehua_d3d12_smoke.exe@pid49136`: `1024`
- `winehua_graphics_smoke.exe@pid53155`: `1024`

### `BOX64_DYNAREC_SAFEFLAGS`

- `wine@pid47065`: `1`
- `wineserver@pid47051`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `BOX64_DYNAREC_VOLATILE_METADATA`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_DYNAREC_WEAKBARRIER`

- `wine@pid47065`: `2`
- `wineserver@pid47051`: `2`
- `wineboot.exe@pid47130`: `2`
- `winemenubuilder.exe@pid47156`: `2`
- `services.exe@pid47161`: `2`
- `winedevice.exe@pid47348`: `2`
- `plugplay.exe@pid47185`: `2`
- `svchost.exe@pid47261`: `2`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `2`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_EMULATED_LIBS`

- `explorer@pid47647`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`
- `explorer.exe@pid47855`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`
- `winehua_keep.exe@pid47879`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`
- `winehua_d3d_switch_cube.exe@pid53953`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`
- `winehua_d3d12_smoke.exe@pid49136`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`
- `winehua_graphics_smoke.exe@pid53155`: `libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayl`

### `BOX64_LD_LIBRARY_PATH`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `wineserver@pid47051`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/e`

### `BOX64_LOG`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_NOBANNER`

- `wine@pid47065`: `1`
- `wineserver@pid47051`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `BOX64_PCLMULQDQ`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `BOX64_SHOWSEGV`

- `wine@pid47065`: `1`
- `wineserver@pid47051`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `BOX64_SYSINFO_CACHED`

- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`

### `BOX64_SYSINFO_CPUNAME`

- `wineboot.exe@pid47130`: `Unknown CPU`
- `winemenubuilder.exe@pid47156`: `Unknown CPU`
- `services.exe@pid47161`: `Unknown CPU`
- `winedevice.exe@pid47348`: `Unknown CPU`
- `plugplay.exe@pid47185`: `Unknown CPU`
- `svchost.exe@pid47261`: `Unknown CPU`
- `explorer.exe@pid47855`: `Unknown CPU`
- `winehua_keep.exe@pid47879`: `Unknown CPU`
- `rpcss.exe@pid47923`: `Unknown CPU`

### `BOX64_SYSINFO_FREQUENCY`

- `wineboot.exe@pid47130`: `2400000000`
- `winemenubuilder.exe@pid47156`: `2400000000`
- `services.exe@pid47161`: `2400000000`
- `winedevice.exe@pid47348`: `2400000000`
- `plugplay.exe@pid47185`: `2400000000`
- `svchost.exe@pid47261`: `2400000000`
- `explorer.exe@pid47855`: `2400000000`
- `winehua_keep.exe@pid47879`: `2400000000`
- `rpcss.exe@pid47923`: `2400000000`

### `BOX64_SYSINFO_NCPU`

- `wineboot.exe@pid47130`: `12`
- `winemenubuilder.exe@pid47156`: `12`
- `services.exe@pid47161`: `12`
- `winedevice.exe@pid47348`: `12`
- `plugplay.exe@pid47185`: `12`
- `svchost.exe@pid47261`: `12`
- `explorer.exe@pid47855`: `12`
- `winehua_keep.exe@pid47879`: `12`
- `rpcss.exe@pid47923`: `12`

### `DOWNLOAD_CACHE`

- `wine@pid47065`: `/data/cache`
- `wineserver@pid47051`: `/data/cache`
- `wineboot.exe@pid47130`: `/data/cache`
- `winemenubuilder.exe@pid47156`: `/data/cache`
- `services.exe@pid47161`: `/data/cache`
- `winedevice.exe@pid47348`: `/data/cache`
- `plugplay.exe@pid47185`: `/data/cache`
- `svchost.exe@pid47261`: `/data/cache`
- `explorer@pid47647`: `/data/cache`
- `explorer.exe@pid47855`: `/data/cache`
- `winehua_keep.exe@pid47879`: `/data/cache`
- `rpcss.exe@pid47923`: `/data/cache`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/cache`
- `virgl-ipc-proc@pid46992`: `/data/cache`
- `virgl-ipc-main@pid46992`: `/data/cache`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/cache`
- `winehua_graphics_smoke.exe@pid53155`: `/data/cache`

### `DXVK_LOG_LEVEL`

- `explorer@pid47647`: `warn`
- `explorer.exe@pid47855`: `warn`
- `winehua_keep.exe@pid47879`: `warn`

### `DXVK_LOG_PATH`

- `explorer@pid47647`: `C:\windows\temp`
- `explorer.exe@pid47855`: `C:\windows\temp`
- `winehua_keep.exe@pid47879`: `C:\windows\temp`

### `DXVK_WINEHUA_BATCH_MAPPED_FLUSH`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`

### `DXVK_WINEHUA_COMMAND_QUERY_RESET`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`

### `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT`

- `explorer@pid47647`: `auto`
- `explorer.exe@pid47855`: `auto`
- `winehua_keep.exe@pid47879`: `auto`

### `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`

### `DXVK_WINEHUA_PRECISE_SHADOW`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`

### `EGL_PLATFORM`

- `explorer@pid47647`: `wayland`
- `explorer.exe@pid47855`: `wayland`
- `winehua_keep.exe@pid47879`: `wayland`
- `winehua_d3d_switch_cube.exe@pid53953`: `wayland`
- `virgl-ipc-main@pid46992`: `surfaceless`
- `winehua_d3d12_smoke.exe@pid49136`: `wayland`
- `winehua_graphics_smoke.exe@pid53155`: `wayland`

### `GALLIUM_DRIVER`

- `explorer@pid47647`: `virpipe`
- `explorer.exe@pid47855`: `virpipe`
- `winehua_keep.exe@pid47879`: `virpipe`
- `winehua_d3d_switch_cube.exe@pid53953`: `virpipe`
- `winehua_d3d12_smoke.exe@pid49136`: `virpipe`
- `winehua_graphics_smoke.exe@pid53155`: `virpipe`

### `GST_PLUGIN_PATH`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`

### `GST_PLUGIN_SYSTEM_PATH`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`

### `HAP_DEBUGGABLE`

- `wine@pid47065`: `true`
- `wineserver@pid47051`: `true`
- `wineboot.exe@pid47130`: `true`
- `winemenubuilder.exe@pid47156`: `true`
- `services.exe@pid47161`: `true`
- `winedevice.exe@pid47348`: `true`
- `plugplay.exe@pid47185`: `true`
- `svchost.exe@pid47261`: `true`
- `explorer@pid47647`: `true`
- `explorer.exe@pid47855`: `true`
- `winehua_keep.exe@pid47879`: `true`
- `rpcss.exe@pid47923`: `true`
- `winehua_d3d_switch_cube.exe@pid53953`: `true`
- `virgl-ipc-proc@pid46992`: `true`
- `virgl-ipc-main@pid46992`: `true`
- `winehua_d3d12_smoke.exe@pid49136`: `true`
- `winehua_graphics_smoke.exe@pid53155`: `true`

### `HNP_PRIVATE_HOME`

- `wine@pid47065`: `/data/app`
- `wineserver@pid47051`: `/data/app`
- `wineboot.exe@pid47130`: `/data/app`
- `winemenubuilder.exe@pid47156`: `/data/app`
- `services.exe@pid47161`: `/data/app`
- `winedevice.exe@pid47348`: `/data/app`
- `plugplay.exe@pid47185`: `/data/app`
- `svchost.exe@pid47261`: `/data/app`
- `explorer@pid47647`: `/data/app`
- `explorer.exe@pid47855`: `/data/app`
- `winehua_keep.exe@pid47879`: `/data/app`
- `rpcss.exe@pid47923`: `/data/app`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/app`
- `virgl-ipc-proc@pid46992`: `/data/app`
- `virgl-ipc-main@pid46992`: `/data/app`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/app`
- `winehua_graphics_smoke.exe@pid53155`: `/data/app`

### `HNP_PUBLIC_HOME`

- `wine@pid47065`: `/data/service/hnp`
- `wineserver@pid47051`: `/data/service/hnp`
- `wineboot.exe@pid47130`: `/data/service/hnp`
- `winemenubuilder.exe@pid47156`: `/data/service/hnp`
- `services.exe@pid47161`: `/data/service/hnp`
- `winedevice.exe@pid47348`: `/data/service/hnp`
- `plugplay.exe@pid47185`: `/data/service/hnp`
- `svchost.exe@pid47261`: `/data/service/hnp`
- `explorer@pid47647`: `/data/service/hnp`
- `explorer.exe@pid47855`: `/data/service/hnp`
- `winehua_keep.exe@pid47879`: `/data/service/hnp`
- `rpcss.exe@pid47923`: `/data/service/hnp`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/service/hnp`
- `virgl-ipc-proc@pid46992`: `/data/service/hnp`
- `virgl-ipc-main@pid46992`: `/data/service/hnp`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/service/hnp`
- `winehua_graphics_smoke.exe@pid53155`: `/data/service/hnp`

### `HOME`

- `wine@pid47065`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `wineserver@pid47051`: `/storage/Users/currentUser`
- `wineboot.exe@pid47130`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `winemenubuilder.exe@pid47156`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `services.exe@pid47161`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `winedevice.exe@pid47348`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `plugplay.exe@pid47185`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `svchost.exe@pid47261`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `explorer@pid47647`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `explorer.exe@pid47855`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `winehua_keep.exe@pid47879`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `rpcss.exe@pid47923`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `winehua_d3d_switch_cube.exe@pid53953`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `virgl-ipc-proc@pid46992`: `/storage/Users/currentUser`
- `virgl-ipc-main@pid46992`: `/storage/Users/currentUser`
- `winehua_d3d12_smoke.exe@pid49136`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`
- `winehua_graphics_smoke.exe@pid53155`: `/storage/Users/currentUser/Download/app.hackeris.winehua/`

### `LANG`

- `wine@pid47065`: `zh_CN.UTF-8`
- `wineserver@pid47051`: `en_US.UTF-8`
- `wineboot.exe@pid47130`: `zh_CN.UTF-8`
- `winemenubuilder.exe@pid47156`: `zh_CN.UTF-8`
- `services.exe@pid47161`: `zh_CN.UTF-8`
- `winedevice.exe@pid47348`: `zh_CN.UTF-8`
- `plugplay.exe@pid47185`: `zh_CN.UTF-8`
- `svchost.exe@pid47261`: `zh_CN.UTF-8`
- `explorer@pid47647`: `zh_CN.UTF-8`
- `explorer.exe@pid47855`: `zh_CN.UTF-8`
- `winehua_keep.exe@pid47879`: `zh_CN.UTF-8`
- `rpcss.exe@pid47923`: `zh_CN.UTF-8`
- `winehua_d3d_switch_cube.exe@pid53953`: `zh_CN.UTF-8`
- `virgl-ipc-proc@pid46992`: `en_US.UTF-8`
- `virgl-ipc-main@pid46992`: `en_US.UTF-8`
- `winehua_d3d12_smoke.exe@pid49136`: `zh_CN.UTF-8`
- `winehua_graphics_smoke.exe@pid53155`: `zh_CN.UTF-8`

### `LC_ALL`

- `wine@pid47065`: `zh_CN.UTF-8`
- `wineboot.exe@pid47130`: `zh_CN.UTF-8`
- `winemenubuilder.exe@pid47156`: `zh_CN.UTF-8`
- `services.exe@pid47161`: `zh_CN.UTF-8`
- `winedevice.exe@pid47348`: `zh_CN.UTF-8`
- `plugplay.exe@pid47185`: `zh_CN.UTF-8`
- `svchost.exe@pid47261`: `zh_CN.UTF-8`
- `explorer@pid47647`: `zh_CN.UTF-8`
- `explorer.exe@pid47855`: `zh_CN.UTF-8`
- `winehua_keep.exe@pid47879`: `zh_CN.UTF-8`
- `rpcss.exe@pid47923`: `zh_CN.UTF-8`
- `winehua_d3d_switch_cube.exe@pid53953`: `zh_CN.UTF-8`
- `winehua_d3d12_smoke.exe@pid49136`: `zh_CN.UTF-8`
- `winehua_graphics_smoke.exe@pid53155`: `zh_CN.UTF-8`

### `LD_LIBRARY_PATH`

- `wine@pid47065`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `wineboot.exe@pid47130`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `winemenubuilder.exe@pid47156`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `services.exe@pid47161`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `winedevice.exe@pid47348`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `plugplay.exe@pid47185`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `svchost.exe@pid47261`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `rpcss.exe@pid47923`: `/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`

### `LD_PRELOAD`

- `wine@pid47065`: `libappspawn_helper.z.so`
- `wineserver@pid47051`: `libappspawn_helper.z.so`
- `wineboot.exe@pid47130`: `libappspawn_helper.z.so`
- `winemenubuilder.exe@pid47156`: `libappspawn_helper.z.so`
- `services.exe@pid47161`: `libappspawn_helper.z.so`
- `winedevice.exe@pid47348`: `libappspawn_helper.z.so`
- `plugplay.exe@pid47185`: `libappspawn_helper.z.so`
- `svchost.exe@pid47261`: `libappspawn_helper.z.so`
- `explorer@pid47647`: `libappspawn_helper.z.so`
- `explorer.exe@pid47855`: `libappspawn_helper.z.so`
- `winehua_keep.exe@pid47879`: `libappspawn_helper.z.so`
- `rpcss.exe@pid47923`: `libappspawn_helper.z.so`
- `winehua_d3d_switch_cube.exe@pid53953`: `libappspawn_helper.z.so`
- `winehua_d3d12_smoke.exe@pid49136`: `libappspawn_helper.z.so`
- `winehua_graphics_smoke.exe@pid53155`: `libappspawn_helper.z.so`

### `LIBGL_ALWAYS_SOFTWARE`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `LIBGL_DRIVERS_PATH`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`

### `MALI_REPORT_MEM_USAGE`

- `wine@pid47065`: `1`
- `wineserver@pid47051`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `virgl-ipc-proc@pid46992`: `1`
- `virgl-ipc-main@pid46992`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `MANGOHUD_CONFIG`

- `wineboot.exe@pid47130`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `winemenubuilder.exe@pid47156`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `services.exe@pid47161`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `winedevice.exe@pid47348`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `plugplay.exe@pid47185`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `svchost.exe@pid47261`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `explorer.exe@pid47855`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `winehua_keep.exe@pid47879`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
- `rpcss.exe@pid47923`: `legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`

### `MESA_LOADER_DRIVER_OVERRIDE`

- `explorer@pid47647`: `swrast`
- `explorer.exe@pid47855`: `swrast`
- `winehua_keep.exe@pid47879`: `swrast`
- `winehua_d3d_switch_cube.exe@pid53953`: `swrast`
- `winehua_d3d12_smoke.exe@pid49136`: `swrast`
- `winehua_graphics_smoke.exe@pid53155`: `swrast`

### `MIDI_SOUNDFONT_PATH`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`

### `OHOS_SOCKET_AppSpawn`

- `virgl-ipc-proc@pid46992`: `14`
- `virgl-ipc-main@pid46992`: `14`

### `OHOS_SOCKET_NativeSpawn`

- `wine@pid47065`: `18`
- `wineserver@pid47051`: `18`
- `wineboot.exe@pid47130`: `18`
- `winemenubuilder.exe@pid47156`: `18`
- `services.exe@pid47161`: `18`
- `winedevice.exe@pid47348`: `18`
- `plugplay.exe@pid47185`: `18`
- `svchost.exe@pid47261`: `18`
- `explorer@pid47647`: `18`
- `explorer.exe@pid47855`: `18`
- `winehua_keep.exe@pid47879`: `18`
- `rpcss.exe@pid47923`: `18`
- `winehua_d3d_switch_cube.exe@pid53953`: `18`
- `winehua_d3d12_smoke.exe@pid49136`: `18`
- `winehua_graphics_smoke.exe@pid53155`: `18`

### `OLDPWD`

- `wine@pid47065`: `/`
- `wineserver@pid47051`: `/`
- `wineboot.exe@pid47130`: `/`
- `winemenubuilder.exe@pid47156`: `/`
- `services.exe@pid47161`: `/`
- `winedevice.exe@pid47348`: `/`
- `plugplay.exe@pid47185`: `/`
- `svchost.exe@pid47261`: `/`
- `explorer@pid47647`: `/`
- `explorer.exe@pid47855`: `/`
- `winehua_keep.exe@pid47879`: `/`
- `rpcss.exe@pid47923`: `/`
- `winehua_d3d_switch_cube.exe@pid53953`: `/`
- `virgl-ipc-proc@pid46992`: `/`
- `virgl-ipc-main@pid46992`: `/`
- `winehua_d3d12_smoke.exe@pid49136`: `/`
- `winehua_graphics_smoke.exe@pid53155`: `/`

### `PATH`

- `wine@pid47065`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `wineserver@pid47051`: `/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `wineboot.exe@pid47130`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `winemenubuilder.exe@pid47156`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `services.exe@pid47161`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `winedevice.exe@pid47348`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `plugplay.exe@pid47185`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `svchost.exe@pid47261`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `explorer@pid47647`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `explorer.exe@pid47855`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `winehua_keep.exe@pid47879`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `rpcss.exe@pid47923`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `winehua_d3d_switch_cube.exe@pid53953`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `virgl-ipc-proc@pid46992`: `/data/app/bin:/data/service/hnp/bin:/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `virgl-ipc-main@pid46992`: `/data/app/bin:/data/service/hnp/bin:/data/app/bin:/data/service/hnp/bin:/usr/local/bin:/bin:/usr/bin:/system/bin:/vendor/bin`
- `winehua_d3d12_smoke.exe@pid49136`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`
- `winehua_graphics_smoke.exe@pid53155`: `/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/`

### `PROCESSBROKER`

- `wine@pid47065`: `/data/storage/el2/base/files/.wine_broker`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/.wine_broker`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/.wine_broker`
- `services.exe@pid47161`: `/data/storage/el2/base/files/.wine_broker`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/.wine_broker`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/.wine_broker`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/.wine_broker`
- `explorer@pid47647`: `/data/storage/el2/base/files/.wine_broker`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/.wine_broker`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/.wine_broker`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/.wine_broker`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine_broker`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/.wine_broker`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine_broker`

### `PROCESS_START_TIME`

- `wine@pid47065`: `84708821`
- `wineserver@pid47051`: `84708789`
- `wineboot.exe@pid47130`: `84708821`
- `winemenubuilder.exe@pid47156`: `84708821`
- `services.exe@pid47161`: `84708821`
- `winedevice.exe@pid47348`: `84708821`
- `plugplay.exe@pid47185`: `84708821`
- `svchost.exe@pid47261`: `84708821`
- `explorer@pid47647`: `84713379`
- `explorer.exe@pid47855`: `84713379`
- `winehua_keep.exe@pid47879`: `84713379`
- `rpcss.exe@pid47923`: `84708821`
- `winehua_d3d_switch_cube.exe@pid53953`: `84785567`
- `virgl-ipc-proc@pid46992`: `84708556`
- `virgl-ipc-main@pid46992`: `84708556`
- `winehua_d3d12_smoke.exe@pid49136`: `84730757`
- `winehua_graphics_smoke.exe@pid53155`: `84779917`

### `PULSE_RUNTIME_PATH`

- `wine@pid47065`: `/data/data/.pulse_dir/runtime`
- `wineserver@pid47051`: `/data/data/.pulse_dir/runtime`
- `wineboot.exe@pid47130`: `/data/data/.pulse_dir/runtime`
- `winemenubuilder.exe@pid47156`: `/data/data/.pulse_dir/runtime`
- `services.exe@pid47161`: `/data/data/.pulse_dir/runtime`
- `winedevice.exe@pid47348`: `/data/data/.pulse_dir/runtime`
- `plugplay.exe@pid47185`: `/data/data/.pulse_dir/runtime`
- `svchost.exe@pid47261`: `/data/data/.pulse_dir/runtime`
- `explorer@pid47647`: `/data/data/.pulse_dir/runtime`
- `explorer.exe@pid47855`: `/data/data/.pulse_dir/runtime`
- `winehua_keep.exe@pid47879`: `/data/data/.pulse_dir/runtime`
- `rpcss.exe@pid47923`: `/data/data/.pulse_dir/runtime`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/data/.pulse_dir/runtime`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/data/.pulse_dir/runtime`
- `winehua_graphics_smoke.exe@pid53155`: `/data/data/.pulse_dir/runtime`

### `PULSE_STATE_PATH`

- `wine@pid47065`: `/data/data/.pulse_dir/state`
- `wineserver@pid47051`: `/data/data/.pulse_dir/state`
- `wineboot.exe@pid47130`: `/data/data/.pulse_dir/state`
- `winemenubuilder.exe@pid47156`: `/data/data/.pulse_dir/state`
- `services.exe@pid47161`: `/data/data/.pulse_dir/state`
- `winedevice.exe@pid47348`: `/data/data/.pulse_dir/state`
- `plugplay.exe@pid47185`: `/data/data/.pulse_dir/state`
- `svchost.exe@pid47261`: `/data/data/.pulse_dir/state`
- `explorer@pid47647`: `/data/data/.pulse_dir/state`
- `explorer.exe@pid47855`: `/data/data/.pulse_dir/state`
- `winehua_keep.exe@pid47879`: `/data/data/.pulse_dir/state`
- `rpcss.exe@pid47923`: `/data/data/.pulse_dir/state`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/data/.pulse_dir/state`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/data/.pulse_dir/state`
- `winehua_graphics_smoke.exe@pid53155`: `/data/data/.pulse_dir/state`

### `PWD`

- `wine@pid47065`: `/storage/Users/currentUser`
- `wineserver@pid47051`: `/storage/Users/currentUser`
- `wineboot.exe@pid47130`: `/storage/Users/currentUser`
- `winemenubuilder.exe@pid47156`: `/storage/Users/currentUser`
- `services.exe@pid47161`: `/storage/Users/currentUser`
- `winedevice.exe@pid47348`: `/storage/Users/currentUser`
- `plugplay.exe@pid47185`: `/storage/Users/currentUser`
- `svchost.exe@pid47261`: `/storage/Users/currentUser`
- `explorer@pid47647`: `/storage/Users/currentUser`
- `explorer.exe@pid47855`: `/storage/Users/currentUser`
- `winehua_keep.exe@pid47879`: `/storage/Users/currentUser`
- `rpcss.exe@pid47923`: `/storage/Users/currentUser`
- `winehua_d3d_switch_cube.exe@pid53953`: `/storage/Users/currentUser`
- `virgl-ipc-proc@pid46992`: `/storage/Users/currentUser`
- `virgl-ipc-main@pid46992`: `/storage/Users/currentUser`
- `winehua_d3d12_smoke.exe@pid49136`: `/storage/Users/currentUser`
- `winehua_graphics_smoke.exe@pid53155`: `/storage/Users/currentUser`

### `SHELL`

- `wine@pid47065`: `/bin/sh`
- `wineserver@pid47051`: `/bin/sh`
- `wineboot.exe@pid47130`: `/bin/sh`
- `winemenubuilder.exe@pid47156`: `/bin/sh`
- `services.exe@pid47161`: `/bin/sh`
- `winedevice.exe@pid47348`: `/bin/sh`
- `plugplay.exe@pid47185`: `/bin/sh`
- `svchost.exe@pid47261`: `/bin/sh`
- `explorer@pid47647`: `/bin/sh`
- `explorer.exe@pid47855`: `/bin/sh`
- `winehua_keep.exe@pid47879`: `/bin/sh`
- `rpcss.exe@pid47923`: `/bin/sh`
- `winehua_d3d_switch_cube.exe@pid53953`: `/bin/sh`
- `virgl-ipc-proc@pid46992`: `/bin/sh`
- `virgl-ipc-main@pid46992`: `/bin/sh`
- `winehua_d3d12_smoke.exe@pid49136`: `/bin/sh`
- `winehua_graphics_smoke.exe@pid53155`: `/bin/sh`

### `SHLVL`

- `wine@pid47065`: `1`
- `wineserver@pid47051`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `virgl-ipc-proc@pid46992`: `1`
- `virgl-ipc-main@pid46992`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `TERM`

- `wine@pid47065`: `ansi`
- `wineserver@pid47051`: `ansi`
- `wineboot.exe@pid47130`: `ansi`
- `winemenubuilder.exe@pid47156`: `ansi`
- `services.exe@pid47161`: `ansi`
- `winedevice.exe@pid47348`: `ansi`
- `plugplay.exe@pid47185`: `ansi`
- `svchost.exe@pid47261`: `ansi`
- `explorer@pid47647`: `ansi`
- `explorer.exe@pid47855`: `ansi`
- `winehua_keep.exe@pid47879`: `ansi`
- `rpcss.exe@pid47923`: `ansi`
- `winehua_d3d_switch_cube.exe@pid53953`: `ansi`
- `virgl-ipc-proc@pid46992`: `ansi`
- `virgl-ipc-main@pid46992`: `ansi`
- `winehua_d3d12_smoke.exe@pid49136`: `ansi`
- `winehua_graphics_smoke.exe@pid53155`: `ansi`

### `TMP`

- `wine@pid47065`: `/data/local/mtp_tmp/`
- `wineserver@pid47051`: `/data/local/mtp_tmp/`
- `wineboot.exe@pid47130`: `/data/local/mtp_tmp/`
- `winemenubuilder.exe@pid47156`: `/data/local/mtp_tmp/`
- `services.exe@pid47161`: `/data/local/mtp_tmp/`
- `winedevice.exe@pid47348`: `/data/local/mtp_tmp/`
- `plugplay.exe@pid47185`: `/data/local/mtp_tmp/`
- `svchost.exe@pid47261`: `/data/local/mtp_tmp/`
- `explorer@pid47647`: `/data/local/mtp_tmp/`
- `explorer.exe@pid47855`: `/data/local/mtp_tmp/`
- `winehua_keep.exe@pid47879`: `/data/local/mtp_tmp/`
- `rpcss.exe@pid47923`: `/data/local/mtp_tmp/`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/local/mtp_tmp/`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/local/mtp_tmp/`
- `winehua_graphics_smoke.exe@pid53155`: `/data/local/mtp_tmp/`

### `TMPDIR`

- `wine@pid47065`: `/data/storage/el2/base/cache`
- `wineserver@pid47051`: `/data/storage/el2/base/cache`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/cache`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/cache`
- `services.exe@pid47161`: `/data/storage/el2/base/cache`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/cache`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/cache`
- `svchost.exe@pid47261`: `/data/storage/el2/base/cache`
- `explorer@pid47647`: `/data/storage/el2/base/cache`
- `explorer.exe@pid47855`: `/data/storage/el2/base/cache`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/cache`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/cache`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/cache`
- `virgl-ipc-proc@pid46992`: `/data/storage/el2/base/cache`
- `virgl-ipc-main@pid46992`: `/data/storage/el2/base/cache`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/cache`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/cache`

### `UBSAN_OPTIONS`

- `wine@pid47065`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `wineserver@pid47051`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `wineboot.exe@pid47130`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winemenubuilder.exe@pid47156`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `services.exe@pid47161`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winedevice.exe@pid47348`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `plugplay.exe@pid47185`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `svchost.exe@pid47261`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `explorer@pid47647`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `explorer.exe@pid47855`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winehua_keep.exe@pid47879`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `rpcss.exe@pid47923`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winehua_d3d_switch_cube.exe@pid53953`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `virgl-ipc-proc@pid46992`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `virgl-ipc-main@pid46992`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winehua_d3d12_smoke.exe@pid49136`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`
- `winehua_graphics_smoke.exe@pid53155`: `print_stacktrace=1:print_module_map=2:log_exe_name=1`

### `USER`

- `wine@pid47065`: `100`
- `wineserver@pid47051`: `100`
- `wineboot.exe@pid47130`: `100`
- `winemenubuilder.exe@pid47156`: `100`
- `services.exe@pid47161`: `100`
- `winedevice.exe@pid47348`: `100`
- `plugplay.exe@pid47185`: `100`
- `svchost.exe@pid47261`: `100`
- `explorer@pid47647`: `100`
- `explorer.exe@pid47855`: `100`
- `winehua_keep.exe@pid47879`: `100`
- `rpcss.exe@pid47923`: `100`
- `winehua_d3d_switch_cube.exe@pid53953`: `100`
- `virgl-ipc-proc@pid46992`: `100`
- `virgl-ipc-main@pid46992`: `100`
- `winehua_d3d12_smoke.exe@pid49136`: `100`
- `winehua_graphics_smoke.exe@pid53155`: `100`

### `USE_LIBBOX64`

- `wine@pid47065`: `1`
- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VKR_WINEHUA_SHADOW_FROM_HOST`

- `explorer@pid47647`: `precise`
- `explorer.exe@pid47855`: `precise`
- `winehua_keep.exe@pid47879`: `precise`
- `winehua_d3d_switch_cube.exe@pid53953`: `precise`
- `winehua_d3d12_smoke.exe@pid49136`: `precise`
- `winehua_graphics_smoke.exe@pid53155`: `precise`

### `VK_DRIVER_FILES`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`

### `VK_ICD_FILENAMES`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`

### `VN_DEBUG`

- `explorer@pid47647`: `vtest`
- `explorer.exe@pid47855`: `vtest`
- `winehua_keep.exe@pid47879`: `vtest`
- `winehua_d3d_switch_cube.exe@pid53953`: `vtest`
- `winehua_d3d12_smoke.exe@pid49136`: `vtest`
- `winehua_graphics_smoke.exe@pid53155`: `vtest`

### `VN_PERF`

- `explorer@pid47647`: `no_fence_feedback,no_query_feedback,no_multi_ring`
- `explorer.exe@pid47855`: `no_fence_feedback,no_query_feedback,no_multi_ring`
- `winehua_keep.exe@pid47879`: `no_fence_feedback,no_query_feedback,no_multi_ring`
- `winehua_d3d_switch_cube.exe@pid53953`: `no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
- `winehua_d3d12_smoke.exe@pid49136`: `no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
- `winehua_graphics_smoke.exe@pid53155`: `no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`

### `VN_WINEHUA_DIRECT_FENCE_WAIT`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VN_WINEHUA_PERSISTENT_MAP_SYNC`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VN_WINEHUA_PRESENT_TRACE`

- `winehua_d3d12_smoke.exe@pid49136`: `1`

### `VN_WINEHUA_REMOTE_MEMORY_SYNC`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VN_WINEHUA_STRONG_RING_BARRIER`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `VTEST_SOCKET_NAME`

- `explorer@pid47647`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`

### `WAYLAND_DISPLAY`

- `wine@pid47065`: `wine-wayland`
- `wineboot.exe@pid47130`: `wine-wayland`
- `winemenubuilder.exe@pid47156`: `wine-wayland`
- `services.exe@pid47161`: `wine-wayland`
- `winedevice.exe@pid47348`: `wine-wayland`
- `plugplay.exe@pid47185`: `wine-wayland`
- `svchost.exe@pid47261`: `wine-wayland`
- `explorer@pid47647`: `wine-wayland`
- `explorer.exe@pid47855`: `wine-wayland`
- `winehua_keep.exe@pid47879`: `wine-wayland`
- `rpcss.exe@pid47923`: `wine-wayland`
- `winehua_d3d_switch_cube.exe@pid53953`: `wine-wayland`
- `winehua_d3d12_smoke.exe@pid49136`: `wine-wayland`
- `winehua_graphics_smoke.exe@pid53155`: `wine-wayland`

### `WINEBINDIR`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin`

### `WINEDATADIR`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/../share/wine`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/../share/wine`

### `WINEDEBUG`

- `wine@pid47065`: `-all`
- `wineserver@pid47051`: `-all`
- `wineboot.exe@pid47130`: `-all`
- `winemenubuilder.exe@pid47156`: `-all`
- `services.exe@pid47161`: `-all`
- `winedevice.exe@pid47348`: `-all`
- `plugplay.exe@pid47185`: `-all`
- `svchost.exe@pid47261`: `-all`
- `explorer@pid47647`: `-all`
- `explorer.exe@pid47855`: `-all`
- `winehua_keep.exe@pid47879`: `-all`
- `rpcss.exe@pid47923`: `-all`
- `winehua_d3d_switch_cube.exe@pid53953`: `-all`
- `winehua_d3d12_smoke.exe@pid49136`: `-all`
- `winehua_graphics_smoke.exe@pid53155`: `-all`

### `WINEDLLDIR`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/x86_64-unix`

### `WINEDLLDIR0`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`

### `WINEDLLDIR1`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/dxvk/legacy/x64`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`

### `WINEDLLDIR2`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/dxvk/legacy/x86`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`

### `WINEDLLDIR3`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/x86_64-windows`

### `WINEDLLDIR4`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/i386-windows`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/i386-windows`

### `WINEDLLDIR5`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin`

### `WINEDLLOVERRIDES`

- `explorer@pid47647`: `d3d12=n;d3d11=n;dxgi=n`
- `explorer.exe@pid47855`: `d3d12=n;d3d11=n;dxgi=n`
- `winehua_keep.exe@pid47879`: `d3d12=n;d3d11=n;dxgi=n`
- `winehua_d3d_switch_cube.exe@pid53953`: `d3d12=n;d3d11=n;dxgi=n`
- `winehua_d3d12_smoke.exe@pid49136`: `d3d12=n;d3d11=n;dxgi=n`
- `winehua_graphics_smoke.exe@pid53155`: `d3d12=n;d3d11=n;dxgi=n`

### `WINEDLLPATH`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/da`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/da`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/da`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6`

### `WINEHUA_AUTOMATION`

- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `WINEHUA_D3D_BACKEND`

- `explorer@pid47647`: `vkd3d_limited_500k`
- `explorer.exe@pid47855`: `vkd3d_limited_500k`
- `winehua_keep.exe@pid47879`: `vkd3d_limited_500k`
- `winehua_d3d_switch_cube.exe@pid53953`: `vkd3d_limited_500k`
- `winehua_d3d12_smoke.exe@pid49136`: `vkd3d_limited_500k`
- `winehua_graphics_smoke.exe@pid53155`: `vkd3d_limited_500k`

### `WINEHUA_DESKTOP`

- `winehua_d3d_switch_cube.exe@pid53953`: `shell`
- `winehua_d3d12_smoke.exe@pid49136`: `shell`
- `winehua_graphics_smoke.exe@pid53155`: `shell`

### `WINEHUA_DESKTOP_MODE`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_DISPLAY_FPS_FILE`

- `explorer@pid47647`: `C:\windows\temp\winehua_display_fps.txt`
- `explorer.exe@pid47855`: `C:\windows\temp\winehua_display_fps.txt`
- `winehua_keep.exe@pid47879`: `C:\windows\temp\winehua_display_fps.txt`
- `winehua_d3d_switch_cube.exe@pid53953`: `C:\windows\temp\winehua_display_fps.txt`
- `winehua_d3d12_smoke.exe@pid49136`: `C:\windows\temp\winehua_display_fps.txt`
- `winehua_graphics_smoke.exe@pid53155`: `C:\windows\temp\winehua_display_fps.txt`

### `WINEHUA_DXVK_PROFILE`

- `explorer@pid47647`: `legacy`
- `explorer.exe@pid47855`: `legacy`
- `winehua_keep.exe@pid47879`: `legacy`
- `winehua_d3d_switch_cube.exe@pid53953`: `modern-2.6`
- `winehua_d3d12_smoke.exe@pid49136`: `modern-2.6`
- `winehua_graphics_smoke.exe@pid53155`: `modern-2.6`

### `WINEHUA_DXVK_RELAXED_FEATURES`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`

### `WINEHUA_DXVK_ROOT`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/dxvk/legacy`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/dxvk/legacy`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/dxvk/legacy`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/dxvk/modern-2.6`

### `WINEHUA_DXVK_TRACE_PRESENT_IMAGE`

- `winehua_d3d12_smoke.exe@pid49136`: `1`

### `WINEHUA_DXVK_VERSION`

- `explorer@pid47647`: `1.10.3`
- `explorer.exe@pid47855`: `1.10.3`
- `winehua_keep.exe@pid47879`: `1.10.3`
- `winehua_d3d_switch_cube.exe@pid53953`: `2.6.2`
- `winehua_d3d12_smoke.exe@pid49136`: `2.6.2`
- `winehua_graphics_smoke.exe@pid53155`: `2.6.2`

### `WINEHUA_EGL_LIBRARY_PATH`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`

### `WINEHUA_FRAME_TRANSPORT`

- `explorer@pid47647`: `virgl_texture+surface_queue+external_oes`
- `explorer.exe@pid47855`: `virgl_texture+surface_queue+external_oes`
- `winehua_keep.exe@pid47879`: `virgl_texture+surface_queue+external_oes`
- `winehua_d3d_switch_cube.exe@pid53953`: `virgl_texture+surface_queue+external_oes`
- `winehua_d3d12_smoke.exe@pid49136`: `virgl_texture+surface_queue+external_oes`
- `winehua_graphics_smoke.exe@pid53155`: `virgl_texture+surface_queue+external_oes`

### `WINEHUA_FRAME_ZERO_COPY`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_GL_STALL_DIAG`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_GRAPHICS_ACTIVE`

- `explorer@pid47647`: `virgl`
- `explorer.exe@pid47855`: `virgl`
- `winehua_keep.exe@pid47879`: `virgl`
- `winehua_d3d_switch_cube.exe@pid53953`: `virgl`
- `winehua_d3d12_smoke.exe@pid49136`: `virgl`
- `winehua_graphics_smoke.exe@pid53155`: `virgl`

### `WINEHUA_GRAPHICS_BACKEND`

- `explorer@pid47647`: `virgl`
- `explorer.exe@pid47855`: `virgl`
- `winehua_keep.exe@pid47879`: `virgl`
- `winehua_d3d_switch_cube.exe@pid53953`: `virgl`
- `winehua_d3d12_smoke.exe@pid49136`: `virgl`
- `winehua_graphics_smoke.exe@pid53155`: `virgl`

### `WINEHUA_GUEST_GFX_DIR`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/guest_gfx`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/guest_gfx`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/guest_gfx`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/guest_gfx`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/guest_gfx`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/guest_gfx`

### `WINEHUA_GUEST_GFX_MODE`

- `explorer@pid47647`: `mesa-virpipe`
- `explorer.exe@pid47855`: `mesa-virpipe`
- `winehua_keep.exe@pid47879`: `mesa-virpipe`
- `winehua_d3d_switch_cube.exe@pid53953`: `mesa-virpipe`
- `winehua_d3d12_smoke.exe@pid49136`: `mesa-virpipe`
- `winehua_graphics_smoke.exe@pid53155`: `mesa-virpipe`

### `WINEHUA_GUEST_GFX_PLATFORM`

- `explorer@pid47647`: `wayland`
- `explorer.exe@pid47855`: `wayland`
- `winehua_keep.exe@pid47879`: `wayland`
- `winehua_d3d_switch_cube.exe@pid53953`: `wayland`
- `winehua_d3d12_smoke.exe@pid49136`: `wayland`
- `winehua_graphics_smoke.exe@pid53155`: `wayland`

### `WINEHUA_GUEST_GFX_READY`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_HOST_ARCH`

- `winehua_d3d_switch_cube.exe@pid53953`: `aarch64`
- `winehua_d3d12_smoke.exe@pid49136`: `aarch64`
- `winehua_graphics_smoke.exe@pid53155`: `aarch64`

### `WINEHUA_PERF_PROFILE`

- `explorer@pid47647`: `shadow-precise`
- `explorer.exe@pid47855`: `shadow-precise`
- `winehua_keep.exe@pid47879`: `shadow-precise`
- `winehua_d3d_switch_cube.exe@pid53953`: `shadow-precise`
- `winehua_d3d12_smoke.exe@pid49136`: `shadow-precise`
- `winehua_graphics_smoke.exe@pid53155`: `shadow-precise`

### `WINEHUA_PRESENT_BACKEND`

- `winehua_d3d_switch_cube.exe@pid53953`: `venus_broker_present`
- `winehua_d3d12_smoke.exe@pid49136`: `venus_broker_present`
- `winehua_graphics_smoke.exe@pid53155`: `venus_broker_present`

### `WINEHUA_SHM_FALLBACK`

- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `WINEHUA_VENUS_ICD_ARCH`

- `explorer@pid47647`: `x86_64`
- `explorer.exe@pid47855`: `x86_64`
- `winehua_keep.exe@pid47879`: `x86_64`
- `winehua_d3d_switch_cube.exe@pid53953`: `x86_64`
- `winehua_d3d12_smoke.exe@pid49136`: `x86_64`
- `winehua_graphics_smoke.exe@pid53155`: `x86_64`

### `WINEHUA_VIRGLRENDERER_LIB`

- `explorer@pid47647`: `libvirglrenderer.so`
- `explorer.exe@pid47855`: `libvirglrenderer.so`
- `winehua_keep.exe@pid47879`: `libvirglrenderer.so`
- `winehua_d3d_switch_cube.exe@pid53953`: `libvirglrenderer.so`
- `winehua_d3d12_smoke.exe@pid49136`: `libvirglrenderer.so`
- `winehua_graphics_smoke.exe@pid53155`: `libvirglrenderer.so`

### `WINEHUA_VIRGL_LIBRARY_READY`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_VIRGL_READY`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_VIRGL_SOCKET`

- `explorer@pid47647`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine/graphics/virgl.sock`

### `WINEHUA_VIRGL_SOCKET_READY`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_VKD3D_PROFILE`

- `explorer@pid47647`: `limited-500k`
- `explorer.exe@pid47855`: `limited-500k`
- `winehua_keep.exe@pid47879`: `limited-500k`
- `winehua_d3d_switch_cube.exe@pid53953`: `limited-500k`
- `winehua_d3d12_smoke.exe@pid49136`: `limited-500k`
- `winehua_graphics_smoke.exe@pid53155`: `limited-500k`

### `WINEHUA_VKD3D_ROOT`

- `explorer@pid47647`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/vkd3d/limited-500k`

### `WINEHUA_VKD3D_VERSION`

- `explorer@pid47647`: `2.6`
- `explorer.exe@pid47855`: `2.6`
- `winehua_keep.exe@pid47879`: `2.6`
- `winehua_d3d_switch_cube.exe@pid53953`: `2.6`
- `winehua_d3d12_smoke.exe@pid49136`: `2.6`
- `winehua_graphics_smoke.exe@pid53155`: `2.6`

### `WINEHUA_VTEST_FRONTBUFFER_LOG`

- `explorer@pid47647`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `explorer.exe@pid47855`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`

### `WINEHUA_VTEST_PRESENT`

- `explorer@pid47647`: `surface-queue`
- `explorer.exe@pid47855`: `surface-queue`
- `winehua_keep.exe@pid47879`: `surface-queue`
- `winehua_d3d_switch_cube.exe@pid53953`: `surface-queue`
- `winehua_d3d12_smoke.exe@pid49136`: `surface-queue`
- `winehua_graphics_smoke.exe@pid53155`: `surface-queue`

### `WINEHUA_VULKAN_LOADER_ARCH`

- `explorer@pid47647`: `x86_64`
- `explorer.exe@pid47855`: `x86_64`
- `winehua_keep.exe@pid47879`: `x86_64`
- `winehua_d3d_switch_cube.exe@pid53953`: `x86_64`
- `winehua_d3d12_smoke.exe@pid49136`: `x86_64`
- `winehua_graphics_smoke.exe@pid53155`: `x86_64`

### `WINEHUA_VULKAN_PRESENT`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_VULKAN_RUNTIME`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_WAYLAND_READBACK`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINEHUA_WINE_UNIX_ARCH`

- `winehua_d3d_switch_cube.exe@pid53953`: `x86_64`
- `winehua_d3d12_smoke.exe@pid49136`: `x86_64`
- `winehua_graphics_smoke.exe@pid53155`: `x86_64`

### `WINEHUA_WORKING_DIRECTORY`

- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine/drive_c/smoke/x64`
- `winehua_d3d12_smoke.exe@pid49136`: `C:\smoke\x64`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine/drive_c/smoke/x64`

### `WINEHUA_ZERO_COPY_READY_DIR`

- `explorer@pid47647`: `/data/storage/el2/base/cache`
- `explorer.exe@pid47855`: `/data/storage/el2/base/cache`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/cache`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/cache`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/cache`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/cache`

### `WINEPREFIX`

- `wine@pid47065`: `/data/storage/el2/base/files/.wine`
- `wineserver@pid47051`: `/data/storage/el2/base/files/.wine`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/.wine`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/.wine`
- `services.exe@pid47161`: `/data/storage/el2/base/files/.wine`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/.wine`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/.wine`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/.wine`
- `explorer@pid47647`: `/data/storage/el2/base/files/.wine`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/.wine`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/.wine`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/.wine`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/.wine`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine`

### `WINESERVERSOCKET`

- `wineboot.exe@pid47130`: `16`
- `winemenubuilder.exe@pid47156`: `20`
- `services.exe@pid47161`: `21`
- `winedevice.exe@pid47348`: `24`
- `plugplay.exe@pid47185`: `23`
- `svchost.exe@pid47261`: `23`
- `explorer.exe@pid47855`: `26`
- `winehua_keep.exe@pid47879`: `27`
- `rpcss.exe@pid47923`: `28`

### `WINEUNIXDIR`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin`

### `WINEWAYLAND_ENTER_SILENT`

- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINE_OHOS_AUDIO_BOOTSTRAP_FD`

- `wineboot.exe@pid47130`: `15`
- `winemenubuilder.exe@pid47156`: `16`
- `services.exe@pid47161`: `20`
- `winedevice.exe@pid47348`: `23`
- `plugplay.exe@pid47185`: `22`
- `svchost.exe@pid47261`: `16`
- `explorer@pid47647`: `15`
- `explorer.exe@pid47855`: `25`
- `winehua_keep.exe@pid47879`: `26`
- `rpcss.exe@pid47923`: `27`
- `winehua_d3d_switch_cube.exe@pid53953`: `28`
- `winehua_d3d12_smoke.exe@pid49136`: `25`
- `winehua_graphics_smoke.exe@pid53155`: `25`

### `WINE_OHOS_AUDIO_ENABLE`

- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `WINE_OHOS_AUDIO_PROTOCOL_VERSION`

- `wineboot.exe@pid47130`: `1`
- `winemenubuilder.exe@pid47156`: `1`
- `services.exe@pid47161`: `1`
- `winedevice.exe@pid47348`: `1`
- `plugplay.exe@pid47185`: `1`
- `svchost.exe@pid47261`: `1`
- `explorer@pid47647`: `1`
- `explorer.exe@pid47855`: `1`
- `winehua_keep.exe@pid47879`: `1`
- `rpcss.exe@pid47923`: `1`
- `winehua_d3d_switch_cube.exe@pid53953`: `1`
- `winehua_d3d12_smoke.exe@pid49136`: `1`
- `winehua_graphics_smoke.exe@pid53155`: `1`

### `XDG_RUNTIME_DIR`

- `wine@pid47065`: `/data/storage/el2/base/files/.wine`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/.wine`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/.wine`
- `services.exe@pid47161`: `/data/storage/el2/base/files/.wine`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/.wine`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/.wine`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/.wine`
- `explorer@pid47647`: `/data/storage/el2/base/files/.wine`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/.wine`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/.wine`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/.wine`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/.wine`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/.wine`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/.wine`

### `XKB_CONFIG_ROOT`

- `wine@pid47065`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `wineboot.exe@pid47130`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winemenubuilder.exe@pid47156`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `services.exe@pid47161`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winedevice.exe@pid47348`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `plugplay.exe@pid47185`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `svchost.exe@pid47261`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `explorer@pid47647`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `explorer.exe@pid47855`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winehua_keep.exe@pid47879`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `rpcss.exe@pid47923`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winehua_d3d_switch_cube.exe@pid53953`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winehua_d3d12_smoke.exe@pid49136`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
- `winehua_graphics_smoke.exe@pid53155`: `/data/storage/el2/base/files/wine/bin/../share/X11/xkb`

### `__LIBACE_ENTRY_POINT`

- `virgl-ipc-proc@pid46992`: `5be0bd5590`
- `virgl-ipc-main@pid46992`: `5be0bd5590`

### `hwasanEnabled`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `virgl-ipc-proc@pid46992`: `0`
- `virgl-ipc-main@pid46992`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `tsanEnabled`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `virgl-ipc-proc@pid46992`: `0`
- `virgl-ipc-main@pid46992`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

### `ubsanEnabled`

- `wine@pid47065`: `0`
- `wineserver@pid47051`: `0`
- `wineboot.exe@pid47130`: `0`
- `winemenubuilder.exe@pid47156`: `0`
- `services.exe@pid47161`: `0`
- `winedevice.exe@pid47348`: `0`
- `plugplay.exe@pid47185`: `0`
- `svchost.exe@pid47261`: `0`
- `explorer@pid47647`: `0`
- `explorer.exe@pid47855`: `0`
- `winehua_keep.exe@pid47879`: `0`
- `rpcss.exe@pid47923`: `0`
- `winehua_d3d_switch_cube.exe@pid53953`: `0`
- `virgl-ipc-proc@pid46992`: `0`
- `virgl-ipc-main@pid46992`: `0`
- `winehua_d3d12_smoke.exe@pid49136`: `0`
- `winehua_graphics_smoke.exe@pid53155`: `0`

## 附录 B — 图形相关环境变量 (Graphics 类)

- `DXVK_LOG_LEVEL` = `warn`  (出现 3 次)
- `DXVK_LOG_PATH` = `C:\windows\temp`  (出现 3 次)
- `EGL_PLATFORM` = `surfaceless` | `wayland`  (出现 7 次)
- `LIBGL_ALWAYS_SOFTWARE` = `1`  (出现 6 次)
- `LIBGL_DRIVERS_PATH` = `/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`  (出现 6 次)
- `MESA_LOADER_DRIVER_OVERRIDE` = `swrast`  (出现 6 次)
- `VK_DRIVER_FILES` = `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`  (出现 6 次)
- `VK_ICD_FILENAMES` = `/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`  (出现 6 次)
- `VN_DEBUG` = `vtest`  (出现 6 次)
- `VN_PERF` = `no_fence_feedback,no_query_feedback,no_multi_ring` | `no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`  (出现 6 次)
- `VTEST_SOCKET_NAME` = `/data/storage/el2/base/files/.wine/graphics/virgl.sock`  (出现 6 次)
- `WAYLAND_DISPLAY` = `wine-wayland`  (出现 14 次)

## 附录 C — 音频相关环境变量 (Audio 类)

- `MIDI_SOUNDFONT_PATH` = `/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`  (出现 14 次)
- `PULSE_RUNTIME_PATH` = `/data/data/.pulse_dir/runtime`  (出现 15 次)
- `PULSE_STATE_PATH` = `/data/data/.pulse_dir/state`  (出现 15 次)

---

## 补充分析：同一程序两条启动路径的差异（2026-08-30 实测）

### 场景

同一程序 `winehua_d3d_switch_cube.exe` 连续启动两次（argv 完全一致），但**帧率差异显著**（第 1 次高，第 2 次低）：

| | 第 1 次 pid=32079 | 第 2 次 pid=33509 |
|---|---|---|
| 启动路径 | **应用库直启**（App 即时注入 env） | **文件管理/explorer 双击**（wineserver 会话内创建） |
| 帧率 | 高 | 低 |

### 关键环境变量差异

| 变量 | 第 1 次（应用库直启） | 第 2 次（文件管理双击） | 性质 |
|---|---|---|---|
| `WINEHUA_DXVK_PROFILE` | `modern-2.6` | `legacy` | ★ 主因 |
| `WINEHUA_DXVK_VERSION` | `2.6.2` | `1.10.3` | ★ 主因 |
| `DXVK_WINEHUA_BATCH_MAPPED_FLUSH` 等 5 项兼容开关 | 无 | 全有（`FLUSH_DYNAMIC_MAPPED`/`COMMAND_QUERY_RESET`/…） | ★ legacy 慢路径 |
| `VN_PERF` | `…no_semaphore_feedback,no_multi_ring` | `…no_multi_ring`（缺 semaphore feedback) | 次 |
| `WINEHUA_PRESENT_BACKEND` / `WORKING_DIRECTORY` / `DESKTOP` | 有（App 注入上下文） | 无 | 标记 |
| `WINESERVERSOCKET` + `APPSPAWN_FD_wineserver_sock` | 无 | 有（fd 传递） | 标记 |
| `MANGOHUD_CONFIG` | 无 | 有（box64 `hookMangoHud` 注入，值=`Box64 arm64 v0.4.3 c2f0f7a`） | 待观察 |
| `BOX64_SYSINFO_NCPU/CPUNAME/FREQUENCY` | 无 | 有（会话级系统信息缓存） | 旁注 |

### 根因

文件管理双击的进程由 **wineserver 会话链**（`explorer` 及其全家）创建，环境为**引擎启动时的注入**——当前引擎会话的 DXVK 档位为 **legacy（DXVK 1.10.3）** + legacy 专用兼容开关组（强制 flush/query reset 等慢路径补偿）；而应用库直启由 **App 启动侧即时注入 `dxvk_modern_2_6`（DXVK 2.6.2）**，框架级优化（ring/upload/shadow 新路径）全开。

快照佐证继承链：`explorer`(pid 21567) `DXVK=legacy` → 会话内全部进程 `DXVK=legacy`，而 App 直启 pid 32079 `DXVK=modern-2.6`。

**性能损失贡献（估计排序）**：
1. DXVK 1.10.3 vs 2.6.2 本身的渲染/上传优化差距（主）；
2. legacy 兼容开关组（`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`、`FLUSH_DYNAMIC_MAPPED=1` 等强制同步路径）（次）；
3. `VN_PERF` 缺 `no_semaphore_feedback`、`MANGOHUD_CONFIG` 注入（影响未确认，待观察）。

### 对照实验确认（2026-08-30 19:32）

将全局 DXVK 档位切到 **legacy（1.10.3）** 后再从**应用库直启** `winehua_d3d_switch_cube`（pid 62819）：
快照确认 `DXVK_PROFILE=legacy` + 兼容开关组（`BATCH_MAPPED_FLUSH`/`COMMAND_QUERY_RESET`/`FLUSH_DYNAMIC_MAPPED`/`EMULATE_RGBA8_SNORM_RT`/`RELAXED_FEATURES`），**帧率同样低**。

结论：
- **帧率低的根源 = DXVK legacy 档（1.10.3 + 5 件强制同步兼容开关组），与启动路径无关**；
- 路径差异、`MANGOHUD_CONFIG`、`BOX64_SYSINFO_*`、`VN_PERF`(semaphore feedback) 均**排除**为性能主因（62819 无 MANGOHUD/SYSINFO 且帧率仍低）；
- 机理: 1.10.3 适配不了 venus 影子内存新上传/ring 机制，必需补偿开关（每帧上传强制 flush 串行化）; 2.6.2 原生适配 VKR 新批量上传路径无需补偿。

### 修复候选（未实施）

全局 DXVK 档位选择（用户设置持久化的 `winehua.dxvk.preference`）目前**只在 App 直启路径即时生效**，引擎会话/文件管理路径按引擎启动时基线段（默认 legacy）——建议把全局档位**贯通引擎会话注入**（`setup_wine_env`/引擎注入点读取同一 preferences），使"从文件管理启动"也吃全局设置。
## 附录 E — 高/低帧率场景完整环境变量（全量快照）

> 为后续性能分析保留的原始全量 env（非差异表）。三个样本均为 `winehua_d3d_switch_cube.exe`：
> - **高帧率** pid 32079：应用库直启 + DXVK modern-2.6 (2.6.2)
> - **低帧率** pid 33509：文件管理(explorer)双击 + DXVK legacy (1.10.3) + 兼容开关组
> - **低帧率** pid 62819：应用库直启 + 全局档位切 legacy (1.10.3)（对照实验）；与 33509 仅差 `DXVK_WINEHUA_PRECISE_SHADOW` 等少量 ArkTS 注入项

### 高帧率样本（一） — `winehua_d3d_switch_cube.exe` pid `32079`

> 应用库直启 · DXVK 2.6.2 · 帧率高

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe
```

`APPSPAWN_FD_wine_audio_bootstrap=28`
`AppSpawnCheckUnexpectedExitCall=32079`
`BOX64_AES=0`
`BOX64_AVX=0`
`BOX64_DYNAREC_BIGBLOCK=3`
`BOX64_DYNAREC_CALLRET=2`
`BOX64_DYNAREC_FORWARD=1024`
`BOX64_DYNAREC_SAFEFLAGS=1`
`BOX64_DYNAREC_VOLATILE_METADATA=0`
`BOX64_DYNAREC_WEAKBARRIER=0`
`BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
`BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
`BOX64_LOG=0`
`BOX64_NOBANNER=1`
`BOX64_PCLMULQDQ=0`
`BOX64_SHOWSEGV=1`
`DOWNLOAD_CACHE=/data/cache`
`EGL_PLATFORM=wayland`
`GALLIUM_DRIVER=virpipe`
`GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`HAP_DEBUGGABLE=true`
`HNP_PRIVATE_HOME=/data/app`
`HNP_PUBLIC_HOME=/data/service/hnp`
`HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
`LANG=zh_CN.UTF-8`
`LC_ALL=zh_CN.UTF-8`
`LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
`LD_PRELOAD=libappspawn_helper.z.so`
`LIBGL_ALWAYS_SOFTWARE=1`
`LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
`MALI_REPORT_MEM_USAGE=1`
`MESA_LOADER_DRIVER_OVERRIDE=swrast`
`MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
`OHOS_SOCKET_NativeSpawn=18`
`OLDPWD=/`
`PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
`PROCESS_START_TIME=85269572`
`PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
`PULSE_STATE_PATH=/data/data/.pulse_dir/state`
`PWD=/storage/Users/currentUser`
`SHELL=/bin/sh`
`SHLVL=1`
`TERM=ansi`
`TMP=/data/local/mtp_tmp/`
`TMPDIR=/data/storage/el2/base/cache`
`UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
`USER=100`
`USE_LIBBOX64=1`
`VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
`VKR_WINEHUA_SHADOW_FROM_HOST=precise`
`VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VN_DEBUG=vtest`
`VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring`
`VN_WINEHUA_DIRECT_FENCE_WAIT=1`
`VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
`VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
`VN_WINEHUA_STRONG_RING_BARRIER=1`
`VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WAYLAND_DISPLAY=wine-wayland`
`WINEBINDIR=/data/storage/el2/base/files/wine/bin`
`WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
`WINEDEBUG=-all`
`WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
`WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
`WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64`
`WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86`
`WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
`WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
`WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
`WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
`WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x64:/data/storage/el2/base/files/wine/dxvk/modern-2.6/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`WINEHUA_AUTOMATION=0`
`WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
`WINEHUA_DESKTOP=shell`
`WINEHUA_DESKTOP_MODE=1`
`WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
`WINEHUA_DXVK_PROFILE=modern-2.6`
`WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/modern-2.6`
`WINEHUA_DXVK_VERSION=2.6.2`
`WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
`WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
`WINEHUA_FRAME_ZERO_COPY=1`
`WINEHUA_GL_STALL_DIAG=1`
`WINEHUA_GRAPHICS_ACTIVE=virgl`
`WINEHUA_GRAPHICS_BACKEND=virgl`
`WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
`WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
`WINEHUA_GUEST_GFX_PLATFORM=wayland`
`WINEHUA_GUEST_GFX_READY=1`
`WINEHUA_HOST_ARCH=aarch64`
`WINEHUA_PERF_PROFILE=shadow-precise`
`WINEHUA_PRESENT_BACKEND=venus_broker_present`
`WINEHUA_SHM_FALLBACK=0`
`WINEHUA_VENUS_ICD_ARCH=x86_64`
`WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
`WINEHUA_VIRGL_LIBRARY_READY=1`
`WINEHUA_VIRGL_READY=1`
`WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WINEHUA_VIRGL_SOCKET_READY=1`
`WINEHUA_VKD3D_PROFILE=limited-500k`
`WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
`WINEHUA_VKD3D_VERSION=2.6`
`WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
`WINEHUA_VTEST_PRESENT=surface-queue`
`WINEHUA_VULKAN_LOADER_ARCH=x86_64`
`WINEHUA_VULKAN_PRESENT=1`
`WINEHUA_VULKAN_RUNTIME=1`
`WINEHUA_WAYLAND_READBACK=1`
`WINEHUA_WINE_UNIX_ARCH=x86_64`
`WINEHUA_WORKING_DIRECTORY=/data/storage/el2/base/files/.wine/drive_c/smoke/x64`
`WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
`WINEPREFIX=/data/storage/el2/base/files/.wine`
`WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
`WINEWAYLAND_ENTER_SILENT=1`
`WINE_OHOS_AUDIO_BOOTSTRAP_FD=28`
`WINE_OHOS_AUDIO_ENABLE=1`
`WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
`XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
`XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
`hwasanEnabled=0`
`tsanEnabled=0`
`ubsanEnabled=0`

### 低帧率样本（二） — `winehua_d3d_switch_cube.exe` pid `33509`

> 文件管理双击 · DXVK 1.10.3 + 兼容开关组 · 帧率低

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe
```

`APPSPAWN_FD_wine_audio_bootstrap=15`
`APPSPAWN_FD_wineserver_sock=24`
`AppSpawnCheckUnexpectedExitCall=21567`
`BOX64_AES=0`
`BOX64_AVX=0`
`BOX64_DYNAREC_BIGBLOCK=3`
`BOX64_DYNAREC_CALLRET=2`
`BOX64_DYNAREC_FORWARD=1024`
`BOX64_DYNAREC_SAFEFLAGS=1`
`BOX64_DYNAREC_VOLATILE_METADATA=0`
`BOX64_DYNAREC_WEAKBARRIER=0`
`BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
`BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
`BOX64_LOG=0`
`BOX64_NOBANNER=1`
`BOX64_PCLMULQDQ=0`
`BOX64_SHOWSEGV=1`
`BOX64_SYSINFO_CACHED=1`
`BOX64_SYSINFO_CPUNAME=Unknown CPU`
`BOX64_SYSINFO_FREQUENCY=2400000000`
`BOX64_SYSINFO_NCPU=12`
`DOWNLOAD_CACHE=/data/cache`
`DXVK_LOG_LEVEL=warn`
`DXVK_LOG_PATH=C:\windows\temp`
`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
`DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
`DXVK_WINEHUA_PRECISE_SHADOW=1`
`EGL_PLATFORM=wayland`
`GALLIUM_DRIVER=virpipe`
`GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`HAP_DEBUGGABLE=true`
`HNP_PRIVATE_HOME=/data/app`
`HNP_PUBLIC_HOME=/data/service/hnp`
`HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
`LANG=zh_CN.UTF-8`
`LC_ALL=zh_CN.UTF-8`
`LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
`LD_PRELOAD=libappspawn_helper.z.so`
`LIBGL_ALWAYS_SOFTWARE=1`
`LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
`MALI_REPORT_MEM_USAGE=1`
`MANGOHUD_CONFIG=legacy_layout=0,custom_text_center=Box64 arm64 v0.4.3 c2f0f7a,gpu_stats=1,cpu_stats=1,fps=1,frame_timing=1`
`MESA_LOADER_DRIVER_OVERRIDE=swrast`
`MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
`OHOS_SOCKET_NativeSpawn=18`
`OLDPWD=/`
`PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
`PROCESS_START_TIME=85147486`
`PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
`PULSE_STATE_PATH=/data/data/.pulse_dir/state`
`PWD=/storage/Users/currentUser`
`SHELL=/bin/sh`
`SHLVL=1`
`TERM=ansi`
`TMP=/data/local/mtp_tmp/`
`TMPDIR=/data/storage/el2/base/cache`
`UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
`USER=100`
`USE_LIBBOX64=1`
`VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
`VKR_WINEHUA_SHADOW_FROM_HOST=precise`
`VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VN_DEBUG=vtest`
`VN_PERF=no_fence_feedback,no_query_feedback,no_multi_ring`
`VN_WINEHUA_DIRECT_FENCE_WAIT=1`
`VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
`VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
`VN_WINEHUA_STRONG_RING_BARRIER=1`
`VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WAYLAND_DISPLAY=wine-wayland`
`WINEBINDIR=/data/storage/el2/base/files/wine/bin`
`WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
`WINEDEBUG=-all`
`WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
`WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
`WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
`WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
`WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
`WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
`WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
`WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
`WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
`WINEHUA_DESKTOP_MODE=1`
`WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
`WINEHUA_DXVK_PROFILE=legacy`
`WINEHUA_DXVK_RELAXED_FEATURES=1`
`WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
`WINEHUA_DXVK_VERSION=1.10.3`
`WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
`WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
`WINEHUA_FRAME_ZERO_COPY=1`
`WINEHUA_GL_STALL_DIAG=1`
`WINEHUA_GRAPHICS_ACTIVE=virgl`
`WINEHUA_GRAPHICS_BACKEND=virgl`
`WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
`WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
`WINEHUA_GUEST_GFX_PLATFORM=wayland`
`WINEHUA_GUEST_GFX_READY=1`
`WINEHUA_PERF_PROFILE=shadow-precise`
`WINEHUA_SHM_FALLBACK=0`
`WINEHUA_VENUS_ICD_ARCH=x86_64`
`WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
`WINEHUA_VIRGL_LIBRARY_READY=1`
`WINEHUA_VIRGL_READY=1`
`WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WINEHUA_VIRGL_SOCKET_READY=1`
`WINEHUA_VKD3D_PROFILE=limited-500k`
`WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
`WINEHUA_VKD3D_VERSION=2.6`
`WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
`WINEHUA_VTEST_PRESENT=surface-queue`
`WINEHUA_VULKAN_LOADER_ARCH=x86_64`
`WINEHUA_VULKAN_PRESENT=1`
`WINEHUA_VULKAN_RUNTIME=1`
`WINEHUA_WAYLAND_READBACK=1`
`WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
`WINEPREFIX=/data/storage/el2/base/files/.wine`
`WINESERVERSOCKET=29`
`WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
`WINEWAYLAND_ENTER_SILENT=1`
`WINE_OHOS_AUDIO_BOOTSTRAP_FD=28`
`WINE_OHOS_AUDIO_ENABLE=1`
`WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
`XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
`XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
`hwasanEnabled=0`
`tsanEnabled=0`
`ubsanEnabled=0`

### 低帧率样本（三） — `winehua_d3d_switch_cube.exe` pid `62819`

> 应用库直启 + legacy 档 · DXVK 1.10.3 + 兼容开关组 · 帧率低（对照实验确认路径无关）

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe
```

`APPSPAWN_FD_wine_audio_bootstrap=28`
`AppSpawnCheckUnexpectedExitCall=62819`
`BOX64_AES=0`
`BOX64_AVX=0`
`BOX64_DYNAREC_BIGBLOCK=3`
`BOX64_DYNAREC_CALLRET=2`
`BOX64_DYNAREC_FORWARD=1024`
`BOX64_DYNAREC_SAFEFLAGS=1`
`BOX64_DYNAREC_VOLATILE_METADATA=0`
`BOX64_DYNAREC_WEAKBARRIER=0`
`BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
`BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
`BOX64_LOG=0`
`BOX64_NOBANNER=1`
`BOX64_PCLMULQDQ=0`
`BOX64_SHOWSEGV=1`
`DOWNLOAD_CACHE=/data/cache`
`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
`DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
`EGL_PLATFORM=wayland`
`GALLIUM_DRIVER=virpipe`
`GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`HAP_DEBUGGABLE=true`
`HNP_PRIVATE_HOME=/data/app`
`HNP_PUBLIC_HOME=/data/service/hnp`
`HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
`LANG=zh_CN.UTF-8`
`LC_ALL=zh_CN.UTF-8`
`LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
`LD_PRELOAD=libappspawn_helper.z.so`
`LIBGL_ALWAYS_SOFTWARE=1`
`LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
`MALI_REPORT_MEM_USAGE=1`
`MESA_LOADER_DRIVER_OVERRIDE=swrast`
`MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
`OHOS_SOCKET_NativeSpawn=18`
`OLDPWD=/`
`PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`PROCESSBROKER=/data/storage/el2/base/files/.wine_broker`
`PROCESS_START_TIME=85732437`
`PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
`PULSE_STATE_PATH=/data/data/.pulse_dir/state`
`PWD=/storage/Users/currentUser`
`SHELL=/bin/sh`
`SHLVL=1`
`TERM=ansi`
`TMP=/data/local/mtp_tmp/`
`TMPDIR=/data/storage/el2/base/cache`
`UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
`USER=100`
`USE_LIBBOX64=1`
`VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1`
`VKR_WINEHUA_SHADOW_FROM_HOST=precise`
`VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VN_DEBUG=vtest`
`VN_PERF=no_fence_feedback,no_query_feedback,no_multi_ring`
`VN_WINEHUA_DIRECT_FENCE_WAIT=1`
`VN_WINEHUA_PERSISTENT_MAP_SYNC=1`
`VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
`VN_WINEHUA_STRONG_RING_BARRIER=1`
`VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WAYLAND_DISPLAY=wine-wayland`
`WINEBINDIR=/data/storage/el2/base/files/wine/bin`
`WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
`WINEDEBUG=-all`
`WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
`WINEDLLDIR0=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64`
`WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
`WINEDLLDIR2=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
`WINEDLLDIR3=/data/storage/el2/base/files/wine/bin/x86_64-windows`
`WINEDLLDIR4=/data/storage/el2/base/files/wine/bin/i386-windows`
`WINEDLLDIR5=/data/storage/el2/base/files/wine/bin`
`WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n`
`WINEDLLPATH=/data/storage/el2/base/files/wine/vkd3d/limited-500k/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`WINEHUA_AUTOMATION=0`
`WINEHUA_D3D_BACKEND=vkd3d_limited_500k`
`WINEHUA_DESKTOP=shell`
`WINEHUA_DESKTOP_MODE=1`
`WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
`WINEHUA_DXVK_PROFILE=legacy`
`WINEHUA_DXVK_RELAXED_FEATURES=1`
`WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
`WINEHUA_DXVK_VERSION=1.10.3`
`WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
`WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
`WINEHUA_FRAME_ZERO_COPY=1`
`WINEHUA_GL_STALL_DIAG=1`
`WINEHUA_GRAPHICS_ACTIVE=virgl`
`WINEHUA_GRAPHICS_BACKEND=virgl`
`WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
`WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
`WINEHUA_GUEST_GFX_PLATFORM=wayland`
`WINEHUA_GUEST_GFX_READY=1`
`WINEHUA_HOST_ARCH=aarch64`
`WINEHUA_PERF_PROFILE=shadow-precise`
`WINEHUA_PRESENT_BACKEND=venus_broker_present`
`WINEHUA_SHM_FALLBACK=0`
`WINEHUA_VENUS_ICD_ARCH=x86_64`
`WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
`WINEHUA_VIRGL_LIBRARY_READY=1`
`WINEHUA_VIRGL_READY=1`
`WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WINEHUA_VIRGL_SOCKET_READY=1`
`WINEHUA_VKD3D_PROFILE=limited-500k`
`WINEHUA_VKD3D_ROOT=/data/storage/el2/base/files/wine/vkd3d/limited-500k`
`WINEHUA_VKD3D_VERSION=2.6`
`WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
`WINEHUA_VTEST_PRESENT=surface-queue`
`WINEHUA_VULKAN_LOADER_ARCH=x86_64`
`WINEHUA_VULKAN_PRESENT=1`
`WINEHUA_VULKAN_RUNTIME=1`
`WINEHUA_WAYLAND_READBACK=1`
`WINEHUA_WINE_UNIX_ARCH=x86_64`
`WINEHUA_WORKING_DIRECTORY=/data/storage/el2/base/files/.wine/drive_c/smoke/x64`
`WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
`WINEPREFIX=/data/storage/el2/base/files/.wine`
`WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
`WINEWAYLAND_ENTER_SILENT=1`
`WINE_OHOS_AUDIO_BOOTSTRAP_FD=28`
`WINE_OHOS_AUDIO_ENABLE=1`
`WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`
`XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
`XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
`hwasanEnabled=0`
`tsanEnabled=0`
`ubsanEnabled=0`

> 完整时序原始记录：`.temp/proc_baseline/proc_snapshot.log`
