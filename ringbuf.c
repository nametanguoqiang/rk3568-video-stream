// ============================================================
// ringbuf.c  环形缓冲实现
//
// 锁的边界：malloc 和 memcpy 在锁外，锁里只做指针操作。
// ============================================================

#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

void ring_init(struct ringbuf *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->not_empty, NULL);
}

void ring_destroy(struct ringbuf *r)
{
	// 把还没被消费的帧释放掉，否则泄漏
	while (r->count > 0)
	{
		free(r->slots[r->tail].data);
		r->tail = (r->tail + 1) % RING_CAP;
		r->count--;
	}
	pthread_mutex_destroy(&r->lock);
	pthread_cond_destroy(&r->not_empty);
}

int ring_put(struct ringbuf *r, const void *data, size_t len)
{
	// 先拷一份，40KB 的 memcpy 不放锁里，否则消费者要等
	unsigned char *copy = malloc(len);
	if (copy == NULL)
		return -1;
	memcpy(copy, data, len);

	pthread_mutex_lock(&r->lock);

	if (r->count == RING_CAP)
	{
		// 满了，丢最旧的：tail 往前推一格
		free(r->slots[r->tail].data);
		r->tail = (r->tail + 1) % RING_CAP;
		r->count--;
		r->drop_cnt++;
	}

	r->slots[r->head].data = copy;
	r->slots[r->head].len  = len;
	r->slots[r->head].seq  = r->put_cnt;
	r->head = (r->head + 1) % RING_CAP;
	r->count++;
	r->put_cnt++;

	pthread_cond_signal(&r->not_empty);
	pthread_mutex_unlock(&r->lock);

	return 0;
}

int ring_get(struct ringbuf *r, struct frame *out, volatile int *stop)
{
	pthread_mutex_lock(&r->lock);

	// while 不是 if：条件变量会虚假唤醒，醒来要重新检查
	while (r->count == 0 && !*stop)
	{
		// 用 timedwait，否则 *stop 置位后线程永远醒不来
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 200 * 1000000L;               // 等 200ms
		if (ts.tv_nsec >= 1000000000L)
		{
			ts.tv_sec  += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&r->not_empty, &r->lock, &ts);
	}

	if (r->count == 0)                              // 被 stop 叫醒的，不是有数据
	{
		pthread_mutex_unlock(&r->lock);
		return -1;
	}

	*out = r->slots[r->tail];
	r->tail = (r->tail + 1) % RING_CAP;
	r->count--;

	pthread_mutex_unlock(&r->lock);

	return 0;
}

void ring_free(struct frame *f)
{
	free(f->data);
	f->data = NULL;
	f->len  = 0;
}

void ring_stats(struct ringbuf *r, unsigned long *put, unsigned long *drop)
{
	pthread_mutex_lock(&r->lock);
	*put  = r->put_cnt;
	*drop = r->drop_cnt;
	pthread_mutex_unlock(&r->lock);
}
