/* winehua_t_wsi_xcb_probe — R-WSI: guest Vulkan 是否暴露 X11 WSI 扩展。
 *
 * spec §5 R-WSI 裁决探针 (M1-T7): winex11.drv/vulkan.c 的 X11 WSI 路线
 * 要求 loader/ICD 暴露 VK_KHR_xcb_surface。本程序只枚举
 * vkEnumerateInstanceExtensionProperties 并回填结论, 不建 instance、
 * 不建 surface (那是 M2 落地工作)。
 *
 * 动态加载 vulkan-1.dll (wine loader; ICD 由 wine 环境的 VK_DRIVER_FILES
 * 指向 venus), 不拉 Vulkan 头 —— 三个调用点用不到别的声明。
 */
#include "../common/winehua_t_check.h"

#define VK_SUCCESS 0

struct vk_ext_props
{
    char extensionName[256];
    unsigned specVersion;
};

typedef int (*PFN_vkEnumerateInstanceExtensionProperties)(
    const char *layer, unsigned *count, struct vk_ext_props *props);

int main(int argc, char **argv)
{
    PFN_vkEnumerateInstanceExtensionProperties enumerate;
    static struct vk_ext_props exts[64];
    HMODULE vk;
    unsigned count = 0, i;
    int have_xcb = 0, have_xlib = 0, have_surface = 0;
    unsigned xcb_version = 0, xlib_version = 0, surface_version = 0;
    char list[1400];
    int off = 0;
    int rc;

    t_begin("winehua_t_wsi_xcb_probe", argc, argv);

    vk = LoadLibraryA("vulkan-1.dll");
    t_check("load-vulkan-1", vk != NULL, "GetLastError=%lu", GetLastError());
    if (!vk)
    {
        t_skip("vulkan-1.dll load failed (no guest vulkan loader)");
        return t_finish();
    }

    enumerate = (PFN_vkEnumerateInstanceExtensionProperties)
        GetProcAddress(vk, "vkEnumerateInstanceExtensionProperties");
    t_check("get-enumerate-proc", enumerate != NULL,
            "vkEnumerateInstanceExtensionProperties");
    if (!enumerate)
    {
        t_skip("loader missing vkEnumerateInstanceExtensionProperties export");
        return t_finish();
    }

    rc = enumerate(NULL, &count, NULL);
    t_check("count-query", rc == VK_SUCCESS, "rc=%d", rc);
    if (rc != VK_SUCCESS)
    {
        t_skip("instance extension count query failed");
        return t_finish();
    }
    t_metric("instance-ext-count", "%u", count);
    if (count > 64)
        count = 64;

    rc = enumerate(NULL, &count, exts);
    t_check("enum-extensions", rc == VK_SUCCESS, "rc=%d", rc);
    if (rc != VK_SUCCESS)
    {
        t_skip("instance extension enumeration failed");
        return t_finish();
    }

    for (i = 0; i < count; ++i)
    {
        const char *name = exts[i].extensionName;
        int room = (int)sizeof(list) - off;
        if (room > 4)
            off += _snprintf(list + off, room, "%s%s", i ? " " : "", name);
        if (off < 0 || off >= (int)sizeof(list))
            off = (int)sizeof(list) - 1;
        if (!lstrcmpA(name, "VK_KHR_xcb_surface"))
        {
            have_xcb = 1;
            xcb_version = exts[i].specVersion;
        }
        else if (!lstrcmpA(name, "VK_KHR_xlib_surface"))
        {
            have_xlib = 1;
            xlib_version = exts[i].specVersion;
        }
        else if (!lstrcmpA(name, "VK_KHR_surface"))
        {
            have_surface = 1;
            surface_version = exts[i].specVersion;
        }
    }
    list[off] = '\0';
    t_check("ext-list-captured", off > 0, "count=%u: %s", count, list);

    t_metric("vk-khr-surface", have_surface ? "yes spec=%u" : "absent", surface_version);
    t_metric("vk-khr-xcb-surface", have_xcb ? "yes spec=%u" : "absent", xcb_version);
    t_metric("vk-khr-xlib-surface", have_xlib ? "yes spec=%u" : "absent", xlib_version);

    /* R-WSI 事实采集 (判定在主机侧): 设备端只报有/无, 不把「absent」记成
     * 失败 —— 能力缺席是合法测量答案 (smoke 硬约束: 能力探针报
     * UNSUPPORTED/SKIP 不算失败), 用 t_skip 携带结论, metrics 供归档细读。
     * VK_KHR_surface 是 WSI 基座, 缺它则 xcb 有也无意义, 一并入结论。 */
    if (!have_surface)
        t_skip("VK_KHR_surface absent: no WSI base extension");
    else if (!have_xcb)
        t_skip("VK_KHR_xcb_surface absent: winex11 X11 WSI unavailable,"
               " Vulkan present stays on wayland route (R-WSI verdict data)");
    else
        t_check("xcb-surface-present", 1, "VK_KHR_xcb_surface spec=%u", xcb_version);

    return t_finish();
}
