// cam_to_privcam.c
// Capture one YUYV frame from /dev/video0 (MMAP) and push it through privcam /dev/video2 (MMAP).
// Output: out.yuyv (raw YUYV frame)

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

struct mmap_buf {
    void  *addr;
    size_t len;
};

static void set_fmt(int fd, enum v4l2_buf_type type, uint32_t w, uint32_t h, uint32_t pixfmt)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1)
        die("VIDIOC_S_FMT");

    // Print negotiated format
    fprintf(stderr, "Set fmt type=%d -> %ux%u fourcc=%c%c%c%c bytesperline=%u sizeimage=%u\n",
            type,
            fmt.fmt.pix.width, fmt.fmt.pix.height,
            fmt.fmt.pix.pixelformat & 0xFF,
            (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
            fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
}

static struct mmap_buf *req_mmap_buffers(int fd, enum v4l2_buf_type type, int count)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = count;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1)
        die("VIDIOC_REQBUFS");

    if (req.count < 1) {
        fprintf(stderr, "REQBUFS returned count=%u\n", req.count);
        exit(1);
    }

    struct mmap_buf *bufs = calloc(req.count, sizeof(*bufs));
    if (!bufs)
        die("calloc bufs");

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;

        if (xioctl(fd, VIDIOC_QUERYBUF, &b) == -1)
            die("VIDIOC_QUERYBUF");

        bufs[i].len = b.length;
        bufs[i].addr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bufs[i].addr == MAP_FAILED)
            die("mmap");
    }

    return bufs;
}

static void queue_all(int fd, enum v4l2_buf_type type, int count, size_t payload_bytes)
{
    for (int i = 0; i < count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.bytesused = payload_bytes; // for OUTPUT this matters; for CAPTURE it's ok
        if (xioctl(fd, VIDIOC_QBUF, &b) == -1)
            die("VIDIOC_QBUF");
    }
}

static int dqbuf_one(int fd, enum v4l2_buf_type type, struct v4l2_buffer *out)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_DQBUF, &b) == -1)
        return -1;

    *out = b;
    return 0;
}

static void stream_on(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1)
        die("VIDIOC_STREAMON");
}

static void stream_off(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) == -1)
        die("VIDIOC_STREAMOFF");
}

int main(int argc, char **argv)
{
    const char *cam_dev = (argc > 1) ? argv[1] : "/dev/video0";
    const char *m2m_dev = (argc > 2) ? argv[2] : "/dev/video2";
    const char *out_path = (argc > 3) ? argv[3] : "out.yuyv";
    uint32_t w = (argc > 4) ? (uint32_t)atoi(argv[4]) : 640;
    uint32_t h = (argc > 5) ? (uint32_t)atoi(argv[5]) : 480;

    const uint32_t pixfmt = V4L2_PIX_FMT_YUYV;
    const size_t frame_sz = (size_t)w * (size_t)h * 2; // YUYV = 2 bytes/pixel

    fprintf(stderr, "Camera:  %s\nPrivcam: %s\nOut:     %s\nSize:    %ux%u YUYV (%zu bytes)\n",
            cam_dev, m2m_dev, out_path, w, h, frame_sz);

    // Open camera
    int cam_fd = open(cam_dev, O_RDWR | O_CLOEXEC);
    if (cam_fd < 0) die("open camera");

    // Open privcam mem2mem
    int m2m_fd = open(m2m_dev, O_RDWR | O_CLOEXEC);
    if (m2m_fd < 0) die("open privcam");

    // Set formats
    set_fmt(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, w, h, pixfmt);

    set_fmt(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,  w, h, pixfmt);
    set_fmt(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, w, h, pixfmt);

    // Camera buffers (capture)
    int cam_buf_count = 4;
    struct mmap_buf *cam_bufs = req_mmap_buffers(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cam_buf_count);
    // queue camera capture bufs
    for (int i = 0; i < cam_buf_count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(cam_fd, VIDIOC_QBUF, &b) == -1)
            die("camera VIDIOC_QBUF");
    }
    stream_on(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    // Dequeue one frame from camera
    struct v4l2_buffer cam_dq;
    if (dqbuf_one(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cam_dq) == -1)
        die("camera VIDIOC_DQBUF");

    if (cam_dq.bytesused < frame_sz) {
        fprintf(stderr, "Warning: camera bytesused=%u < expected=%zu\n",
                cam_dq.bytesused, frame_sz);
    }

    uint8_t *cam_frame = (uint8_t *)cam_bufs[cam_dq.index].addr;
    size_t cam_bytes = cam_dq.bytesused ? cam_dq.bytesused : frame_sz;

    // Privcam buffers: OUTPUT + CAPTURE
    int out_count = 4, cap_count = 4;
    struct mmap_buf *out_bufs = req_mmap_buffers(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, out_count);
    struct mmap_buf *cap_bufs = req_mmap_buffers(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cap_count);

    // Queue CAPTURE buffers first (payload doesn't matter yet)
    for (int i = 0; i < cap_count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(m2m_fd, VIDIOC_QBUF, &b) == -1)
            die("privcam CAPTURE VIDIOC_QBUF");
    }

    // Copy camera frame into privcam OUTPUT buffer 0
    int out_idx = 0;
    size_t copy_sz = cam_bytes;
    if (copy_sz > out_bufs[out_idx].len)
        copy_sz = out_bufs[out_idx].len;

    memcpy(out_bufs[out_idx].addr, cam_frame, copy_sz);

    // Queue OUTPUT buffer 0 with bytesused
    struct v4l2_buffer out_q;
    memset(&out_q, 0, sizeof(out_q));
    out_q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_q.memory = V4L2_MEMORY_MMAP;
    out_q.index = out_idx;
    out_q.bytesused = (uint32_t)copy_sz;
    if (xioctl(m2m_fd, VIDIOC_QBUF, &out_q) == -1)
        die("privcam OUTPUT VIDIOC_QBUF");

    // Start streaming on privcam (both queues)
    stream_on(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    stream_on(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);

    // Dequeue processed CAPTURE buffer
    struct v4l2_buffer cap_dq;
    if (dqbuf_one(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cap_dq) == -1)
        die("privcam CAPTURE VIDIOC_DQBUF");

    size_t out_bytes = cap_dq.bytesused ? cap_dq.bytesused : frame_sz;
    if (out_bytes > cap_bufs[cap_dq.index].len)
        out_bytes = cap_bufs[cap_dq.index].len;

    // Write output to file
    int out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) die("open out file");

    ssize_t wr = write(out_fd, cap_bufs[cap_dq.index].addr, out_bytes);
    if (wr < 0) die("write out");
    close(out_fd);

    fprintf(stderr, "Wrote %zd bytes to %s\n", wr, out_path);

    // Cleanup
    stream_off(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    stream_off(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    // Re-queue camera buffer back and stop camera
    if (xioctl(cam_fd, VIDIOC_QBUF, &cam_dq) == -1)
        die("camera re-QBUF");
    stream_off(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    close(m2m_fd);
    close(cam_fd);

    return 0;
}
