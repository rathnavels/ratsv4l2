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
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static int dmabuf_sync(int dmabuf_fd, int start)
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
    if (heap_fd < 0) { perror("open dma_heap"); return -1; }

    struct dma_heap_allocation_data alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;

    if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }
    close(heap_fd);
    return (int)alloc.fd;
}

static int read_exact(int fd, void *dst, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, (uint8_t*)dst + off, len - off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        off += (size_t)r;
    }
    return off == len ? 0 : -1;
}

static int load_file_into_dmabuf(const char *path, int dmabuf_fd, size_t size)
{
    int f = open(path, O_RDONLY | O_CLOEXEC);
    if (f < 0) { perror("open input"); return -1; }

    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (p == MAP_FAILED) { perror("mmap dmabuf"); close(f); return -1; }

    if (dmabuf_sync(dmabuf_fd, 1) < 0) { munmap(p, size); close(f); return -1; }

    if (read_exact(f, p, size) < 0) {
        fprintf(stderr, "%s too small (need %zu bytes)\n", path, size);
        dmabuf_sync(dmabuf_fd, 0);
        munmap(p, size);
        close(f);
        return -1;
    }

    if (dmabuf_sync(dmabuf_fd, 0) < 0) { /* continue */ }

    munmap(p, size);
    close(f);
    return 0;
}

static int set_fmt_i420m(int vfd, enum v4l2_buf_type type, uint32_t w, uint32_t h)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 3;

    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); return -1; }
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M) {
        fprintf(stderr, "Driver changed pixelformat to %.4s\n", (char *)&fmt.fmt.pix_mp.pixelformat);
        return -1;
    }
    if (fmt.fmt.pix_mp.num_planes != 3) {
        fprintf(stderr, "Driver returned num_planes=%u\n", fmt.fmt.pix_mp.num_planes);
        return -1;
    }
    return 0;
}

static int reqbufs(int vfd, enum v4l2_buf_type type, enum v4l2_memory mem, unsigned count)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = type;
    req.memory = mem;
    req.count = count;

    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); return -1; }
    if (req.count < count) { fprintf(stderr, "REQBUFS got %u\n", req.count); return -1; }
    return 0;
}

struct cap_plane_map { void *addr; size_t len; };

static int map_capture_planes(int vfd, unsigned index, struct cap_plane_map plane[3])
{
    struct v4l2_buffer b;
    struct v4l2_plane planes[3];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));

    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    b.length = 3;
    b.m.planes = planes;

    if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { perror("VIDIOC_QUERYBUF cap"); return -1; }

    for (int p = 0; p < 3; p++) {
        plane[p].len = b.m.planes[p].length;
        plane[p].addr = mmap(NULL, plane[p].len, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vfd, b.m.planes[p].m.mem_offset);
        if (plane[p].addr == MAP_FAILED) { perror("mmap cap plane"); return -1; }
    }
    return 0;
}

static int qbuf_capture(int vfd, unsigned index)
{
    struct v4l2_buffer b;
    struct v4l2_plane planes[3];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));

    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    b.length = 3;
    b.m.planes = planes;

    if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF cap"); return -1; }
    return 0;
}

static int qbuf_output_dmabuf_i420m(int vfd, unsigned index,
                                   int y_fd, int u_fd, int v_fd,
                                   unsigned y_size, unsigned u_size, unsigned v_size)
{
    struct v4l2_buffer b;
    struct v4l2_plane planes[3];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));

    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = index;
    b.length = 3;
    b.m.planes = planes;

    planes[0].m.fd = y_fd; planes[0].bytesused = y_size; planes[0].length = y_size;
    planes[1].m.fd = u_fd; planes[1].bytesused = u_size; planes[1].length = u_size;
    planes[2].m.fd = v_fd; planes[2].bytesused = v_size; planes[2].length = v_size;

    if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF out dmabuf"); return -1; }
    return 0;
}

static int stream_on(int vfd, enum v4l2_buf_type type)
{
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return -1; }
    return 0;
}
static int stream_off(int vfd, enum v4l2_buf_type type)
{
    if (xioctl(vfd, VIDIOC_STREAMOFF, &type) < 0) { perror("VIDIOC_STREAMOFF"); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr,
            "Usage:\n  %s in.y in.u in.v out.y out.u out.v\n", argv[0]);
        return 1;
    }

    const char *dev  = "/dev/video2";
    const char *heap = "/dev/dma_heap/system";
    const char *in_y = argv[1];
    const char *in_u = argv[2];
    const char *in_v = argv[3];
    const char *out_y = argv[4];
    const char *out_u = argv[5];
    const char *out_v = argv[6];

    const uint32_t W = 640, H = 480;
    const size_t YSZ = (size_t)W * H;
    const size_t USZ = YSZ / 4;
    const size_t VSZ = YSZ / 4;

    printf("[MODE] system dma-heap -> 3 dmabufs (Y/U/V) -> privcam\n");
    printf("Inputs:  %s %s %s\n", in_y, in_u, in_v);
    printf("Outputs: %s %s %s\n", out_y, out_u, out_v);

    int vfd = open(dev, O_RDWR | O_CLOEXEC);
    if (vfd < 0) { perror("open privcam"); return 1; }

    if (set_fmt_i420m(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  W, H) < 0) return 1;
    if (set_fmt_i420m(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, W, H) < 0) return 1;

    // Allocate dmabufs
    int y_fd = alloc_dmabuf_from_heap(heap, YSZ);
    int u_fd = alloc_dmabuf_from_heap(heap, USZ);
    int v_fd = alloc_dmabuf_from_heap(heap, VSZ);
    if (y_fd < 0 || u_fd < 0 || v_fd < 0) return 1;

    // Load each plane file into its dmabuf
    if (load_file_into_dmabuf(in_y, y_fd, YSZ) < 0) return 1;
    if (load_file_into_dmabuf(in_u, u_fd, USZ) < 0) return 1;
    if (load_file_into_dmabuf(in_v, v_fd, VSZ) < 0) return 1;

    // OUTPUT: DMABUF slots
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_DMABUF, 1) < 0) return 1;

    // CAPTURE: MMAP buffers
    const unsigned cap_count = 2;
    if (reqbufs(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, cap_count) < 0) return 1;

    struct cap_plane_map cap[cap_count][3];
    memset(cap, 0, sizeof(cap));
    for (unsigned i = 0; i < cap_count; i++) {
        if (map_capture_planes(vfd, i, cap[i]) < 0) return 1;
        if (qbuf_capture(vfd, i) < 0) return 1;
    }

    // Queue one OUTPUT frame
    if (qbuf_output_dmabuf_i420m(vfd, 0, y_fd, u_fd, v_fd,
                                (unsigned)YSZ, (unsigned)USZ, (unsigned)VSZ) < 0)
        return 1;

    // Stream
    if (stream_on(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) return 1;
    if (stream_on(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0) return 1;

    // DQ CAPTURE
    struct v4l2_buffer dq;
    struct v4l2_plane dqplanes[3];
    memset(&dq, 0, sizeof(dq));
    memset(dqplanes, 0, sizeof(dqplanes));
    dq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    dq.memory = V4L2_MEMORY_MMAP;
    dq.length = 3;
    dq.m.planes = dqplanes;

    if (xioctl(vfd, VIDIOC_DQBUF, &dq) < 0) { perror("VIDIOC_DQBUF cap"); return 1; }

    int oy = open(out_y, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    int ou = open(out_u, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    int ov = open(out_v, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0644);
    if (oy<0||ou<0||ov<0) { perror("open out"); return 1; }

    write(oy, cap[dq.index][0].addr, dq.m.planes[0].bytesused);
    write(ou, cap[dq.index][1].addr, dq.m.planes[1].bytesused);
    write(ov, cap[dq.index][2].addr, dq.m.planes[2].bytesused);
    close(oy); close(ou); close(ov);

    // DQ OUTPUT (good practice)
    struct v4l2_buffer odq;
    struct v4l2_plane odqplanes[3];
    memset(&odq, 0, sizeof(odq));
    memset(odqplanes, 0, sizeof(odqplanes));
    odq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    odq.memory = V4L2_MEMORY_DMABUF;
    odq.length = 3;
    odq.m.planes = odqplanes;
    xioctl(vfd, VIDIOC_DQBUF, &odq);

    stream_off(vfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    stream_off(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    printf("Wrote: %s %s %s\n", out_y, out_u, out_v);
    return 0;
}