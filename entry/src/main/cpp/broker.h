#ifndef WINE_BROKER_H
#define WINE_BROKER_H

// 启动 Process Broker Unix socket server（在后台线程运行）
// 返回 0 表示成功，非 0 表示失败
int StartBrokerServer();

#endif // WINE_BROKER_H
