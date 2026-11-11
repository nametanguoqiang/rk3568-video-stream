// ============================================================
// ringbuf.h  环形缓冲，单生产者多消费者
//
// 定长槽位，满了丢最旧。每帧数据单独 malloc，消费者用完 free。
// ============================================================

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <pthread.h>

#define RING_CAP 8              // 8 个槽位，15fps 下约 0.5 秒缓冲

struct frame
{
	unsigned char *data;        // 帧数据，malloc 出来的
	size_t         len;         // 有效字节数
	unsigned long  seq;         // 帧序号，调试用
};

struct ringbuf
{
	struct frame slots[RING_CAP];
	int head;                   // 下一个写的位置
	int tail;                   // 下一个读的位置
	int count;                  // 当前有效帧数
	unsigned long put_cnt;      // 累计生产
	unsigned long drop_cnt;     // 累计丢弃
	pthread_mutex_t lock;
	pthread_cond_t  not_empty;
};

void ring_init(struct ringbuf *r);
void ring_destroy(struct ringbuf *r);
int  ring_put(struct ringbuf *r, const void *data, size_t len);
int  ring_get(struct ringbuf *r, struct frame *out, volatile int *stop);
void ring_free(struct frame *f);
void ring_stats(struct ringbuf *r, unsigned long *put, unsigned long *drop);

#endif
