/* winehua_t_font_enum — 字体枚举与 CJK 存在性（P5，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.4。
 * 失败特征：CJK 字族缺失=字体扫描/locale 链断（wine 中文 UI 的用户侧
 * 观测面）；枚举空=gdi 枚举断。
 * EnumFontFamiliesEx 按 charset 各扫一轮（DEFAULT/GB2312）；中文轮廓经
 * GetGlyphOutlineW(GGO_NATIVE) 取 TTPOLYGON 曲线数据，非空即字模
 * 扫描链通。
 */
#include "../common/winehua_t_check.h"
#include <wingdi.h>

static int g_families_ansi;
static int g_families_gb;
static char g_first_family[LF_FACESIZE];
static char g_first_gb_family[LF_FACESIZE];

static int CALLBACK fam_cb_ansi(const LOGFONTA *lf, const TEXTMETRICA *tm,
                                DWORD type, LPARAM lparam)
{
    (void)tm; (void)type; (void)lparam;
    if (lf && lf->lfFaceName[0] && !g_first_family[0])
        lstrcpynA(g_first_family, lf->lfFaceName, LF_FACESIZE);
    g_families_ansi++;
    return 1;
}

static int CALLBACK fam_cb_gb(const LOGFONTA *lf, const TEXTMETRICA *tm,
                              DWORD type, LPARAM lparam)
{
    (void)tm; (void)type; (void)lparam;
    if (lf && lf->lfFaceName[0] && !g_first_gb_family[0])
        lstrcpynA(g_first_gb_family, lf->lfFaceName, LF_FACESIZE);
    g_families_gb++;
    return 1;
}

int main(int argc, char **argv)
{
    HDC dc;
    LOGFONTA lf;
    GLYPHMETRICS gm;
    MAT2 mat;
    DWORD shape_len;
    HFONT hfont, old;

    t_begin("winehua_t_font_enum", argc, argv);

    dc = GetDC(NULL);
    t_check("get-screen-dc", dc != NULL, "dc=%p", (void *)dc);
    if (!dc)
        return t_finish();

    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExA(dc, &lf, fam_cb_ansi, 0, 0);
    t_check("enum-ansi-nonempty", g_families_ansi > 0 && g_first_family[0],
            "families=%d first=%s", g_families_ansi, g_first_family);

    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = GB2312_CHARSET;
    EnumFontFamiliesExA(dc, &lf, fam_cb_gb, 0, 0);
    t_check("enum-gb2312-nonempty", g_families_gb > 0,
            "gb_families=%d first=%s", g_families_gb, g_first_gb_family);
    /* CJK 存在性与具体字族名解耦（设备部署的 CJK 字体名不定，
     * GB2312 轮有字族 + 中文字形轮廓非空 = 中文渲染输入面可用） */
    t_check("cjk-family-present", g_families_gb > 0,
            "gb_families=%d", g_families_gb);

    /* 中文轮廓非空：选一个 CJK 字体后取 U+4E2D 的字形曲线 */
    hfont = CreateFontW(32, 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        DEFAULT_QUALITY, DEFAULT_PITCH, L"SimSun");
    old = hfont ? SelectObject(dc, hfont) : NULL;
    memset(&gm, 0, sizeof(gm));
    memset(&mat, 0, sizeof(mat));
    mat.eM11.value = mat.eM22.value = 1 << 16; /* 1.0 fixed */
    SetLastError(0);
    shape_len = GetGlyphOutlineW(dc, 0x4E2D, GGO_NATIVE, &gm, 0, NULL, &mat);
    t_check("cjk-glyph-outline", shape_len > 0 && shape_len != GDI_ERROR,
            "U+4E2D shape=%lu bytes (GDI_ERROR=%u)",
            (unsigned long)shape_len, (unsigned)GDI_ERROR);
    if (old) SelectObject(dc, old);
    if (hfont) DeleteObject(hfont);

    t_metric("font-families-ansi", "%d", g_families_ansi);
    t_metric("font-families-gb", "%d", g_families_gb);
    t_metric("font-first-gb-family", "%s", g_first_gb_family);
    ReleaseDC(NULL, dc);
    return t_finish();
}
