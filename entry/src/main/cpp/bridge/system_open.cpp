#include "system_open.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>

namespace {
using Clock = std::chrono::steady_clock;
struct Request {
    std::mutex mutex; std::condition_variable changed;
    std::string target; Clock::time_point deadline;
    bool done = false, accepted = false;
};
std::mutex bridge;
napi_threadsafe_function callback;
bool enabled = false;
std::vector<std::shared_ptr<Request>> requests;
void CancelLocked() {
    for (auto& r : requests) {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->done = true; r->accepted = false; r->changed.notify_all();
    }
    requests.clear();
}
void CallJs(napi_env env, napi_value fn, void*, void* data) {
    std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request>*>(data));
    auto r = *holder;
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        if (!env || !fn || r->done || Clock::now() >= r->deadline) {
            r->done = true; r->changed.notify_all(); return;
        }
    }
    napi_value arg, receiver, result;
    napi_create_string_utf8(env, r->target.data(), r->target.size(), &arg);
    napi_get_undefined(env, &receiver);
    bool accepted = false;
    // Master callbacks return void and initiate startAbility asynchronously.
    // Preserve their acknowledgement after execution, avoiding a second Wine
    // open. Boolean callbacks can still reject synchronously for fallback.
    // Do not hold the request mutex across JS: a callback may cancel requests.
    if (napi_call_function(env, receiver, fn, 1, &arg, &result) == napi_ok) {
        napi_valuetype type = napi_undefined;
        if (napi_typeof(env, result, &type) == napi_ok) {
            if (type == napi_undefined) accepted = true;
            else if (type == napi_boolean) napi_get_value_bool(env, result, &accepted);
        }
    } else {
        bool pending = false; napi_is_exception_pending(env, &pending);
        if (pending) { napi_value error; napi_get_and_clear_last_exception(env, &error); }
    }
    std::lock_guard<std::mutex> lock(r->mutex);
    if (!r->done && Clock::now() < r->deadline) r->accepted = accepted;
    r->done = true; r->changed.notify_all();
}
napi_value SetEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value arg; bool value = false;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    if (argc == 1 && napi_get_value_bool(env, arg, &value) == napi_ok) {
        std::lock_guard<std::mutex> lock(bridge); enabled = value; if (!value) CancelLocked();
    }
    napi_value result; napi_get_undefined(env, &result); return result;
}
napi_value SetCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value arg, name, result; napi_valuetype type = napi_undefined;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    if (argc) napi_typeof(env, arg, &type);
    std::lock_guard<std::mutex> lock(bridge); CancelLocked();
    if (callback) { napi_release_threadsafe_function(callback, napi_tsfn_abort); callback = nullptr; }
    if (type == napi_function) {
        napi_create_string_utf8(env, "systemOpen", NAPI_AUTO_LENGTH, &name);
        if (napi_create_threadsafe_function(env, arg, nullptr, name, 16, 1, nullptr, nullptr,
            nullptr, CallJs, &callback) == napi_ok) napi_unref_threadsafe_function(env, callback);
        else callback = nullptr;
    }
    napi_get_undefined(env, &result); return result;
}
void Cleanup(void*) {
    std::lock_guard<std::mutex> lock(bridge); enabled = false; CancelLocked();
    if (callback) { napi_release_threadsafe_function(callback, napi_tsfn_abort); callback = nullptr; }
}
}
void ResetSystemOpenRequests() { std::lock_guard<std::mutex> lock(bridge); CancelLocked(); }
bool DispatchSystemOpen(const std::string& target) {
    if (target.empty() || target.size() > 4090 || target.find_first_of("\r\n") != std::string::npos) return false;
    auto r = std::make_shared<Request>(); r->target = target; r->deadline = Clock::now() + std::chrono::milliseconds(1500);
    {
        std::lock_guard<std::mutex> lock(bridge);
        if (!enabled || !callback || requests.size() >= 16) return false;
        auto* holder = new std::shared_ptr<Request>(r);
        if (napi_call_threadsafe_function(callback, holder, napi_tsfn_nonblocking) != napi_ok) { delete holder; return false; }
        requests.push_back(r);
    }
    bool accepted;
    { std::unique_lock<std::mutex> lock(r->mutex);
      r->changed.wait_until(lock, r->deadline, [&] { return r->done; });
      accepted = r->done && r->accepted; r->done = true; }
    { std::lock_guard<std::mutex> lock(bridge);
      requests.erase(std::remove(requests.begin(), requests.end(), r), requests.end()); }
    return accepted;
}
void RegisterSystemOpen(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"setSystemOpenEnabled", nullptr, SetEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOpenTargetCallback", nullptr, SetCallback, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    napi_define_properties(env, exports, 2, props); napi_add_env_cleanup_hook(env, Cleanup, nullptr);
}
