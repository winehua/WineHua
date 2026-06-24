# Wine ARM64 Native 缂栬瘧 + ARM64EC 浠跨湡 绉绘鏂规

鍙傝€冮」鐩? HangOver (https://github.com/AndreRH/hangover)

## 鎬昏

```
鈹屸攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
鈹? x86_64 Windows App (ARM64EC 浠跨湡)        鈹? FEX/Box64-ARM64EC
鈹溾攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
鈹? ARM64 Wine PE DLLs (aarch64-windows)    鈹? WoW64 灞?
鈹? ARM64 Wine Unix .so (aarch64-linux-ohos) 鈹? native
鈹溾攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
鈹? HarmonyOS ARM64                          鈹?
鈹斺攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
```

## 瀹炵幇姝ラ

### Phase 1: 缂栬瘧宸ュ叿閾惧噯澶?

1. **aarch64-w64-mingw32 浜ゅ弶缂栬瘧鍣?*锛圵ine PE DLL 缂栬瘧蹇呴渶锛?
   - 褰撳墠 OHOS SDK 涓嶆彁渚?mingw ARM64 浜ゅ弶缂栬瘧鍣?
   - 鏂规A: 鐢?LLVM/clang 鑷缓锛坄--target=aarch64-w64-mingw32`锛?
   - 鏂规B: 浠?HangOver 鍙傝€冨叾 mingw 鏋勫缓鏂瑰紡
   - 杈撳嚭: `/opt/mingw-aarch64/bin/aarch64-w64-mingw32-gcc`

2. **OHOS ARM64 sysroot 鎵╁睍**锛堜緷璧栧簱锛?
   - FreeType, Wayland, xkbcommon 浜ゅ弶缂栬瘧涓?aarch64-linux-ohos
   - 瀹夎鍒?`build/sysroot-ext/usr/lib/aarch64-linux-ohos/`

### Phase 2: 鏋勫缓鑴氭湰鏀归€?

3. **scripts/env.sh** 鈥?鏋舵瀯鍙傛暟鍖?
   - `TARGET` 浠?`NATIVE_ARCH` 鎺ㄥ: `arm64-v8a` 鈫?`aarch64-linux-ohos`
   - `SYSROOT_EXT_LIB` 鍙傛暟鍖栦负 `$SYSROOT_EXT/usr/lib/$TARGET`

4. **scripts/build_wine.sh** 鈥?ARM64 configure + 缂栬瘧
   - `--host=aarch64-linux-ohos`
   - `--enable-archs=aarch64,x86_64`锛圵oW64: PE 绔敮鎸佷袱绉嶆灦鏋勶級
   - `aarch64_CC=/opt/mingw-aarch64/bin/aarch64-w64-mingw32-gcc`
   - Unix .so 鐢熸垚璺緞: `build-ohos/dlls/*/xxx.so` (ARM64 ELF)
   - PE .dll 鐢熸垚璺緞: `build-ohos/dlls/*/aarch64-windows/` + `x86_64-windows/`

5. **scripts/build_deps.sh** 鈥?ARM64 渚濊禆
   - build_freetype/wayland/xkbcommon 澧炲姞 aarch64-linux-ohos target

6. **scripts/assemble.sh** 鈥?ARM64 甯冨眬
   - Unix .so 鈫?`libs/arm64-v8a/` (涓嶅啀璧?rawfile zip)
   - PE .dll 鈫?rawfile `bin/aarch64-windows/` + `bin/x86_64-windows/`
   - **璺宠繃 Box64 鏋勫缓**

### Phase 3: ARM64EC 浠跨湡鍣ㄩ泦鎴?

7. **ARM64EC 浠跨湡灞?*
   - 鍙傝€?HangOver 鐨?`libarm64ecfex.dll`锛團EX 闆嗘垚涓?PE DLL锛?
   - Wine 鐨?ARM64EC 鏈哄埗: 搴旂敤鍏ュ彛涓?x86_64锛學indows API 璋冪敤鏃跺垏鎹㈠埌 ARM64 鍘熺敓
   - 闇€瑕佺紪璇?FEX 鎴?Box64 涓?OHOS ARM64 鍘熺敓 .so
   - 浣滀负 `libarm64ecfex.dll` 鏀惧叆 `bin/aarch64-windows/`

### Phase 4: Wine 浠ｇ爜閫傞厤

8. **OHOS ARM64 鍏煎淇**锛堥璁″皯閲忥級
   - `dlls/ntdll/unix/signal_arm64.c` 鈥?纭 OHOS musl 淇″彿鍏煎
   - `dlls/ntdll/unix/virtual.c` 鈥?ARM64EC map 鍦?OHOS 涓婄殑 mmap 鍏煎
   - PAD_MODE 浠ｇ爜锛坧rocess.c/loader.c锛夊凡鍦ㄥ師鐢熶唬鐮佷腑锛孉RM64 鍚屾牱閫傜敤

### Phase 5: 娴嬭瘯

9. **x86_64 Windows 搴旂敤楠岃瘉**
   - wineboot --init
   - notepad
   - explorer /desktop

## 椋庨櫓

| 椋庨櫓 | 褰卞搷 | 缂撹В |
|------|------|------|
| aarch64 mingw 浜ゅ弶缂栬瘧鍣ㄦ瀯寤?| 闃诲 PE DLL 缂栬瘧 | 鐢?LLVM/clang 鑷缓锛屽弬鑰?HangOver |
| ARM64 WoW64 鍦?Linux 涓婁笉澶熸垚鐔?| x86_64 搴旂敤鍏煎鎬?| 閲嶇偣鍔熻兘鑱氱劍锛岄€愭閫傞厤 |
| OHOS musl + ARM64 淇″彿鍏煎 | 宕╂簝 | 宸叉湁 signal_arm64.c锛屽彧闇€楠岃瘉 |
| FEX 绉绘鍒?OHOS | 浠跨湡鍣ㄦ牳蹇?| Box64 宸叉湁 OHOS 绉绘缁忛獙锛屽彲澶嶇敤 |
