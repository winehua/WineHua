// Exercise the actual broker/JS callback path with the SDK NAPI ABI.
#include "bridge/system_open.cpp"
#include <cassert>
#include <iostream>

static napi_valuetype returnedType = napi_undefined;
static bool returnedBool = false, throwCallback = false, cancelCallback = false;
static unsigned calls = 0, cleared = 0;
extern "C" {
napi_status napi_create_string_utf8(napi_env, const char*, size_t, napi_value* value) {
    *value = reinterpret_cast<napi_value>(2); return napi_ok;
}
napi_status napi_get_undefined(napi_env, napi_value* value) {
    *value = reinterpret_cast<napi_value>(3); return napi_ok;
}
napi_status napi_call_function(napi_env, napi_value, napi_value, size_t, const napi_value*, napi_value* result) {
    ++calls;
    if (cancelCallback) ResetSystemOpenRequests();
    *result = reinterpret_cast<napi_value>(4);
    return throwCallback ? napi_pending_exception : napi_ok;
}
napi_status napi_typeof(napi_env, napi_value, napi_valuetype* type) { *type = returnedType; return napi_ok; }
napi_status napi_get_value_bool(napi_env, napi_value, bool* value) { *value = returnedBool; return napi_ok; }
napi_status napi_is_exception_pending(napi_env, bool* value) { *value = throwCallback; return napi_ok; }
napi_status napi_get_and_clear_last_exception(napi_env, napi_value* value) { ++cleared; *value = nullptr; return napi_ok; }
}

static void Check(napi_valuetype type, bool value, bool throwing, bool cancel, bool expected) {
    returnedType = type; returnedBool = value; throwCallback = throwing; cancelCallback = cancel;
    auto request = std::make_shared<Request>();
    request->target = "https://example.com";
    request->deadline = Clock::now() + std::chrono::seconds(1);
    { std::lock_guard<std::mutex> lock(bridge); requests.push_back(request); }
    CallJs(reinterpret_cast<napi_env>(1), reinterpret_cast<napi_value>(1), nullptr,
           new std::shared_ptr<Request>(request));
    assert(request->done && request->accepted == expected);
    ResetSystemOpenRequests();
}

int main() {
    Check(napi_undefined, false, false, false, true); // unchanged master callback
    Check(napi_boolean, true, false, false, true);
    Check(napi_boolean, false, false, false, false);
    Check(napi_string, false, false, false, false);
    Check(napi_undefined, false, true, false, false);
    Check(napi_undefined, false, false, true, false); // cancellation inside JS must not deadlock
    assert(calls == 6 && cleared == 1);
    auto expired = std::make_shared<Request>(); expired->deadline = Clock::now();
    CallJs(reinterpret_cast<napi_env>(1), reinterpret_cast<napi_value>(1), nullptr,
           new std::shared_ptr<Request>(expired));
    assert(expired->done && !expired->accepted && calls == 6);
    std::cout << "OPEN callback PASS: master void/boolean acknowledgement, rejection, exceptions, expiry and reentrant cancellation\n";
}
