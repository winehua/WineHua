/* winehua_t_resource — 资源加载（P2，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.18。
 * 失败特征：断 = PE 资源段解析/图形资源链。
 */
#define OEMRESOURCE /* OBM_* 标准位图桩需要 */
#include "../common/winehua_t_check.h"

int main(int argc, char **argv)
{
    HICON icon;
    HCURSOR cursor;
    HBITMAP bmp;

    t_begin("winehua_t_resource", argc, argv);

    /* 系统共享图标/光标（LoadIcon/LoadCursor 0 桩） */
    icon = LoadIconA(NULL, IDI_APPLICATION);
    t_check("load-icon", icon != NULL, "err=%lu", icon ? 0 : GetLastError());
    cursor = LoadCursorA(NULL, IDC_ARROW);
    t_check("load-cursor", cursor != NULL, "err=%lu", cursor ? 0 : GetLastError());
    if (icon)
        t_check("draw-icon", DrawIconEx(GetDC(NULL), 0, 0, icon, 32, 32, 0, NULL, DI_NORMAL),
                "DrawIconEx failed");
    if (icon) DestroyIcon(icon);
    if (cursor) DestroyCursor(cursor);

    /* LoadImage 从内存/系统标准位图 */
    bmp = (HBITMAP)LoadImageA(NULL, MAKEINTRESOURCEA(OBM_CHECK), IMAGE_BITMAP, 0, 0,
                              LR_DEFAULTSIZE);
    t_check("load-std-bitmap", bmp != NULL, "err=%lu", bmp ? 0 : GetLastError());
    if (bmp)
    {
        BITMAP bm;
        if (GetObjectA(bmp, sizeof(bm), &bm) >= (int)sizeof(bm))
            t_check("bitmap-dims", bm.bmWidth > 0 && bm.bmHeight > 0,
                    "%dx%d", bm.bmWidth, bm.bmHeight);
        else
            t_check("bitmap-dims", 0, "GetObject failed");
        DeleteObject(bmp);
    }

    /* 文件位图：先写一个最小 24bpp BMP 再 LoadImage 读回 */
    {
        char temp[MAX_PATH], path[MAX_PATH];
        FILE *fp;
        if (GetTempPathA(sizeof(temp), temp) > 0)
        {
            snprintf(path, sizeof(path), "%swh_t_resource.bmp", temp);
            fp = fopen(path, "wb");
            t_check("write-bmp-file", fp != NULL, "path=%s", path);
            if (fp)
            {
                /* 2x2 24bpp BMP */
                unsigned char bmp_data[] = {
                    'B','M', 0x76,0x00,0x00,0x00, 0,0,0,0, 0x46,0x00,0x00,0x00,
                    0x28,0x00,0x00,0x00, 2,0,0,0, 2,0,0,0, 1,0, 24,0,
                    0,0,0,0, 0x30,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
                    0xFF,0x00,0x00, 0, 0, 0x00,0x00,0xFF, 0, 0, 0, 0,
                };
                fwrite(bmp_data, 1, sizeof(bmp_data), fp);
                fclose(fp);
                {
                    HBITMAP loaded = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0,
                                                         LR_LOADFROMFILE);
                    t_check("load-from-file", loaded != NULL,
                            "err=%lu", loaded ? 0 : GetLastError());
                    if (loaded)
                    {
                        BITMAP bm;
                        if (GetObjectA(loaded, sizeof(bm), &bm) >= (int)sizeof(bm))
                            t_check("file-bitmap-dims", bm.bmWidth == 2 && bm.bmHeight == 2,
                                    "%dx%d", bm.bmWidth, bm.bmHeight);
                        else
                            t_check("file-bitmap-dims", 0, "GetObject failed");
                        DeleteObject(loaded);
                    }
                    DeleteFileA(path);
                }
            }
        }
    }
    return t_finish();
}
