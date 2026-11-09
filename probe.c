// ============================================================
// probe.c  探测 V4L2 设备能力：驱动名、能力位、支持格式、支持分辨率
//
// 用法: ./probe [设备节点]
// 例子: ./probe /dev/video0
//
// 实现要点：
// 1. 能力位要看 device_caps 不是 capabilities，新版驱动真实能力在后一个
// 2. 先判断多平面还是单平面，后面查格式要用对应的 type
// 3. ENUM_FMT 是「翻书式」枚举：index 从 0 递增，问到驱动返回失败为止
// ============================================================

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

// 能力位是 32 位整数，一位代表一种能力，用表翻译比 if-else 清爽
static void print_caps(unsigned int caps)
{
    struct { unsigned int bit; const char *name; } tab[] = {
        { V4L2_CAP_VIDEO_CAPTURE,        "VIDEO_CAPTURE  单平面采集" },
        { V4L2_CAP_VIDEO_CAPTURE_MPLANE, "VIDEO_CAPTURE_MPLANE  多平面采集" },
        { V4L2_CAP_STREAMING,            "STREAMING  支持 mmap 流式 IO" },
        { V4L2_CAP_READWRITE,            "READWRITE  支持 read/write" },
        { V4L2_CAP_VIDEO_M2M,            "VIDEO_M2M  内存到内存" },
    };

    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (caps & tab[i].bit)                  // 按位与，这一位是 1 就支持
            printf("  - %s\n", tab[i].name);
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";

    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("QUERYCAP");
        return 1;
    }

    printf("=== %s ===\n", dev);
    printf("driver  : %s\n", cap.driver);
    printf("card    : %s\n", cap.card);
    printf("bus     : %s\n", cap.bus_info);

    // 带 V4L2_CAP_DEVICE_CAPS 标志时，capabilities 里是整张卡的汇总，
    // 这个节点自己的真实能力在 device_caps 里，必须读后者
    unsigned int caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    printf("caps    : 0x%08x\n", caps);
    print_caps(caps);

    enum v4l2_buf_type type;

    // MPLANE 要先判断，因为同时支持两种能力的设备优先用多平面
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;   // rkisp_mainpath 走这条
    else if (caps & V4L2_CAP_VIDEO_CAPTURE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;          // USB 摄像头走这条
    else {
        printf("\n这不是采集节点\n");
        close(fd);
        return 0;
    }

    printf("\n支持的格式:\n");

    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;                            // 查哪种类型的格式

    for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {

        // pixelformat 是 32 位整数，拆成 4 个字节就是四个字符，比如 NV12
        printf("  [%u] %c%c%c%c  %s\n", fmt.index,
               fmt.pixelformat & 0xff,
               (fmt.pixelformat >> 8)  & 0xff,
               (fmt.pixelformat >> 16) & 0xff,
               (fmt.pixelformat >> 24) & 0xff,
               fmt.description);

        struct v4l2_frmsizeenum fs;
        memset(&fs, 0, sizeof(fs));
        fs.pixel_format = fmt.pixelformat;      // 查这个格式支持哪些分辨率

        for (fs.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                printf("        %ux%u\n", fs.discrete.width, fs.discrete.height);
            else                                // STEPWISE：范围内任意分辨率
                printf("        %ux%u ~ %ux%u\n",
                       fs.stepwise.min_width, fs.stepwise.min_height,
                       fs.stepwise.max_width, fs.stepwise.max_height);
        }
    }

    // 查当前生效的格式，能看出驱动默认给的是多少
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = type;

    if (ioctl(fd, VIDIOC_G_FMT, &f) == 0) {
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            printf("\n当前: %ux%u  %c%c%c%c  planes=%u  bytesperline=%u\n",
                   f.fmt.pix_mp.width, f.fmt.pix_mp.height,
                   f.fmt.pix_mp.pixelformat & 0xff,
                   (f.fmt.pix_mp.pixelformat >> 8) & 0xff,
                   (f.fmt.pix_mp.pixelformat >> 16) & 0xff,
                   (f.fmt.pix_mp.pixelformat >> 24) & 0xff,
                   f.fmt.pix_mp.num_planes,
                   f.fmt.pix_mp.plane_fmt[0].bytesperline);
        else
            printf("\n当前: %ux%u  %c%c%c%c  bytesperline=%u\n",
                   f.fmt.pix.width, f.fmt.pix.height,
                   f.fmt.pix.pixelformat & 0xff,
                   (f.fmt.pix.pixelformat >> 8) & 0xff,
                   (f.fmt.pix.pixelformat >> 16) & 0xff,
                   (f.fmt.pix.pixelformat >> 24) & 0xff,
                   f.fmt.pix.bytesperline);
    }

    close(fd);
    return 0;
}
