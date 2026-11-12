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

#define BOUNDARY    "frame"             // multipart 分隔串，浏览器靠它切帧

static int g_listen = -1;               // 监听 socket
static int g_clifd[NET_MAX_CLI];        // 已连接的客户端描述符
static int g_clicnt = 0;                // 当前连接数
static pthread_mutex_t g_clilock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_accept_tid;

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

// 浏览器会顺手要 /favicon.ico，那种连接不发流，直接关掉省一个名额
static int is_favicon(int fd)
{
	char buf[512];
	ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return 1;

	buf[n] = '\0';
	if (strstr(buf, "favicon") != NULL)
		return 1;

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

		if (is_favicon(fd))                 // 不是来看画面的，别占名额
		{
			close(fd);
			continue;
		}

		if (send_stream_header(fd) < 0)     // 先把头发出去，浏览器才进解码状态
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
