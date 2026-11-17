#pragma once

#include <cstdint>
#include <string>

bool StartHostVulkanProbe(uint64_t surfaceId, const std::string& runId);
void StopHostVulkanProbe();
/** 轻量查询 host Vulkan 物理设备名称 (GPU 型号, 如 "Mali-G920")。无 Vulkan
 * 环境/失败时返回空串。用于 ArkTS 按 GPU 能力控制 DXVK2.6 等选项。 */
std::string ProbeGpuDeviceName();
