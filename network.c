// ============================================================
// network.c  HTTP MJPEG 推流服务
//
// 一个 accept 线程只管收连接，帧由消费线程统一广播。
// 这样一帧能同时送给所有人，不会出现两个客户端抢同一帧。
// ============================================================

#include "network.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>                     // realloc
#include <time.h>                       // time，算运行秒数
#define BOUNDARY    "frame"             // multipart 分隔串，浏览器靠它切帧

static int g_listen = -1;               // 监听 socket
static int g_clifd[NET_MAX_CLI];        // 已连接的客户端描述符
static int g_clicnt = 0;                // 当前连接数
static pthread_mutex_t g_clilock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_accept_tid;

// ---------- /capture 用的最新帧 ----------
static unsigned char *g_snap = NULL;    // 最新一帧的副本，按需扩容
static size_t         g_snapcap = 0;    // 已分配容量
static size_t         g_snaplen = 0;    // 当前有效长度
static unsigned long  g_snapseq = 0;    // 帧序号，拿来做文件名
static pthread_mutex_t g_snaplock = PTHREAD_MUTEX_INITIALIZER;

// ---------- /status 用的数字 ----------
static double         g_fps = 0.0;
static unsigned long  g_frames = 0;
static unsigned long  g_drop_net = 0;
static unsigned long  g_drop_prev = 0;
static time_t         g_start = 0;

// 一次 send 可能只发出去一部分，必须循环到末尾，否则后面的帧全错位
static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t sent = 0;

	while (sent < len)
	{
		// MSG_NOSIGNAL：对端已关时返回 -1，不抛 SIGPIPE
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
		if (n < 0)
		{
			if (errno == EINTR)             // 被信号打断，重试
				continue;
			return -1;
		}
		if (n == 0)                     // 极少数情况，对端关了但没报错
			return -1;

		sent += n;
	}

	return 0;
}

// 回 200 和 multipart 头，浏览器收到它才知道后面是连续替换的图片
static int send_stream_header(int fd)
{
	const char *hdr =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: close\r\n"
		"\r\n";

	return send_all(fd, hdr, strlen(hdr));
}
// 普通 HTTP 响应：有长度、发完就关，跟推流那种无限长的流不一样
static int send_response(int fd, const char *ctype, const char *body, int blen)
{
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %d\r\n"
			 "Connection: close\r\n"
			 "\r\n", ctype, blen);

	if (send_all(fd, hdr, n) < 0)
		return -1;

	if (blen > 0 && send_all(fd, body, blen) < 0)
		return -1;

	return 0;
}
// 读自己进程的常驻内存，单位 KB。压测时靠它看长时间运行有没有泄漏
static long read_rss_kb(void)
{
	FILE *fp = fopen("/proc/self/statm", "r");
	if (fp == NULL)
		return -1;

	long total = 0;
	long rss   = 0;

	if (fscanf(fp, "%ld %ld", &total, &rss) != 2)
	{
		fclose(fp);
		return -1;
	}
	fclose(fp);

	// statm 的单位是页，页大小一般是 4KB
	return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

// /status 返回 JSON，浏览器用 JS 解析显示
static int handle_status(int fd)
{
	char body[256];
	int n = snprintf(body, sizeof(body),
			 "{\"fps\":%.1f,\"frames\":%lu,\"drop_stream\":%lu,"
			 "\"drop_preview\":%lu,\"clients\":%d,\"uptime\":%ld,\"rss\":%ld}\n",
			 g_fps, g_frames, g_drop_net, g_drop_prev,
			 g_clicnt, (long)(time(NULL) - g_start), read_rss_kb());

	return send_response(fd, "application/json", body, n);
}
// /capture 抓拍一帧，落盘并返回文件名和长度
static int handle_capture(int fd)
{
	char path[128];
	char body[160];
	size_t len = 0;
	unsigned long seq = 0;

	pthread_mutex_lock(&g_snaplock);

	if (g_snap == NULL)                     // 还没采到帧，直接说清楚
	{
		pthread_mutex_unlock(&g_snaplock);
		const char *e = "no frame yet\n";
		return send_response(fd, "text/plain", e, (int)strlen(e));
	}

	snprintf(path, sizeof(path), "/root/snap_%lu.jpg", g_snapseq);

	FILE *fp = fopen(path, "wb");
	if (fp != NULL)
	{
		// 锁里写盘，最坏把采集拖慢几毫秒，换来的是不用再拷一份副本
		fwrite(g_snap, 1, g_snaplen, fp);
		fclose(fp);
	}

	len = g_snaplen;
	seq = g_snapseq;

	pthread_mutex_unlock(&g_snaplock);

	int n = snprintf(body, sizeof(body), "%s %zu\n", path, len);
	printf("[NET] 抓拍 seq=%lu, %zu 字节\n", seq, len);

	return send_response(fd, "text/plain", body, n);
}


// 从请求行里取出路径，形如 "GET /status HTTP/1.1" 取中间那段
static int read_request(int fd, char *path, size_t cap)
{
	char buf[1024];
	ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return -1;

	buf[n] = '\0';

	char *sp1 = strchr(buf, ' ');
	if (sp1 == NULL)
		return -1;

	char *p = sp1 + 1;
	char *sp2 = strchr(p, ' ');
	if (sp2 == NULL)
		return -1;

	size_t len = (size_t)(sp2 - p);
	if (len >= cap)
		return -1;

	memcpy(path, p, len);
	path[len] = '\0';

	return 0;
}

static void *accept_thread(void *arg)
{
	(void)arg;

	while (1)
	{
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);

		int fd = accept(g_listen, (struct sockaddr *)&addr, &len);
		if (fd < 0)
		{
			if (errno == EINTR)             // 被信号打断，继续等
				continue;
			break;                          // 监听被 shutdown，退出
		}

		// 读请求最多等 2 秒，防止连上来不说话的客户端卡死 accept 线程
		struct timeval tv;
		tv.tv_sec  = 2;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		// 发不出去最多等 1 秒，超时就认为这个客户端有问题，广播时会被踢掉
		struct timeval sotv;
		sotv.tv_sec  = 1;
		sotv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sotv, sizeof(sotv));

		char path[64];// 只要路径，不要参数，浏览器访问时会带 ?t=xxx
		if (read_request(fd, path, sizeof(path)) < 0)
		{
			close(fd);
			continue;
		}

		if (strcmp(path, "/status") == 0)       // 只要数字，不占客户端名额
		{
			handle_status(fd);
			close(fd);
			continue;
		}

		if (strncmp(path, "/capture", 8) == 0)  // 抓一张存盘，同样不占名额
		{
			handle_capture(fd);
			close(fd);
			continue;
		}

		if (strcmp(path, "/favicon.ico") == 0)  // 浏览器顺手要的图标
		{
			close(fd);
			continue;
		}

		if (send_stream_header(fd) < 0)         // 其余路径一律当推流
		{
			close(fd);
			continue;
		}

		pthread_mutex_lock(&g_clilock);
		if (g_clicnt >= NET_MAX_CLI)
		{
			pthread_mutex_unlock(&g_clilock);
			close(fd);                      // 满了不接，等别人断
			continue;
		}
		g_clifd[g_clicnt] = fd;
		g_clicnt++;
		printf("[NET] 新客户端 fd=%d，当前 %d 个\n", fd, g_clicnt);
		pthread_mutex_unlock(&g_clilock);
	}

	return NULL;
}

int net_start(int port)
{
	// 往已经关掉的连接里写会触发 SIGPIPE，默认动作是直接杀进程
	signal(SIGPIPE, SIG_IGN);
	// 记录启动时间，/status 用
	g_start = time(NULL);
	
	g_listen = socket(AF_INET, SOCK_STREAM, 0);
	if (g_listen < 0)
	{
		perror("socket");
		return -1;
	}

	// 不设这个，程序刚退再启会 bind 失败，要等一分钟 TIME_WAIT
	int on = 1;
	setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 所有网卡都听，不写死 IP
	addr.sin_port        = htons(port);

	if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		perror("bind");
		close(g_listen);
		g_listen = -1;
		return -1;
	}

	if (listen(g_listen, 8) < 0)            // 8 是排队长度，不是客户端上限
	{
		perror("listen");
		close(g_listen);
		g_listen = -1;
		return -1;
	}

	if (pthread_create(&g_accept_tid, NULL, accept_thread, NULL) != 0)
	{
		perror("pthread_create accept");
		close(g_listen);
		g_listen = -1;
		return -1;
	}

	printf("[NET] 已监听 %d 端口，浏览器打开 http://<板子IP>:%d/\n", port, port);
	return 0;
}

void net_broadcast(const struct frame *f)
{
	if (g_clicnt == 0)                      // 没人看，直接返回，省一次加锁
		return;

	char part[160];
	int n = snprintf(part, sizeof(part),
			 "--" BOUNDARY "\r\n"
			 "Content-Type: image/jpeg\r\n"
			 "Content-Length: %zu\r\n"
			 "\r\n", f->len);

	pthread_mutex_lock(&g_clilock);

    for (int i = 0; i < g_clicnt; i++)
    {
        int fd = g_clifd[i];
        int bad = 0;

        if (send_all(fd, part, n) < 0)          // 分隔串加这一段头部
            bad = 1;
        else if (send_all(fd, f->data, f->len) < 0)     // JPEG 本体
            bad = 1;
        else if (send_all(fd, "\r\n", 2) < 0)   // 每帧后面补一个换行
            bad = 1;

        if (bad)                                // 对端关了或者断网
        {
            close(fd);
            g_clicnt--;
            g_clifd[i] = g_clifd[g_clicnt];     // 用最后一个补位
            i--;                                // 补过来的还没检查
            printf("[NET] 客户端断开，剩 %d 个\n", g_clicnt);
        }
    }

	pthread_mutex_unlock(&g_clilock);
}

void net_stop(void)
{
	if (g_listen < 0)
		return;

	// shutdown 能让阻塞在 accept 上的线程立刻返回，只 close 不保证
	shutdown(g_listen, SHUT_RDWR);
	pthread_join(g_accept_tid, NULL);
	close(g_listen);
	g_listen = -1;

	pthread_mutex_lock(&g_clilock);
	for (int i = 0; i < g_clicnt; i++)
		close(g_clifd[i]);
	g_clicnt = 0;
	pthread_mutex_unlock(&g_clilock);
}
void net_keep_frame(const struct frame *f)
{
	pthread_mutex_lock(&g_snaplock);

	if (f->len > g_snapcap)                 // 容量不够就扩，只在第一帧和变分辨率时发生
	{
		unsigned char *p = realloc(g_snap, f->len);
		if (p == NULL)
		{
			pthread_mutex_unlock(&g_snaplock);
			return;                         // 内存不够就先不留，抓拍会返回没帧
		}
		g_snap = p;
		g_snapcap = f->len;
	}

	memcpy(g_snap, f->data, f->len);
	g_snaplen = f->len;
	g_snapseq = f->seq;

	pthread_mutex_unlock(&g_snaplock);
}

void net_update_stats(double fps, unsigned long frames,
		      unsigned long drop_net, unsigned long drop_prev)
{
	g_fps = fps;
	g_frames = frames;
	g_drop_net = drop_net;
	g_drop_prev = drop_prev;
}
