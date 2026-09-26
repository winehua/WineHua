/* winehua_t_shell_link — 快捷方式（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.18。
 * 失败特征：断=shell 命名空间层。
 * 协议：IShellLink 设置目标/参数/工作目录 → IPersistFile 保存 .lnk →
 * 重新 Load → GetPath/GetArguments/GetWorkingDirectory 往返一致。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <objidl.h>
#include <shobjidl.h>
#include <shlguid.h>

#define LNK_PATH "C:\\smoke\\winehua_t_link.lnk"
#define TARGET "C:\\windows\\notepad.exe"
#define ARGS "-test file.txt"
#define WORKDIR "C:\\smoke"

int main(int argc, char **argv)
{
    HRESULT hr;
    IShellLinkA *link = NULL;
    IPersistFile *pf = NULL;
    char path[MAX_PATH], args[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAA fd;

    t_begin("winehua_t_shell_link", argc, argv);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    t_check("coinitialize", SUCCEEDED(hr), "hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr))
        return t_finish();

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void **)&link);
    t_check("cocreate-shelllink", hr == S_OK && link != NULL,
            "hr=0x%08lx", (unsigned long)hr);
    if (hr != S_OK || !link)
    {
        CoUninitialize();
        return t_finish();
    }

    t_check("set-path", IShellLinkA_SetPath(link, TARGET) == S_OK, "");
    t_check("set-arguments", IShellLinkA_SetArguments(link, ARGS) == S_OK, "");
    t_check("set-workdir", IShellLinkA_SetWorkingDirectory(link, WORKDIR) == S_OK, "");

    hr = IShellLinkA_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
    t_check("queryinterface-persistfile", hr == S_OK && pf != NULL,
            "hr=0x%08lx", (unsigned long)hr);
    if (hr == S_OK && pf)
    {
        WCHAR wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, LNK_PATH, -1, wide, MAX_PATH);
        hr = IPersistFile_Save(pf, wide, TRUE);
        t_check("save-lnk", hr == S_OK, "hr=0x%08lx", (unsigned long)hr);
        if (hr == S_OK)
        {
            /* 重新解析：新对象 Load 后比对字段 */
            IShellLinkA *back = NULL;
            IPersistFile *pf2 = NULL;
            hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IShellLinkA, (void **)&back);
            if (hr == S_OK && back)
            {
                hr = IShellLinkA_QueryInterface(back, &IID_IPersistFile, (void **)&pf2);
                if (hr == S_OK && pf2)
                {
                    hr = IPersistFile_Load(pf2, wide, STGM_READ);
                    t_check("load-lnk", hr == S_OK, "hr=0x%08lx", (unsigned long)hr);
                    if (hr == S_OK)
                    {
                        memset(path, 0, sizeof(path));
                        memset(args, 0, sizeof(args));
                        memset(dir, 0, sizeof(dir));
                        IShellLinkA_GetPath(back, path, MAX_PATH, &fd, 0);
                        IShellLinkA_GetArguments(back, args, MAX_PATH);
                        IShellLinkA_GetWorkingDirectory(back, dir, MAX_PATH);
                        t_check("path-roundtrip", !lstrcmpiA(path, TARGET),
                                "path='%s'", path);
                        t_check("args-roundtrip", !lstrcmpA(args, ARGS),
                                "args='%s'", args);
                        t_check("workdir-roundtrip", !lstrcmpiA(dir, WORKDIR),
                                "dir='%s'", dir);
                    }
                    IPersistFile_Release(pf2);
                }
                IShellLinkA_Release(back);
            }
            DeleteFileA(LNK_PATH);
        }
        IPersistFile_Release(pf);
    }
    IShellLinkA_Release(link);
    CoUninitialize();
    return t_finish();
}
