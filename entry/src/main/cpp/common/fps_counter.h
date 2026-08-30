#pragma once
#include <chrono>
#include <atomic>

#undef LOG_TAG
#define LOG_TAG "WL_FPS"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#include <hilog/log.h>

// 轻量 FPS 计数器，每 10 秒输出一次帧率
class FpsCounter {
public:
    explicit FpsCounter(const char* tag) : tag_(tag) {
        last_ = std::chrono::steady_clock::now();
    }
    void Tick() {
        ++frames_;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count();
        if (ms >= 10000) {
            double fps = frames_ * 1000.0 / ms;
            OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}.2f fps", tag_, fps);
            frames_ = 0;
            last_ = now;
        }
    }
private:
    const char* tag_;
    std::chrono::steady_clock::time_point last_;
    int frames_ = 0;
};
