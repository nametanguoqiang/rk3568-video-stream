// ============================================================
// preview.h  framebuffer 本地预览的对外接口
//
// 预览线程自己吃一条队列，跟推流那条完全独立。
// ============================================================

#ifndef PREVIEW_H
#define PREVIEW_H

#include "ringbuf.h"

struct preview_ctx
{
	struct ringbuf *ring;               // 预览专用的那条队列
	volatile int   *stop;               // 指向采集那边的退出标志
};

int  preview_init(const char *fbdev);   // 打开 fb、读参数、映射显存
void *preview_thread(void *arg);        // 线程体，参数是 struct preview_ctx *
void preview_deinit(void);              // 解除映射、关 fd

#endif
