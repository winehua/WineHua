#ifndef WINE_BROKER_H
#define WINE_BROKER_H

#include <string>
#include <vector>

// 全局: Broker 构造 NCP entryParams 时加会话 home/prefix (由 LaunchPadMode 设置)。
// NCP 不继承发起 Wine 进程的环境，所以 prefix 必须由 broker 显式重放。
extern std::string gBrokerHomeDir;
extern std::string gBrokerPrefixDir;

// 启动 Process Broker Unix socket server（在后台线程运行）
// 返回 0 表示成功，非 0 表示失败
int StartBrokerServer();

#endif // WINE_BROKER_H
