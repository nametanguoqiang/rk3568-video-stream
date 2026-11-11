// ============================================================
// capture.c  V4L2 采集，存成 jpg
//
// 用法: ./capture <设备节点> <像素格式> <输出前缀> <帧数> [宽x高]
// 例子: ./capture /dev/video9 MJPG /root/frame 1
//       ./capture /dev/video9 MJPG /root/frame 1 640x480
//
// 实现要点：
// 1. open 用 O_NONBLOCK，后面配 select 等数据，不忙等
// 2. 4 块缓冲 + mmap 零拷贝，每帧用完必须 QBUF 还回去，否则第 4 帧后卡死
// 3. 采集线程只做 DQBUF → ring_put → QBUF，不做任何耗时操作
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include "ringbuf.h"

#define BUFFER_COUNT    4				// 4 块够周转，2 块容易等，再多浪费内存

// ---------- 缓冲描述 ----------
struct buffer
{
	void  *start;						// mmap 返回的地址
	size_t length;						// munmap 时要原样传回
};

// ---------- 辅助函数 ----------

// ioctl 被信号打断时 errno 是 EINTR，命令其实没执行，要重试
static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
	{
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

// "MJPG" 四个字符打包成一个 32 位整数，V4L2 用这种方式表示像素格式
static unsigned int fourcc_code(const char *s)
{
	return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}
static struct ringbuf g_ring;           // 全局环形缓冲，采集和消费共享
static volatile int   g_stop = 0;       // 置 1 通知消费者退出

// 消费者线程：故意睡 20ms，比采集慢，用来测丢帧
static void *consumer(void *arg)
{
	(void)arg;
	struct frame f;
	unsigned long consumed = 0;

	while (ring_get(&g_ring, &f, &g_stop) == 0)
	{
		usleep(100000);		// 模拟慢速处理，100ms 一帧，比采集的 66ms 慢，队列才会堆
		ring_free(&f);
		consumed++;
	}

	printf("\n消费者退出，共消费 %lu 帧\n", consumed);
	return NULL;
}

// ---------- 主函数 ----------
int main(int argc, char **argv)
{
	// 关掉 stdout 缓冲：ssh 执行时 stdout 不是终端会变成全缓冲，
	// 日志全憋到退出才输出，和 stderr 顺序错乱
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc != 5 && argc != 6)
	{
		fprintf(stderr, "用法: %s <设备节点> <像素格式> <输出前缀> <帧数> [宽x高]\n", argv[0]);
		fprintf(stderr, "例子: %s /dev/video9 MJPG /root/frame 1\n", argv[0]);
		fprintf(stderr, "      %s /dev/video9 MJPG /root/frame 1 640x480\n", argv[0]);
		return 1;
	}

	const char *dev_name = argv[1];					// 设备节点，比如 /dev/video9
	unsigned int pixfmt  = fourcc_code(argv[2]);	// 像素格式，MJPG 或 YUYV
	const char *prefix   = argv[3];				// 输出文件前缀，落盘交给消费线程，这里暂时用不到
	(void)prefix;					// 压掉 unused 警告，第 7 步接回来
	int want_frames      = atoi(argv[4]);			// 采几帧

	unsigned int want_w = 1280;						// 默认 720p
	unsigned int want_h = 720;

	if (argc == 6)									// 传了分辨率就按传的来
	{
		if (sscanf(argv[5], "%ux%u", &want_w, &want_h) != 2)
		{
			fprintf(stderr, "分辨率格式错误，应该写成 640x480 这样\n");
			return 1;
		}
	}

	if (want_frames <= 0)
	{
		fprintf(stderr, "帧数必须是正整数\n");
		return 1;
	}

	// ---------- [1] 打开设备 ----------

	// O_NONBLOCK：某些驱动在 open 阶段会等硬件初始化，阻塞模式可能卡住
	int fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
	{
		perror("open");
		return 1;
	}
	printf("[1] 设备已打开: %s (fd=%d)\n", dev_name, fd);

	// ---------- [2] 查询设备能力 ----------

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));					// 不清零的话结构里有垃圾值

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
	{
		perror("VIDIOC_QUERYCAP");
		return 1;
	}

	printf("[2] 驱动: %s\n", cap.driver);
	printf("    卡名: %s\n", cap.card);
	printf("    总线: %s\n", cap.bus_info);

	// USB 摄像头会建两个节点，元数据节点没有采集能力，传错这里就拦住了
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		fprintf(stderr, "错误: %s 不是视频采集节点\n", dev_name);
		return 1;
	}

	// mmap 属于流式 IO，驱动不支持就没法零拷贝
	if (!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		fprintf(stderr, "错误: %s 不支持流式 IO，不能用 mmap\n", dev_name);
		return 1;
	}

	// ---------- [3] 设置采集格式 ----------

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));

	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;	// 单平面，USB 摄像头用这个
	fmt.fmt.pix.width       = want_w;
	fmt.fmt.pix.height      = want_h;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;		// 扫描方式交给驱动决定

	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
	{
		perror("VIDIOC_S_FMT");
		return 1;
	}

	// S_FMT 是商量不是命令，驱动可能改了分辨率但不报错，必须读回来确认
	printf("[3] 实际格式: %c%c%c%c %ux%u, 每行字节 %u, 图像大小 %u\n",
		   (fmt.fmt.pix.pixelformat      ) & 0xff,
		   (fmt.fmt.pix.pixelformat >>  8) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 16) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 24) & 0xff,
		   fmt.fmt.pix.width,
		   fmt.fmt.pix.height,
		   fmt.fmt.pix.bytesperline,
		   fmt.fmt.pix.sizeimage);

	// 不设的话驱动用默认帧率，这颗摄像头默认给 15fps
	struct v4l2_streamparm parm;
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator   = 1;		// 分子 1
	parm.parm.capture.timeperframe.denominator = 30;	// 分母 30，就是 1/30 秒一帧

	if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
	{
		perror("VIDIOC_S_PARM");
	}
	else
	{
		// S_PARM 也是商量，必须读回来确认驱动真给了 30
		printf("[3.5] 帧率: %u/%u\n",
			   parm.parm.capture.timeperframe.numerator,
			   parm.parm.capture.timeperframe.denominator);
	}

	// S_PARM 回填的值不可信，用 G_PARM 读驱动实际记的是多少
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0)
	{
		printf("[3.6] 设完读回: %u/%u\n",
			   parm.parm.capture.timeperframe.numerator,
			   parm.parm.capture.timeperframe.denominator);
	}

	// ---------- [4] 申请缓冲并映射 ----------

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));

	req.count  = BUFFER_COUNT;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;					// mmap 方式，不是 read 拷贝

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
	{
		perror("VIDIOC_REQBUFS");
		return 1;
	}

	printf("[4] 驱动实际分配 %u 块缓冲\n", req.count);	// 内存紧张时可能少给

	if (req.count < 2)
	{
		fprintf(stderr, "错误: 缓冲数量不足，至少需要 2 块\n");
		return 1;
	}

	struct buffer *buffers = calloc(req.count, sizeof(*buffers));
	if (buffers == NULL)
	{
		perror("calloc");
		return 1;
	}

	for (unsigned int i = 0; i < req.count; i++)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));

		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;								// 问第几块

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
		{
			perror("VIDIOC_QUERYBUF");
			return 1;
		}

		buffers[i].length = buf.length;

		// MAP_SHARED：和驱动共享同一块物理内存，驱动写进来我能看见
		// buf.m.offset：这块缓冲在设备内存里的偏移，只有驱动知道
		buffers[i].start = mmap(NULL, buf.length,
								PROT_READ | PROT_WRITE,
								MAP_SHARED, fd, buf.m.offset);

		if (buffers[i].start == MAP_FAILED)
		{
			perror("mmap");
			return 1;
		}

		// 空缓冲交给驱动，不入队的话 STREAMON 会因为没缓冲可用直接失败
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
		{
			perror("VIDIOC_QBUF");
			return 1;
		}

		printf("    缓冲 %u: 长度 %u 字节\n", i, buf.length);
	}

	printf("[4] %u 块缓冲已映射并入队\n", req.count);

	// ---------- [5] 开流 ----------

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
	{
		perror("VIDIOC_STREAMON");
		return 1;
	}
	printf("[5] 已开流，驱动开始采集\n");

	// 开流之后再读一次，看 UVC 带宽协商有没有偷偷改帧率
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0)
	{
		printf("[5.5] 开流后帧率: %u/%u\n",
			   parm.parm.capture.timeperframe.numerator,
			   parm.parm.capture.timeperframe.denominator);
	}
	ring_init(&g_ring);

	pthread_t tid;
	if (pthread_create(&tid, NULL, consumer, NULL) != 0)
	{
		perror("pthread_create");
		return 1;
	}
	// ---------- [6] 循环取帧 ----------

	for (int n = 0; n < want_frames; n++)
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		struct timeval tv;
		tv.tv_sec  = 5;								// 2 秒不够，摄像头要初始化自动曝光
		tv.tv_usec = 0;

		// 非阻塞模式下 DQBUF 没数据会立即返回 EAGAIN，用 select 睡着等更省 CPU
		int r = select(fd + 1, &fds, NULL, NULL, &tv);

		if (r < 0)
		{
			perror("select");
			break;
		}
		if (r == 0)
		{
			fprintf(stderr, "等待超时: 5 秒内没有拿到帧\n");
			break;
		}

		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0)
		{
			perror("VIDIOC_DQBUF");
			break;
		}

		// bytesused 是这一帧的有效字节数，比缓冲总长度小得多
		// 拷进环形缓冲，采集线程只管放，不管谁消费
		ring_put(&g_ring, buffers[buf.index].start, buf.bytesused);

		printf("[6] 取到第 %d 帧, 入队 seq=%lu\n", n, g_ring.put_cnt - 1);

		// 用完必须还回去，不还的话缓冲用一块少一块，第 4 帧之后就取不到数据
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
		{
			perror("VIDIOC_QBUF 归还");
			break;
		}
	}

	// 通知消费者退出，等它把缓冲里的帧取完再往下走
	g_stop = 1;
	pthread_join(tid, NULL);

	unsigned long put_cnt, drop_cnt;
	ring_stats(&g_ring, &put_cnt, &drop_cnt);		// 两个数由函数写回

	printf("\n===== 环形缓冲统计 =====\n");
	printf("生产 : %lu 帧\n", put_cnt);
	printf("丢弃 : %lu 帧\n", drop_cnt);
	printf("======================\n");

	ring_destroy(&g_ring);

	// ---------- [7] 收尾清理 ----------

	xioctl(fd, VIDIOC_STREAMOFF, &type);

	for (unsigned int i = 0; i < req.count; i++)
	{
		munmap(buffers[i].start, buffers[i].length);	// 长度必须和 mmap 时一致
	}
	free(buffers);

	close(fd);

	printf("[7] 已清理，正常退出\n");
	return 0;
}
