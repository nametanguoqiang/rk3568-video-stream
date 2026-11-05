// ---------- 头文件：用到哪个函数就 include 哪个 ----------
#include <stdio.h>              // printf、perror（打印和打印错误原因）
#include <string.h>             // memset（把结构体清零）
#include <fcntl.h>              // open 的 O_RDWR、O_NONBLOCK 标志
#include <unistd.h>             // open、close、read、write 这些系统调用
#include <sys/ioctl.h>          // ioctl（给设备发命令）
#include <linux/videodev2.h>    // V4L2 的全部结构体和命令码，核心头文件

// ---------- 把 capability 的位标志翻译成人话 ----------
static void print_caps(unsigned int caps)
{
    // 定义一张表：每一位对应一个名字
    // 为什么用表？因为能力标志是一堆二进制位，用 if-else 写会很长
    struct { unsigned int bit; const char *name; } tab[] = {
        { V4L2_CAP_VIDEO_CAPTURE,        "VIDEO_CAPTURE  单平面采集" },
        { V4L2_CAP_VIDEO_CAPTURE_MPLANE, "VIDEO_CAPTURE_MPLANE  多平面采集" },
        { V4L2_CAP_STREAMING,            "STREAMING  支持 mmap 流式 IO" },
        { V4L2_CAP_READWRITE,            "READWRITE  支持 read/write" },
        { V4L2_CAP_VIDEO_M2M,            "VIDEO_M2M  内存到内存" },
    };
    // sizeof(tab)/sizeof(tab[0]) = 数组总字节数 ÷ 单个元素字节数 = 元素个数
    for (unsigned i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        // caps & bit 是「按位与」：这一位是 1 就说明支持这个能力
        if (caps & tab[i].bit) printf("  - %s\n", tab[i].name);
}

int main(int argc, char **argv)
{
    // argc = 参数个数（至少是 1，因为程序名本身算一个）
    // argv[1] = 第一个参数，也就是你想探测的设备路径
    // 没传参数就默认用 /dev/video0
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";

    // open 打开设备节点，返回文件描述符（一个整数，后面所有操作都靠它）
    // O_RDWR      = 读写方式打开
    // O_NONBLOCK  = 非阻塞，读不到数据立刻返回，不卡住
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }   // perror 会打印「open: 错误原因」

    // 准备一个结构体装设备信息，必须先清零，否则里面的随机值会干扰
    struct v4l2_capability cap;
    // VIDIOC_QUERYCAP = 「查询设备能力」这个命令码
    // ioctl(设备fd, 命令, 数据缓冲区地址)
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return 1; }

    printf("=== %s ===\n", dev);              // 打印设备路径
    printf("driver  : %s\n", cap.driver);     // 驱动名，比如 rkisp
    printf("card    : %s\n", cap.card);       // 设备名，比如 rkisp_mainpath
    printf("bus     : %s\n", cap.bus_info);   // 总线信息

    // 新版驱动会把「真实能力」放在 device_caps 里，
    // 如果 capabilities 里带 V4L2_CAP_DEVICE_CAPS 这个标志，就要读 device_caps
    unsigned int caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    printf("caps    : 0x%08x\n", caps);       // 打印十六进制的原始值
    print_caps(caps);                          // 翻译成人话

    // 判断这个节点是哪种采集类型
    enum v4l2_buf_type type;
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;   // rkisp_mainpath 走这条
    else if (caps & V4L2_CAP_VIDEO_CAPTURE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;          // 普通 USB 摄像头走这条
    else { printf("\n这不是采集节点\n"); close(fd); return 0; }  // 都不是就退出

    printf("\n支持的格式:\n");
    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));   // 清零，sizeof(fmt) 自动算结构体大小
    fmt.type = type;                // 告诉驱动我要查哪种类型的格式

    // ENUM_FMT = 枚举格式，像翻书一样：index 从 0 开始，一直问到驱动返回失败为止
    for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {
        // pixelformat 是个 32 位整数，把 4 个字节拆出来就是四个字符，比如 NV12
        printf("  [%u] %c%c%c%c  %s\n", fmt.index,
               fmt.pixelformat & 0xff,              // 第 1 个字符
               (fmt.pixelformat >> 8)  & 0xff,      // 第 2 个字符
               (fmt.pixelformat >> 16) & 0xff,      // 第 3 个字符
               (fmt.pixelformat >> 24) & 0xff,      // 第 4 个字符
               fmt.description);                    // 驱动给的中文/英文描述

        // 顺便查一下这个格式支持哪些分辨率
        struct v4l2_frmsizeenum fs;
        memset(&fs, 0, sizeof(fs));
        fs.pixel_format = fmt.pixelformat;   // 要指定查哪个格式的分辨率
        for (fs.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                // 离散型：只支持列出来的这几个分辨率
                printf("        %ux%u\n", fs.discrete.width, fs.discrete.height);
            else
                // 步进型：在这个范围内任意分辨率都行
                printf("        %ux%u ~ %ux%u\n", fs.stepwise.min_width, fs.stepwise.min_height,
                       fs.stepwise.max_width, fs.stepwise.max_height);
        }
    }

    // 查当前生效的格式
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &f) == 0) {   // G_FMT = Get Format，读当前格式
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            // 多平面用 pix_mp 这个成员，字段名和单平面不一样
            printf("\n当前: %ux%u  %c%c%c%c  planes=%u  bytesperline=%u\n",
                   f.fmt.pix_mp.width, f.fmt.pix_mp.height,
                   f.fmt.pix_mp.pixelformat & 0xff, (f.fmt.pix_mp.pixelformat >> 8) & 0xff,
                   (f.fmt.pix_mp.pixelformat >> 16) & 0xff, (f.fmt.pix_mp.pixelformat >> 24) & 0xff,
                   f.fmt.pix_mp.num_planes,              // 有几个平面（NV12 是 1 个）
                   f.fmt.pix_mp.plane_fmt[0].bytesperline);  // 一行多少字节，后面 MPP 要用
        else
            // 单平面用 pix 这个成员
            printf("\n当前: %ux%u  %c%c%c%c  bytesperline=%u\n",
                   f.fmt.pix.width, f.fmt.pix.height,
                   f.fmt.pix.pixelformat & 0xff, (f.fmt.pix.pixelformat >> 8) & 0xff,
                   (f.fmt.pix.pixelformat >> 16) & 0xff, (f.fmt.pix.pixelformat >> 24) & 0xff,
                   f.fmt.pix.bytesperline);
    }

    close(fd);   // 用完必须关，否则文件描述符泄漏
    return 0;    // 0 表示正常退出
}