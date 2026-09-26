/* winehua_t_fs_drives — 盘符映射断言（P0）。
 * 判定规格见 docs/engineering/testing-programs.md §3.7。
 * 失败特征：Z: 缺失 = dosdevices/盘符映射链断（ohos_file 回归哨兵）。
 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    DWORD drives;
    UINT drive_type_c, drive_type_z;
    char volume_name[MAX_PATH + 1];
    char fs_name[MAX_PATH + 1];

    t_begin("winehua_t_fs_drives", argc, argv);

    drives = GetLogicalDrives();
    t_metric("drive_mask", "0x%08X", (unsigned)drives);
    t_check("drive-c-present", (drives & (1u << ('C' - 'A'))) != 0,
            "mask 0x%08X", (unsigned)drives);
    t_check("drive-z-present", (drives & (1u << ('Z' - 'A'))) != 0,
            "mask 0x%08X", (unsigned)drives);

    drive_type_c = GetDriveTypeA("C:\\");
    t_check("drive-c-type-fixed", drive_type_c == DRIVE_FIXED,
            "type=%u", drive_type_c);

    drive_type_z = GetDriveTypeA("Z:\\");
    t_check("drive-z-type-known", drive_type_z == DRIVE_FIXED ||
            drive_type_z == DRIVE_REMOTE, "type=%u", drive_type_z);

    memset(volume_name, 0, sizeof(volume_name));
    memset(fs_name, 0, sizeof(fs_name));
    t_check("z-volume-info", GetVolumeInformationA("Z:\\", volume_name,
            sizeof(volume_name), NULL, NULL, NULL, fs_name, sizeof(fs_name)),
            "volume='%s' fs='%s'", volume_name, fs_name);

    /* C: 应可访问根目录 */
    t_check("c-root-accessible", GetFileAttributesA("C:\\") != INVALID_FILE_ATTRIBUTES,
            "attr=0x%08X", GetFileAttributesA("C:\\"));
    t_check("z-root-accessible", GetFileAttributesA("Z:\\") != INVALID_FILE_ATTRIBUTES,
            "attr=0x%08X", GetFileAttributesA("Z:\\"));
    return t_finish();
}
