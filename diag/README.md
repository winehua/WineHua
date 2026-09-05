# diag/

运行时热修组件的源码。这些组件以二进制 zip 形式放进
entry/src/main/resources/rawfile/，由 AppLibraryService 在 prefix 就绪时
自动解压投放（幂等 marker），源码入此处供审阅与重建。

| 文件 | 产物 | 投放通道 |
|---|---|---|
| dnsapi_musl.c | dnsapi.so (wine/bin/x86_64-unix/, 20136B) | rawfile/dnsapi-fix.zip → ensureDnsapiPatch() |

背景: OHOS musl 无 libresolv（res_query 等为同地址桩符号），wine configure
检不出 HAVE_RESOLV，构建产物 dnsapi.so 为 0 符号空壳，DnsQuery_A 直接
0xC0000005。本实现直连 DNS：/etc/resolv.conf 配置（mtime/ino 变更检测）、
raw UDP:53（任意记录类型）、TCP:53 兜底（防 UDP 劫持）、getaddrinfo 合成
兜底（A/AAAA），导出标准 wine unixlib ABI（含 wow64 thunk）。
