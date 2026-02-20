// cam_to_privcam_dmabuf.c
// Capture one YUYV frame from /dev/video0 (MMAP), export that camera buffer as DMABUF (EXPBUF),
// queue it into privcam OUTPUT using V4L2_MEMORY_DMABUF, dequeue privcam CAPTURE (MMAP), write out.yuyv.

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
#include <unistd.h>


static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
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

    fprintf(stderr, "Set fmt type=%d -> %ux%u fourcc=%c%c%c%c bpl=%u size=%u\n",
            type,
            fmt.fmt.pix.width, fmt.fmt.pix.height,
            fmt.fmt.pix.pixelformat & 0xFF,
            (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
            fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
}

static uint32_t reqbufs(int fd, enum v4l2_buf_type type, enum v4l2_memory mem, uint32_t count)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = count;
    req.type = type;
    req.memory = mem;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1)
        die("VIDIOC_REQBUFS");

    if (req.count < 1) {
        fprintf(stderr, "REQBUFS returned count=%u\n", req.count);
        exit(1);
    }
    return req.count;
}

static struct mmap_buf *map_mmap_buffers(int fd, enum v4l2_buf_type type, uint32_t count)
{
    struct mmap_buf *bufs = calloc(count, sizeof(*bufs));
    if (!bufs) die("calloc");

    for (uint32_t i = 0; i < count; i++) {
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

static void qbuf_mmap(int fd, enum v4l2_buf_type type, uint32_t index, uint32_t bytesused)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    b.bytesused = bytesused;

    if (xioctl(fd, VIDIOC_QBUF, &b) == -1)
        die("VIDIOC_QBUF (MMAP)");
}

static void qbuf_dmabuf(int fd, enum v4l2_buf_type type, uint32_t index,
                        int dmabuf_fd, uint32_t bytesused, uint32_t length)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = index;
    b.m.fd = dmabuf_fd;
    b.bytesused = bytesused;
    b.length = 0;

    if (xioctl(fd, VIDIOC_QBUF, &b) == -1)
        die("VIDIOC_QBUF (DMABUF)");
}

static int dqbuf_any(int fd, enum v4l2_buf_type type, enum v4l2_memory mem, struct v4l2_buffer *out)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = type;
    b.memory = mem;

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

static void export_cam_dmabufs(int cam_fd, uint32_t count, int *fds)
{
    for (uint32_t i = 0; i < count; i++) {
        struct v4l2_exportbuffer exp;
        memset(&exp, 0, sizeof(exp));
        exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exp.index = i;
        exp.plane = 0;
        exp.flags = O_CLOEXEC;

        if (xioctl(cam_fd, VIDIOC_EXPBUF, &exp) == -1) {
            fprintf(stderr, "VIDIOC_EXPBUF failed idx=%u errno=%d (%s)\n",
                    i, errno, strerror(errno));
            die("VIDIOC_EXPBUF");
        }
        fds[i] = exp.fd;
    }
}

int main(int argc, char **argv)
{
    const char *cam_dev = (argc > 1) ? argv[1] : "/dev/video0";
    const char *m2m_dev = (argc > 2) ? argv[2] : "/dev/video2";
    const char *out_path = (argc > 3) ? argv[3] : "out.yuyv";
    uint32_t w = (argc > 4) ? (uint32_t)atoi(argv[4]) : 640;
    uint32_t h = (argc > 5) ? (uint32_t)atoi(argv[5]) : 480;

    const uint32_t pixfmt = V4L2_PIX_FMT_YUYV;
    const uint32_t frame_sz = w * h * 2;

    fprintf(stderr, "[MODE] DMABUF handoff\n");
    fprintf(stderr, "Camera:  %s\nPrivcam: %s\nOut:     %s\nSize:    %ux%u YUYV (%u bytes)\n",
            cam_dev, m2m_dev, out_path, w, h, frame_sz);

    int cam_fd = open(cam_dev, O_RDWR | O_CLOEXEC);
    if (cam_fd < 0) die("open camera");

    int m2m_fd = open(m2m_dev, O_RDWR | O_CLOEXEC);
    if (m2m_fd < 0) die("open privcam");

    // Set formats
    set_fmt(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, w, h, pixfmt);
    set_fmt(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,  w, h, pixfmt);
    set_fmt(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, w, h, pixfmt);

    // Camera: MMAP capture buffers
    const uint32_t cam_req = 4;
    uint32_t cam_count = reqbufs(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, cam_req);
    struct mmap_buf *cam_bufs = map_mmap_buffers(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cam_count);

    // Export camera buffers as DMABUF fds
    int *cam_dmabuf_fds = calloc(cam_count, sizeof(int));
    if (!cam_dmabuf_fds) die("calloc dmabuf_fds");
    for (uint32_t i = 0; i < cam_count; i++) cam_dmabuf_fds[i] = -1;
    export_cam_dmabufs(cam_fd, cam_count, cam_dmabuf_fds);

    // Queue all camera capture buffers
    for (uint32_t i = 0; i < cam_count; i++)
        qbuf_mmap(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);

    stream_on(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    // Privcam CAPTURE: MMAP
    const uint32_t cap_req = 4;
    uint32_t cap_count = reqbufs(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, cap_req);
    struct mmap_buf *cap_bufs = map_mmap_buffers(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cap_count);

    // Queue CAPTURE buffers
    for (uint32_t i = 0; i < cap_count; i++)
        qbuf_mmap(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);

    // Privcam OUTPUT: DMABUF
    uint32_t out_count = reqbufs(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, cam_count);
    if (out_count != cam_count) {
        fprintf(stderr, "Note: privcam OUTPUT DMABUF count=%u (cam_count=%u)\n", out_count, cam_count);
        // We'll still index by camera index; if this differs, we can remap later.
    }

    // Start privcam streaming (both queues)
    stream_on(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    stream_on(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);

    // ---- Process exactly one frame ----

    // 1) DQ one frame from camera
    struct v4l2_buffer cam_dq;
    if (dqbuf_any(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, &cam_dq) == -1)
        die("camera VIDIOC_DQBUF");

    uint32_t cam_bytes = cam_dq.bytesused ? cam_dq.bytesused : frame_sz;
    if (cam_bytes > frame_sz) cam_bytes = frame_sz;

    // 2) Queue that camera buffer's DMABUF fd into privcam OUTPUT (no memcpy)
    // Use same index as camera's index (simple & works when counts match).
    qbuf_dmabuf(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                cam_dq.index,
                cam_dmabuf_fds[cam_dq.index],
                cam_bytes,
                frame_sz);

    // 3) DQ CAPTURE from privcam
    struct v4l2_buffer cap_dq;
    if (dqbuf_any(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, &cap_dq) == -1)
        die("privcam CAPTURE VIDIOC_DQBUF");

    uint32_t out_bytes = cap_dq.bytesused ? cap_dq.bytesused : frame_sz;
    if (out_bytes > cap_bufs[cap_dq.index].len) out_bytes = cap_bufs[cap_dq.index].len;

    int out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd < 0) die("open out file");
    ssize_t wr = write(out_fd, cap_bufs[cap_dq.index].addr, out_bytes);
    if (wr < 0) die("write out");
    close(out_fd);

    fprintf(stderr, "Wrote %zd bytes to %s\n", wr, out_path);

    // 4) DQ privcam OUTPUT (recycle)
    struct v4l2_buffer out_dq;
    if (dqbuf_any(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, &out_dq) == -1)
        die("privcam OUTPUT VIDIOC_DQBUF");

    // Requeue camera buffer and stop
    if (xioctl(cam_fd, VIDIOC_QBUF, &cam_dq) == -1)
        die("camera re-QBUF");

    stream_off(m2m_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    stream_off(m2m_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    stream_off(cam_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    for (uint32_t i = 0; i < cam_count; i++)
        if (cam_dmabuf_fds[i] >= 0) close(cam_dmabuf_fds[i]);

    close(m2m_fd);
    close(cam_fd);
    return 0;
}
