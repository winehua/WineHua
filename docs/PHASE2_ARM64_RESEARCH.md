# ARM64 Pad 鏂规: Box64 .so 璺ㄦ灦鏋勬ā鎷?

> 鏇存柊: 2026-06-22
> 鐘舵€? 鉁?宸插疄鐜?

---

## 1. 鏍稿績姒傚康

**鍦烘櫙**: HarmonyOS ARM64 Pad 璁惧涓婅繍琛?Windows x86_64 绋嬪簭銆?

鏂规锛?*Box64 缂栬瘧涓哄叡浜簱 (box64.so)** 鈥?Wine 缂栬瘧涓?x86_64 (musl)锛孊ox64 .so 鐢?NCP 瀛愯繘绋?dlopen 鍔犺浇锛宍box64_hmos_main()` 鍦ㄥ悓涓€杩涚▼鍐呮ā鎷熸墽琛?x86_64 Wine ELF銆?

```
鈹屸攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
鈹?         HarmonyOS ARM64 Pad                        鈹?
鈹?                                                   鈹?
鈹? NCP 瀛愯繘绋?(appspawn)                              鈹?
鈹?   wine_child.so: Main()                           鈹?
鈹?     dlopen("box64.so") 鈫?box64_hmos_main()        鈹?
鈹?       鈫?                                          鈹?
鈹? Wine x86_64 ELF + PE DLLs (rawfile zip 瑙ｅ帇)      鈹?
鈹?       鈫?Box64 (x86_64 鈫?ARM64 Dynarec)            鈹?
鈹?       鈫?winewayland.drv                           鈹?
鈹? 宓屽叆寮?Wayland compositor 鈫?XComponent            鈹?
鈹斺攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
```

缂栬瘧锛歚cmake -DLIBBOX64_SO=ON`锛屼骇鐗?`box64.so` 鏀惧叆 `entry/libs/arm64-v8a/`銆?
杩愯鏃讹細`wine_child.cpp` 涓?`setenv("USE_LIBBOX64", "1")` 閫氱煡 Wine 渚ч€傞厤 entryParams銆?
wineserver 缂栦负 x86_64 PIE锛屽悓鏍风敱 Box64 鍔犺浇锛屼繚鎸佷笌 Wine 鏋舵瀯涓€鑷淬€?

---

## 2. 缂栬瘧楠岃瘉 (2026-06-11)

ARM64 wineserver + ntdll/unix 宸叉垚鍔熶氦鍙夌紪璇戯細

| 缁勪欢 | 缁撴灉 |
|------|------|
| wineserver | 鉁?ARM aarch64, `ld-musl-aarch64.so.1` |
| ntdll/unix | 鉁?15/21 缂栬瘧閫氳繃 (6 涓渶 configure 澶存枃浠? |

Phase 1 鐨?x86_64 缂栬瘧閰嶆柟瀵?ARM64 瀹屽叏閫傜敤锛屽彧闇€鏀?target triple銆?

---

## 3. 宸叉湁璧勬簮

| 缁勪欢 | 鐘舵€?| 璇存槑 |
|------|------|------|
| Box64 on OHOS ARM64 | 鉁?| Dynarec 妯″紡姝ｅ父杩愯 |
| OHOS ARM64 SDK | 鉁?| `aarch64-linux-ohos` 瀹屾暣鍙敤 |
| Wine x86_64 on Box64 | 鉁?| 褰撳墠鐢熶骇璺緞 |
| Wayland compositor | 鉁?| ARM64 鍘熺敓缂栬瘧 |
| XKB 閿洏鏁版嵁 | 鉁?| 鏋舵瀯鏃犲叧锛屽凡鎵撳寘鍒?HNP |

---

## 4. 寰呰В鍐抽棶棰?

### Box64 fork 瀛愯繘绋?RWX 澶辫触
- **闂**: Box64 fork 瀛愯繘绋嬫棤娉曞垱寤?PROT_EXEC 鍐呭瓨 (EINVAL)
- **褰卞搷**: wineserver 绛夊瓙杩涚▼鍙楅檺鍒?
- **缂撹В**: wineserver 浣跨敤 NAPI 鍘熺敓 fork 缁曡繃

### Wine ARM64 鍘熺敓 (鏈潵)
- 濡傞渶 Wine ARM64 鍘熺敓缂栬瘧锛岄渶瑕?`aarch64-w64-mingw32` 宸ュ叿閾句氦鍙夌紪璇?PE DLL
- 褰撳墠 x86_64 + Box64 鏂规宸叉弧瓒冲熀鏈渶姹傦紝ARM64 鍘熺敓鏆備笉蹇呰

---

## 5. 鎶€鏈喅绛?

| 鏃ユ湡 | 鍐崇瓥 | 鍘熷洜 |
|------|------|------|
| 2026-06 | 浣跨敤 Box64 鍏ㄦā鎷熻€岄潪 ARM64 鍘熺敓 Wine | Box64 OHOS 宸叉湁鎴愮啛绉绘锛岀珛鍗冲彲鐢?|
| 2026-06 | Wayland compositor 鐢?ARM64 鍘熺敓 | 鎬ц兘鍏抽敭璺緞锛岄渶涓?XComponent 鐩存帴浜や簰 |
