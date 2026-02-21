#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
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

static int dmabuf_sync(int dmabuf_fd, int start)  // start=1 BEGIN, start=0 END
{
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = DMA_BUF_SYNC_RW | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END);
    if (xioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        perror("DMA_BUF_IOCTL_SYNC");
        return -1;
    }
    return 0;
}

static int alloc_dmabuf_from_heap(const char *heap_path, size_t size)
{
    int heap_fd = open(heap_path, O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0) {
        perror("open /dev/dma_heap/system");
        return -1;
    }

    struct dma_heap_allocation_data alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    alloc.heap_flags = 0;

    if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    return (int)alloc.fd; // dmabuf fd
}

static int read_exact(int fd, void *dst, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, (uint8_t *)dst + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    return (off == len) ? 0 : -1;
}

struct mmap_buf {
    void *addr;
    size_t len;
};

static int set_fmt(int vfd, enum v4l2_buf_type type, uint32_t w, uint32_t h, uint32_t fourcc)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    if (fmt.fmt.pix.pixelformat != fourcc) {
        fprintf(stderr, "Driver changed pixel format to %.4s\n", (char *)&fmt.fmt.pix.pixelformat);
        return -1;
    }

    return 0;
}

static int reqbufs(int vfd, enum v4l2_buf_type type, enum v4l2_memory mem, unsigned int count)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = type;
    req.memory = mem;
    req.count = count;

    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    if (req.count < count) {
        fprintf(stderr, "VIDIOC_REQBUFS: requested %u got %u\n", count, req.count);
        return -1;
    }
    return 0;
}

static int map_capture_mmap(int vfd, struct mmap_buf *bufs, unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;

        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF (cap)");
            return -1;
        }

        bufs[i].len = b.length;
        bufs[i].addr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        if (bufs[i].addr == MAP_FAILED) {
            perror("mmap (cap)");
            return -1;
        }
    }
    return 0;
}

static int qbuf_capture(int vfd, unsigned int index)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;

    if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) {
        perror("VIDIOC_QBUF (cap)");
        return -1;
    }
    return 0;
}

static int qbuf_output_dmabuf(int vfd, unsigned int index, int dmabuf_fd, size_t bytesused)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = index;
    b.m.fd = dmabuf_fd;
    b.bytesused = (unsigned int)bytesused;

    if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) {
        perror("VIDIOC_QBUF (out dmabuf)");
        return -1;
    }
    return 0;
}

static int stream_on(int vfd, enum v4l2_buf_type type)
{
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

static int stream_off(int vfd, enum v4l2_buf_type type)
{
    if (xioctl(vfd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *privcam_dev = "/dev/video2";
    const char *heap_path = "/dev/dma_heap/system";
    const char *in_path = (argc > 1) ? argv[1] : "in.yuyv";
    const char *out_path = (argc > 2) ? argv[2] : "out.yuyv";

    const uint32_t W = 640, H = 480;
    const uint32_t FOURCC = V4L2_PIX_FMT_YUYV;
    const size_t FRAME_SZ = (size_t)W * (size_t)H * 2;

    printf("[MODE] dma-heap system -> DMABUF -> privcam\n");
    printf("Input:  %s\n", in_path);
    printf("Output: %s\n", out_path);
    printf("Dev:    %s\n", privcam_dev);
    printf("Heap:   %s\n", heap_path);
    printf("Frame:  %ux%u YUYV (%zu bytes)\n", W, H, FRAME_SZ);

    int vfd = open(privcam_dev, O_RDWR | O_CLOEXEC);
    if (vfd < 0) { perror("open privcam"); return 1; }

    // Set both formats (privcam supports same fmt on both in your driver)
    if (set_fmt(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT,  W, H, FOURCC) < 0) return 1;
    if (set_fmt(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, W, H, FOURCC) < 0) return 1;

    // Allocate one dmabuf for OUTPUT
    int dmabuf_fd = alloc_dmabuf_from_heap(heap_path, FRAME_SZ);
    if (dmabuf_fd < 0) return 1;

    void *dmabuf_map = mmap(NULL, FRAME_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (dmabuf_map == MAP_FAILED) { perror("mmap dmabuf"); return 1; }

    // Load disk file into dmabuf
    int infd = open(in_path, O_RDONLY | O_CLOEXEC);
    if (infd < 0) { perror("open input"); return 1; }

    if (dmabuf_sync(dmabuf_fd, 1) < 0) return 1;
    if (read_exact(infd, dmabuf_map, FRAME_SZ) < 0) {
        fprintf(stderr, "Failed to read %zu bytes from %s (file too small?)\n", FRAME_SZ, in_path);
        return 1;
    }
    if (dmabuf_sync(dmabuf_fd, 0) < 0) return 1;

    close(infd);

    // OUTPUT: request queue slots for DMABUF
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, 1) < 0) return 1;

    // CAPTURE: request MMAP buffers
    const unsigned int cap_count = 2;
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, cap_count) < 0) return 1;

    struct mmap_buf cap[cap_count];
    memset(cap, 0, sizeof(cap));
    if (map_capture_mmap(vfd, cap, cap_count) < 0) return 1;

    // Queue all CAPTURE buffers
    for (unsigned int i = 0; i < cap_count; i++) {
        if (qbuf_capture(vfd, i) < 0) return 1;
    }

    // Queue OUTPUT buffer (dmabuf fd)
    if (qbuf_output_dmabuf(vfd, 0, dmabuf_fd, FRAME_SZ) < 0) return 1;

    // Stream ON: CAPTURE then OUTPUT (either order usually ok; this is common)
    if (stream_on(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE) < 0) return 1;
    if (stream_on(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT) < 0) return 1;

    // Dequeue CAPTURE (processed frame)
    struct v4l2_buffer cap_dq;
    memset(&cap_dq, 0, sizeof(cap_dq));
    cap_dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_dq.memory = V4L2_MEMORY_MMAP;

    if (xioctl(vfd, VIDIOC_DQBUF, &cap_dq) < 0) {
        perror("VIDIOC_DQBUF (cap)");
        return 1;
    }

    // Dequeue OUTPUT (consume input)
    struct v4l2_buffer out_dq;
    memset(&out_dq, 0, sizeof(out_dq));
    out_dq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_dq.memory = V4L2_MEMORY_DMABUF;

    if (xioctl(vfd, VIDIOC_DQBUF, &out_dq) < 0) {
        perror("VIDIOC_DQBUF (out)");
        return 1;
    }

    printf("CAPTURE dq: index=%u bytesused=%u\n", cap_dq.index, cap_dq.bytesused);

    int outfd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (outfd < 0) { perror("open out"); return 1; }

    if (cap_dq.bytesused > cap[cap_dq.index].len) {
        fprintf(stderr, "bytesused bigger than buffer!? (%u > %zu)\n", cap_dq.bytesused, cap[cap_dq.index].len);
        return 1;
    }

    if (write(outfd, cap[cap_dq.index].addr, cap_dq.bytesused) != (ssize_t)cap_dq.bytesused) {
        perror("write out");
        return 1;
    }
    close(outfd);

    // Stream OFF
    stream_off(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    stream_off(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);

    // Cleanup
    for (unsigned int i = 0; i < cap_count; i++) {
        if (cap[i].addr && cap[i].addr != MAP_FAILED)
            munmap(cap[i].addr, cap[i].len);
    }
    munmap(dmabuf_map, FRAME_SZ);
    close(dmabuf_fd);
    close(vfd);

    printf("Wrote %s\n", out_path);
    return 0;
}