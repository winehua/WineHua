#pragma once
#include <atomic>
extern std::atomic<bool> ProcessTestPhoneMode;
inline bool PhoneAdapter_IsPhoneMode() { return ProcessTestPhoneMode; }
