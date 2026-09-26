/* winehua_t_audio_waveout — waveOut API 语义（P5，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.21。
 * 失败特征：Open 断=音频设备枚举/fd 引导链断（audio bootstrap fd 的 guest
 * 侧观测面）；GetPosition 恒 0=宿主进度回传断（无声/卡顿类定性）。
 * guest 侧 API 语义面；宿主混音/渲染链由既有 audio 套件守，不重复。
 */
#include "../common/winehua_t_check.h"
#include <mmsystem.h>
#include <math.h>

#define RATE 44100
#define CHUNK_MS 500
#define SAMPLES (RATE * CHUNK_MS / 1000)

static short g_pcm[SAMPLES];

static void fill_sine(void)
{
    int i;
    for (i = 0; i < SAMPLES; ++i)
    {
        double v = sin(2.0 * 3.141592653589793 * 440.0 * i / RATE);
        g_pcm[i] = (short)(v * 16000.0);
    }
}

int main(int argc, char **argv)
{
    UINT devs;
    HWAVEOUT out = NULL;
    WAVEFORMATEX fmt;
    WAVEHDR hdr;
    MMRESULT res;
    DWORD pos0, pos1, vol_saved = 0;
    int vol_roundtrip;

    t_begin("winehua_t_audio_waveout", argc, argv);

    devs = waveOutGetNumDevs();
    t_check("device-present", devs >= 1, "devs=%u", (unsigned)devs);
    if (devs < 1)
        return t_finish();

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = RATE * fmt.nBlockAlign;
    res = waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    t_check("waveout-open", res == MMSYSERR_NOERROR && out != NULL,
            "res=%u", (unsigned)res);
    if (res != MMSYSERR_NOERROR || !out)
        return t_finish();

    /* 音量往返；设备不支持时跳过（GET 失败即无判据，不算失败） */
    if (waveOutGetVolume(out, &vol_saved) == MMSYSERR_NOERROR)
    {
        vol_roundtrip = waveOutSetVolume(out, vol_saved) == MMSYSERR_NOERROR;
        t_check("volume-roundtrip", vol_roundtrip == 1,
                "set rc=%d", vol_roundtrip);
    }
    else
    {
        t_check("volume-roundtrip", 1, "unsupported (skipped)");
    }

    fill_sine();
    memset(&hdr, 0, sizeof(hdr));
    hdr.lpData = (LPSTR)g_pcm;
    hdr.dwBufferLength = sizeof(g_pcm);
    res = waveOutPrepareHeader(out, &hdr, sizeof(hdr));
    t_check("prepare-header", res == MMSYSERR_NOERROR,
            "res=%u", (unsigned)res);

    res = waveOutWrite(out, &hdr, sizeof(hdr));
    t_check("waveout-write", res == MMSYSERR_NOERROR, "res=%u", (unsigned)res);

    /* 播放进度回传：0.5s 素材，等 ~250ms 后位置应单调前进且为 ms 量级 */
    Sleep(50);
    pos0 = 0;
    waveOutGetPosition(out, &pos0, sizeof(pos0));
    Sleep(200);
    pos1 = 0;
    waveOutGetPosition(out, &pos1, sizeof(pos1));
    t_check("position-advances", pos1 > pos0,
            "pos %lu -> %lu ms", (unsigned long)pos0, (unsigned long)pos1);
    t_check("position-ms-scale", pos1 >= 100 && pos1 <= 2000,
            "pos=%lu ms after ~250ms of %dms audio",
            (unsigned long)pos1, CHUNK_MS);
    t_metric("waveout-pos-ms", "%lu", (unsigned long)pos1);

    /* 收尾：等播完（WHDR_DONE）→ Unprepare → Reset → Close */
    {
        int wait;
        for (wait = 0; wait < 20 && !(hdr.dwFlags & WHDR_DONE); ++wait)
            Sleep(50);
        t_check("buffer-done", (hdr.dwFlags & WHDR_DONE) != 0,
                "flags=%lx", (unsigned long)hdr.dwFlags);
    }
    res = waveOutUnprepareHeader(out, &hdr, sizeof(hdr));
    t_check("unprepare-header", res == MMSYSERR_NOERROR,
            "res=%u", (unsigned)res);
    res = waveOutClose(out);
    t_check("waveout-close", res == MMSYSERR_NOERROR, "res=%u", (unsigned)res);

    return t_finish();
}
