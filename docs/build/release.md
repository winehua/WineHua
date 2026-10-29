# 打包发布

> 适用场景：打上架用的发布包之前；发现签名或者打包配置不对时。
> 最后核实：2026-09-24
> 相关配置：根 `build-profile.json5`、`.ohos/`（配置与证书材料）、`AppScope/app.json5`（版本号）、`entry/src/main/module.json5`（设备类型）
> 相关文档：[guide.md](guide.md)（构建）、[../assets/environment.md](../assets/environment.md)（证书与配置文件清单）、[../engineering/quality.md](../engineering/quality.md)（发版前检查）

## 发布包和调试包的区别

| | 调试包 | 发布包 |
|---|---|---|
| 签名材料 | `.ohos/` 下的调试材料（`keyAlias` = `debugKey`） | `.ohos/release/` 下的发布材料 |
| 声明的设备类型 | phone / 2in1 / tablet | 去掉 phone |
| 构建模式 | debug + release 都在 | 只有 release |
| 怎么出包 | `make NATIVE_ARCH=arm64-v8a hap` | 换配置 → `make ... hap` → `hvigorw assembleApp` |
| 产物 | `entry/build/.../entry-default-signed.hap` | `build/outputs/default/wineohos-default-signed.app` |

签名不需要额外的操作：`hvigorw assembleHap` 自带的签名任务会读根 `build-profile.json5` 里 `signingConfigs` 指定的材料，配置文件写的是哪套就用哪套。

## 步骤

**发布包必须从 `main-ui` 分支出**（版本号在那边，见 [../engineering/workflow.md](../engineering/workflow.md)）。

### 1. 去掉 phone 设备类型

编辑 `entry/src/main/module.json5`，把 `deviceTypes` 里的 `"phone"` 删掉。这个文件在版本管理里，最后 `git checkout` 还原即可。

### 2. 换上发布配置

根目录的 `build-profile.json5` **被版本管理忽略**，不能用 `git checkout` 还原，必须手动备份：

```bash
cp build-profile.json5 /tmp/build-profile.debug.bak
cp .ohos/build-profile.release.json5 build-profile.json5
```

### 3. 构建

```bash
make NATIVE_ARCH=arm64-v8a hap    # 出 hap，此时的 hap 已经是发布签名
hvigorw assembleApp               # 打成 app，不需要加参数
```

`assembleApp` 不加参数就行：发布配置里的构建模式只有 release，默认走的就是它。

### 4. 还原

```bash
cp /tmp/build-profile.debug.bak build-profile.json5
git checkout entry/src/main/module.json5
```

**还原之后要重新构建一次调试包**，否则 `entry/build` 里留着的是发布签名的那份，装不到设备上调试。

## 容易踩的点

- **`build-profile.json5` 被版本管理忽略**（`.gitignore` 里同时忽略了 `/build-profile.json5` 和 `/.ohos`），所以没有办法靠 Git 找回被覆盖的内容。换配置之前先备份，换回来之后对比一下确认没弄错。
- **`hvigorw assembleApp` 的产物在 `build/` 目录下面**，`make clean` 会把它一起删掉。做好的发布包要及时挪到别处，不要留在构建目录里等它被清理。
- **配置是单一文件、不分分支的**，但不同分支需要的格式不一样：

  | 分支 | SDK 版本写法 | runtimeOS |
  |---|---|---|
  | `master` / `main-ui` | `"6.1.0(23)"`（带引号的字符串） | `"HarmonyOS"` |
  | `kaihong` | `23`（数字） | `"OpenHarmony"` |

  用错格式的表现是构建在打包阶段报 `unable to update targetSdkVersion`——构建脚本按带引号的格式去匹配，数字形式匹配不上。各版本的备份都在 `.ohos/`，切换分支之前先对一下 `runtimeOS` 这一项。

## 验证签名对不对

用 SDK 里的签名工具验证：

```bash
java -jar <SDK>/toolchains/lib/hap-sign-tool.jar verify-app \
  -inFile wineohos-default-signed.app \
  -outCertChain /tmp/chain.cer -outProfile /tmp/profile.p7b
```

看到 `verify success` 之后，再用 openssl 看证书链里的**叶子证书**：

```bash
openssl pkcs12 -in /tmp/chain.cer -nokeys -passin pass: 2>/dev/null | openssl x509 -noout -subject
```

- 发布签名：`CN=...,Release`
- 调试签名：`CN=...,Development`

两条链的根证书和中间证书是一样的，**只有叶子证书的结尾不同**，所以必须看到叶子证书才能区分。
