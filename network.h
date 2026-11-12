// ============================================================
// network.h  HTTP MJPEG 推流服务的对外接口
//
// 只在 capture.c 里 include，内部实现不暴露。
// ============================================================

#ifndef NETWORK_H
#define NETWORK_H

#include "ringbuf.h"

#define NET_PORT    8080                // 监听端口，浏览器直接访问
#define NET_MAX_CLI 4                   // 最多同时连 4 个客户端

int  net_start(int port);                   // 建监听并起 accept 线程
void net_broadcast(const struct frame *f);  // 把一帧送给所有客户端
void net_stop(void);                        // 关监听、断开所有客户端

#endif
