/* winehua_t_com_basic — COM 基础（P3，手段 R）。
 * 判定规格见 docs/engineering/testing-programs.md §3.11。
 * 失败特征：断=COM 服务表/注册表协作断。
 * 协议：CoInitializeEx → CoCreateInstance(CLSID_ShellLink) → QueryInterface
 * (IPersistFile) → Release×2 引用计数归零 → CoUninitialize，全链 S_OK。
 */
#define COBJMACROS
#include "../common/winehua_t_check.h"
#include <objidl.h>
#include <shobjidl.h>
#include <shlguid.h>

int main(int argc, char **argv)
{
    HRESULT hr;
    IShellLinkA *link = NULL;
    IPersistFile *pf = NULL;
    ULONG ref1, ref2;

    t_begin("winehua_t_com_basic", argc, argv);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    t_check("coinitialize", SUCCEEDED(hr), "hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr))
        return t_finish();

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void **)&link);
    t_check("cocreate-shelllink", hr == S_OK && link != NULL,
            "hr=0x%08lx", (unsigned long)hr);
    if (hr == S_OK && link)
    {
        hr = IShellLinkA_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
        t_check("queryinterface-persistfile", hr == S_OK && pf != NULL,
                "hr=0x%08lx", (unsigned long)hr);
        if (pf)
        {
            /* Release 返回值 MSDN 定性为仅诊断用（ShellLink 内部聚合的
             * 子对象引用不必归零），记 metric 不断言；归零判定收在主对象 */
            ref1 = IPersistFile_Release(pf);
            t_metric("persistfile-last-ref", "%lu", (unsigned long)ref1);
        }
        /* AddRef/Release 往返：主对象计数守恒归零 */
        IShellLinkA_AddRef(link);
        ref2 = IShellLinkA_Release(link);
        ref1 = IShellLinkA_Release(link);
        t_check("refcount-zero", ref1 == 0, "ref=%lu", (unsigned long)ref1);
    }

    CoUninitialize();
    t_metric("com-basic", "done hr_link=0x%08lx", (unsigned long)hr);
    return t_finish();
}
