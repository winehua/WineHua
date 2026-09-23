# hdc 远程共享配置

> 更新: 2026-07-31（内容源自早期临时笔记，格式化整理）
> 相关: `.claude/rules/build-and-log.md` 的「hdc 连接方式」小节（`hdc -s <server_ip> -t <target_ip>` 直连约定）

## 场景

主机 A（192.168.1.5）连着鸿蒙模拟器，让主机 B 也能用 hdc 控制它。

## 原理

hdc server 强制绑定 `127.0.0.1`，无法直接监听外部网口，需要用 Windows 端口转发桥接。

## 主机 A 配置（一次性，需管理员）

1. 端口转发：外部 IP 收到的 8710 请求转发到本地 hdc server

```bash
netsh interface portproxy add v4tov4 listenaddress=192.168.1.5 listenport=8710 connectaddress=127.0.0.1 connectport=8710
```

2. 开放防火墙

```bash
netsh advfirewall firewall add rule name="hdc server" dir=in action=allow protocol=tcp localport=8710
```

## 主机 B 使用

**方式 1：环境变量**（一次设置，整个终端生效）

```bash
export HDC_SERVER=192.168.1.5:8710   # Linux/Mac
set HDC_SERVER=192.168.1.5:8710      # Windows cmd

hdc list targets
hdc shell
```

**方式 2：`-s` 临时指定**（单条命令）

```bash
hdc -s 192.168.1.5:8710 list targets
hdc -s 192.168.1.5:8710 shell
```

## 验证

主机 B 上应该能看到设备：

```bash
hdc -s 192.168.1.5:8710 list targets -v
```

## 清理（不再需要时）

```bash
netsh interface portproxy delete v4tov4 listenaddress=192.168.1.5 listenport=8710
netsh advfirewall firewall delete rule name="hdc server"
```
