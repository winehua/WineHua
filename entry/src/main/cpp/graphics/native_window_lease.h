#pragma once

#include <native_window/external_window.h>

#include <cstdint>

namespace winehua {

enum class NativeWindowReleaseMode {
    DestroyParcelWindow,
    UnreferenceNativeObject,
};

class NativeWindowLease {
public:
    NativeWindowLease() = default;
    ~NativeWindowLease() { Reset(); }

    NativeWindowLease(const NativeWindowLease&) = delete;
    NativeWindowLease& operator=(const NativeWindowLease&) = delete;

    void Adopt(OHNativeWindow* window, NativeWindowReleaseMode mode)
    {
        Reset();
        window_ = window;
        mode_ = mode;
    }

    int32_t Reset()
    {
        if (!window_) return 0;

        int32_t result = 0;
        if (mode_ == NativeWindowReleaseMode::UnreferenceNativeObject)
            result = OH_NativeWindow_NativeObjectUnreference(window_);
        else
            OH_NativeWindow_DestroyNativeWindow(window_);

        window_ = nullptr;
        mode_ = NativeWindowReleaseMode::DestroyParcelWindow;
        return result;
    }

    OHNativeWindow* Get() const { return window_; }
    bool UsesNativeObjectReference() const
    {
        return mode_ == NativeWindowReleaseMode::UnreferenceNativeObject;
    }
    explicit operator bool() const { return window_ != nullptr; }

private:
    OHNativeWindow* window_ = nullptr;
    NativeWindowReleaseMode mode_ = NativeWindowReleaseMode::DestroyParcelWindow;
};

} // namespace winehua
