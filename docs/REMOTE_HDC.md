hdc 远程共享配置总结

场景

主机 A（192.168.1.5）连着鸿蒙模拟器，让主机 B 也能用 hdc 控制它。

配置方法

hdc server 强制绑定 127.0.0.1，无法直接监听外部网口，需要用 Windows 端口转发来桥接。

---
主机 A 配置（一次性，需管理员）

# 1. 端口转发：外部 IP 收到的 8710 请求转发到本地 hdc server
```
netsh interface portproxy add v4tov4 listenaddress=192.168.1.5 listenport=8710 connectaddress=127.0.0.1 connectport=8710
```

# 2. 开放防火墙
```
netsh advfirewall firewall add rule name="hdc server" dir=in action=allow protocol=tcp localport=8710
```

主机 B 使用

# 方式1：环境变量（一次设置，整个终端生效）
```
export HDC_SERVER=192.168.1.5:8710   # Linux/Mac
set HDC_SERVER=192.168.1.5:8710      # Windows cmd

hdc list targets
hdc shell
```

# 方式2：-s 临时指定（单条命令）
```
hdc -s 192.168.1.5:8710 list targets
hdc -s 192.168.1.5:8710 shell
```

验证

# 主机 B 上应该能看到设备
```
hdc -s 192.168.1.5:8710 list targets -v
```

清理（不再需要时）

```
netsh interface portproxy delete v4tov4 listenaddress=192.168.1.5 listenport=8710
netsh advfirewall firewall delete rule name="hdc server"
```
