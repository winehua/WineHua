#include "legacy_text_compat.h"
#include <cassert>

int main()
{
    assert(winehua::DuanwuD3dOverride(nullptr) == nullptr);
    assert(winehua::DuanwuD3dOverride("") == nullptr);
    assert(winehua::DuanwuD3dOverride("0") == nullptr);
    assert(winehua::DuanwuD3dOverride("yes") == nullptr);
    assert(!std::strcmp(winehua::DuanwuD3dOverride("1"), "csmt=0"));
    using winehua::UsesDuanwuBitmapText;
    assert(UsesDuanwuBitmapText("Z:/games/唯美端午节补丁版/启动器.exe"));
    assert(UsesDuanwuBitmapText("Z:\\games\\唯美端午节补丁版\\启动器.exe"));
    assert(UsesDuanwuBitmapText("/storage/Users/currentUser/Download/app.hackeris.winehua/games/唯美端午节补丁版/启动器.exe"));
    assert(UsesDuanwuBitmapText("启动器.exe", "Z:/games/唯美端午节补丁版/"));
    assert(!UsesDuanwuBitmapText("Z:/games/another/启动器.exe"));
    assert(!UsesDuanwuBitmapText("Z:/games/唯美端午节补丁版/资料片DLC/dlc.exe"));
    assert(!UsesDuanwuBitmapText("Z:/games/MewGenics/Mewgenics.exe"));
    assert(!UsesDuanwuBitmapText("Z:/games/唯美端午节补丁版-other/启动器.exe"));
    assert(!UsesDuanwuBitmapText("启动器.exe"));
    return 0;
}
