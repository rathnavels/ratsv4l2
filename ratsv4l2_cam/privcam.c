
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-dma-sg.h>

#define PRIVCAM_DEF_WIDTH 640
#define PRIVCAM_DEF_HEIGHT 480
#define PRIVCAM_DEF_PIXFMT V4L2_PIX_FMT_YUYV
#define PRIVCAM_BPP 2

#define PRIVCAM_CAPS (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_DEVICE_CAPS | V4L2_CAP_STREAMING)

#define BUFTYPE_OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define BUFTYPE_CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

#define USE_DMABUF 1

struct privcam_dev {
    struct v4l2_device v4l2_dev;
    struct video_device vdev;
    struct v4l2_m2m_dev *m2m_dev;
    
    struct mutex lock;
};

struct privcam_ctx {
    struct v4l2_fh fh;
    struct privcam_dev *dev;
    struct v4l2_m2m_ctx *m2m_ctx;
    
    struct v4l2_pix_format_mplane out_fmt;
    struct v4l2_pix_format_mplane cap_fmt;
    
    u32 sequence;
    spinlock_t qlock; /* vb2 queue lock */
};

static struct privcam_dev *privcam;
static struct platform_device *pdev;

/* static size_t privcam_sg_copy(struct sg_table *src, struct sg_table *dst, size_t bytes)
{
    struct sg_mapping_iter s, d;
    size_t copied = 0;

    sg_miter_start(&s, src->sgl, src->nents, SG_MITER_FROM_SG);
    sg_miter_start(&d, dst->sgl, dst->nents, SG_MITER_TO_SG);

    if (!sg_miter_next(&s) || !sg_miter_next(&d))
        goto out;

    while (copied < bytes) {
        size_t chunk = bytes - copied;

        if (chunk > s.length) chunk = s.length;
        if (chunk > d.length) chunk = d.length;

        memcpy(d.addr, s.addr, chunk);
        copied += chunk;

        s.addr = (char *)s.addr + chunk;
        s.length -= chunk;
        d.addr = (char *)d.addr + chunk;
        d.length -= chunk;

        if (s.length == 0) {
            if (!sg_miter_next(&s))
                break;
        }
        if (d.length == 0) {
            if (!sg_miter_next(&d))
                break;
        }
    }

out:
    sg_miter_stop(&d);
    sg_miter_stop(&s);
    return copied;
}


static void privcam_device_run(void *priv)
{
    struct privcam_ctx *ctx = priv;
    struct vb2_v4l2_buffer *src, *dst;
#ifdef USE_DMABUF
    struct sg_table *src_sgt;
    //struct sg_table *dst_sgt;
    size_t copied;
    void *dst_vaddr;
#else
    void *src_vaddr, *dst_vaddr;
#endif
    u32 sz;

    src = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
    dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

    if (!src || !dst)
        goto finish;

    sz = min(vb2_get_plane_payload(&src->vb2_buf, 0),
                vb2_plane_size(&dst->vb2_buf, 0));
    
#ifdef USE_DMABUF
    src_sgt = vb2_dma_sg_plane_desc(&src->vb2_buf, 0);
//    dst_sgt = vb2_dma_sg_plane_desc(&src->vb2_buf, 0);
#else
    src_vaddr = vb2_plane_vaddr(&src->vb2_buf, 0);
#endif

    dst_vaddr = vb2_plane_vaddr(&dst->vb2_buf, 0);

#ifdef USE_DMABUF
    if (!src_sgt || !dst_vaddr) {
#else
    if (!src_vaddr || !dst_vaddr) {
#endif
        v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
        v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
        goto finish;
    }

#ifdef USE_DMABUF
    // copy sg -> linear 
    copied = 0;
    {
        struct sg_mapping_iter s;
        size_t remain = sz;
        uint8_t *out = dst_vaddr;

        sg_miter_start(&s, src_sgt->sgl, src_sgt->nents, SG_MITER_FROM_SG);
        while (remain && sg_miter_next(&s)) {
            size_t chunk = s.length;
            if (chunk > remain) chunk = remain;
            memcpy(out, s.addr, chunk);
            out += chunk;
            remain -= chunk;
            copied += chunk;
        }
        sg_miter_stop(&s);
    }
#endif

#ifdef USE_DMABUF
    //copied = privcam_sg_copy(src_sgt, dst_sgt, sz);
    if(copied != sz)
    {
        v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
        v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
        goto finish;
    }
#else    
    memcpy(dst_vaddr, src_vaddr, sz);
#endif

    vb2_set_plane_payload(&dst->vb2_buf, 0, sz);

    dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
    dst->sequence = ctx->sequence++;
    src->sequence = dst->sequence;

    v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
    v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);

finish:
    v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
} */

static int privcam_job_ready(void *priv)
{
    struct privcam_ctx *ctx = priv;

    return (v4l2_m2m_num_src_bufs_ready(ctx->m2m_ctx) > 0) &&
            (v4l2_m2m_num_dst_bufs_ready(ctx->m2m_ctx) > 0);
}

static void privcam_device_run(void *priv)
{
    struct privcam_ctx *ctx = priv;
    struct vb2_v4l2_buffer *src, *dst;

    src = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
    dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
    if (!src || !dst)
        goto finish;

    for (unsigned int p = 0; p < src->vb2_buf.num_planes; p++) {
        struct sg_table *src_sgt = vb2_dma_sg_plane_desc(&src->vb2_buf, p);
        void *dst_vaddr = vb2_plane_vaddr(&dst->vb2_buf, p);

        u32 sz = min(vb2_get_plane_payload(&src->vb2_buf, p),
                     vb2_plane_size(&dst->vb2_buf, p));

        if (!src_sgt || !dst_vaddr) {
            v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
            v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
            goto finish;
        }

        /* SG -> linear copy */
        size_t copied = 0;
        struct sg_mapping_iter it;
        size_t remain = sz;
        u8 *out = dst_vaddr;

        sg_miter_start(&it, src_sgt->sgl, src_sgt->nents, SG_MITER_FROM_SG);
        while (remain && sg_miter_next(&it)) {
            size_t chunk = it.length;
            if (chunk > remain) chunk = remain;
            memcpy(out, it.addr, chunk);
            out += chunk;
            remain -= chunk;
            copied += chunk;
        }
        sg_miter_stop(&it);

        if (copied != sz) {
            v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
            v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
            goto finish;
        }

        vb2_set_plane_payload(&dst->vb2_buf, p, sz);
    }

    dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
    dst->sequence = ctx->sequence++;
    src->sequence = dst->sequence;

    v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
    v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);

finish:
    v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
}

static void privcam_job_abort(void *priv)
{
    struct privcam_ctx *ctx = priv;
    v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->m2m_ctx);
}

static const struct v4l2_m2m_ops privcam_m2m_ops = {
    .device_run = privcam_device_run,
    .job_ready = privcam_job_ready,
    .job_abort = privcam_job_abort,
};

static inline struct privcam_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
    return container_of(fh, struct privcam_ctx, fh);
}

static int privcam_queue_setup(struct vb2_queue *vq,
                                unsigned int *nbufs,
                                unsigned int *nplanes,
                                unsigned int sizes[],
                                struct device *alloc_devs[])
{
    struct privcam_ctx *ctx = vb2_get_drv_priv(vq);
    struct v4l2_pix_format_mplane *pf;

    if (vq->type == BUFTYPE_OUT)
        pf = &ctx->out_fmt;
    else
        pf = &ctx->cap_fmt;

    *nplanes = pf->num_planes;

    for(unsigned int i = 0; i < *nplanes; i++)
        sizes[i] = pf->plane_fmt[i].sizeimage;

    if(*nbufs < 2)
        *nbufs = 2;

    return 0;    
}

static int privcam_buf_prepare(struct vb2_buffer *vb)
{
    struct privcam_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
    struct v4l2_pix_format_mplane *pf;

    if (vb->vb2_queue->type == BUFTYPE_OUT)
        pf = &ctx->out_fmt;
    else
        pf = &ctx->cap_fmt;

    if(vb->num_planes != pf->num_planes)
        return -EINVAL;

    for (unsigned int i = 0; i < pf->num_planes; i++) {
        if (vb2_plane_size(vb, i) < pf->plane_fmt[i].sizeimage)
            return -EINVAL;
        vb2_set_plane_payload(vb, i, pf->plane_fmt[i].sizeimage);
    }

    return 0;
}

static void privcam_buf_queue(struct vb2_buffer *vb)
{
    struct privcam_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
    struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

    v4l2_m2m_buf_queue(ctx->m2m_ctx, vbuf);
}

static int privcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
    return 0;
}

static void privcam_stop_streaming(struct vb2_queue *vq)
{
    struct privcam_ctx *ctx = vb2_get_drv_priv(vq);
    struct vb2_v4l2_buffer *buf;


    if(vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        while ((buf = v4l2_m2m_src_buf_remove(ctx->m2m_ctx)))
            v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
    } else {
         while ((buf = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx)))
            v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
    }
}

static const struct vb2_ops privcam_vb2_ops = {
    .queue_setup = privcam_queue_setup,
    .buf_prepare = privcam_buf_prepare,
    .buf_queue = privcam_buf_queue,
    .start_streaming = privcam_start_streaming,
    .stop_streaming = privcam_stop_streaming,
    .wait_prepare = vb2_ops_wait_prepare,
    .wait_finish = vb2_ops_wait_finish,
};

static int privcam_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
    struct privcam_ctx *ctx = priv;
    int ret;

    // Output/Source queue
    src_vq->type = BUFTYPE_OUT;
    src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
    src_vq->drv_priv = ctx;
    src_vq->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
    src_vq->ops = &privcam_vb2_ops;
#ifdef USE_DMABUF
    src_vq->mem_ops = &vb2_dma_sg_memops;
#else
    src_vq->mem_ops = &vb2_dma_contig_memops;
#endif
    src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    src_vq->lock = &ctx->dev->lock;
 //   src_vq->dev = ctx->dev->v4l2_dev.dev;
    src_vq->dev = &pdev->dev;

    ret = vb2_queue_init(src_vq);
    if(ret)
    {
        pr_err("Src queue init failed\n");
        return ret;
    }

    // Dest/Capture queue
    dst_vq->type = BUFTYPE_CAP;
    dst_vq->io_modes = VB2_MMAP;
    dst_vq->drv_priv = ctx;
    dst_vq->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
    dst_vq->ops = &privcam_vb2_ops;
#ifdef USE_DMABUF
    dst_vq->mem_ops = &vb2_vmalloc_memops;
#else
    dst_vq->mem_ops = &vb2_dma_contig_memops;
#endif
    dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    dst_vq->lock = &ctx->dev->lock;
    dst_vq->dev = ctx->dev->v4l2_dev.dev;
    
    return vb2_queue_init(dst_vq);
    
}

static void privcam_fill_fmt(struct v4l2_pix_format_mplane *mp, u32 w, u32 h, u32 fourcc)
{
    memset(mp, 0, sizeof(*mp));
    mp->width = w;
    mp->height = h;
    mp->pixelformat = fourcc;

    mp->field = V4L2_FIELD_NONE;
    mp->colorspace = V4L2_COLORSPACE_SRGB;

    if(fourcc == V4L2_PIX_FMT_YUV420M) {
        mp->num_planes = 3;
        
        mp->plane_fmt[0].bytesperline = w;
        mp->plane_fmt[0].sizeimage = w * h;

        mp->plane_fmt[1].bytesperline = w / 2;
        mp->plane_fmt[1].sizeimage = (w * h)/4;

        mp->plane_fmt[2].bytesperline = w / 2;
        mp->plane_fmt[2].sizeimage = (w * h)/4;
    } else {
        mp->num_planes = 1;

        mp->plane_fmt[0].bytesperline = w * 2;
        mp->plane_fmt[0].sizeimage = (w * h) * 2;
    }
}

static int privcam_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
    if(f->index != 0)
        return -EINVAL;

    f->pixelformat = PRIVCAM_DEF_PIXFMT;
    return 0;
}

static int privcam_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct v4l2_fh *fh = priv;
    struct privcam_ctx *ctx = container_of(fh, struct privcam_ctx, fh);
    struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

    if (f->type == BUFTYPE_OUT)
        *pix = ctx->out_fmt;
    else
        *pix = ctx->cap_fmt;

    return 0;
}

static int privcam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{

    struct v4l2_pix_format_mplane *mp = &f->fmt.pix_mp;
    u32 w, h;

    w = mp->width ? mp->width : PRIVCAM_DEF_WIDTH;
    h = mp->height ? mp->height : PRIVCAM_DEF_HEIGHT;

    /* YUYV needs even width */
    w &= ~1U;
    h &= ~1U;

    privcam_fill_fmt(mp, w, h, mp->pixelformat);
    return 0;
}

static int privcam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct v4l2_fh *fh = priv;
    struct privcam_ctx *ctx = container_of(fh, struct privcam_ctx, fh);
    int ret;

    ret = privcam_try_fmt(file, priv, f);
    if(ret)
        return ret;

    if(f->type == BUFTYPE_OUT)
        ctx->out_fmt = f->fmt.pix_mp;
    else
        ctx->cap_fmt = f->fmt.pix_mp;

    return 0;
}

static int privcam_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
    strscpy(cap->driver, "privcam", sizeof(cap->driver));
    strscpy(cap->card, "Private Camera", sizeof(cap->card));
    strscpy(cap->bus_info, "platform:privcam", sizeof(cap->bus_info));

    cap->device_caps = PRIVCAM_CAPS;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

    return 0;
}

static const struct v4l2_ioctl_ops privcam_ioctl_ops = {
/*     .vidioc_enum_fmt_vid_cap_mplane = privcam_enum_fmt,
    .vidioc_enum_fmt_vid_out_mplane = privcam_enum_fmt, */

    .vidioc_g_fmt_vid_cap_mplane = privcam_g_fmt,
    .vidioc_g_fmt_vid_out_mplane = privcam_g_fmt,

    .vidioc_try_fmt_vid_cap_mplane = privcam_try_fmt,
    .vidioc_try_fmt_vid_out_mplane = privcam_try_fmt,

    .vidioc_s_fmt_vid_cap_mplane = privcam_s_fmt,
    .vidioc_s_fmt_vid_out_mplane = privcam_s_fmt,

    .vidioc_querycap = privcam_querycap,

    .vidioc_reqbufs       = v4l2_m2m_ioctl_reqbufs,
    .vidioc_create_bufs   = v4l2_m2m_ioctl_create_bufs,
    .vidioc_querybuf      = v4l2_m2m_ioctl_querybuf,
    .vidioc_qbuf          = v4l2_m2m_ioctl_qbuf,
    .vidioc_dqbuf         = v4l2_m2m_ioctl_dqbuf,
    .vidioc_streamon      = v4l2_m2m_ioctl_streamon,
    .vidioc_streamoff     = v4l2_m2m_ioctl_streamoff,
    .vidioc_expbuf        = v4l2_m2m_ioctl_expbuf,

};


static int privcam_open(struct file *file)
{
    struct privcam_dev *dev = video_drvdata(file);
    struct privcam_ctx *ctx;
    int ret;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if(!ctx)
        return -ENOMEM;

    spin_lock_init(&ctx->qlock);

    v4l2_fh_init(&ctx->fh, &dev->vdev);
    v4l2_fh_add(&ctx->fh);

    file->private_data = &ctx->fh;

    ctx->dev = dev;

    privcam_fill_fmt(&ctx->out_fmt, PRIVCAM_DEF_WIDTH, PRIVCAM_DEF_HEIGHT, PRIVCAM_DEF_PIXFMT);
    privcam_fill_fmt(&ctx->cap_fmt, PRIVCAM_DEF_WIDTH, PRIVCAM_DEF_HEIGHT, PRIVCAM_DEF_PIXFMT);

    ctx->m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, privcam_queue_init);
    if(IS_ERR(ctx->m2m_ctx)) {
        ret = PTR_ERR(ctx->m2m_ctx);
        ctx->m2m_ctx = NULL;
        v4l2_fh_del(&ctx->fh);
        v4l2_fh_exit(&ctx->fh);

        kfree(ctx);
        return ret;
    }

    ctx->fh.m2m_ctx = ctx->m2m_ctx;

    return 0;
}

static int privcam_release(struct file *file)
{
    struct v4l2_fh *fh = file->private_data;
    struct privcam_ctx *ctx = container_of(fh, struct privcam_ctx, fh);

    if(ctx->m2m_ctx) {
        ctx->fh.m2m_ctx = NULL;
        v4l2_m2m_ctx_release(ctx->m2m_ctx);
    }

    v4l2_fh_del(&ctx->fh);
    v4l2_fh_exit(fh);

    kfree(ctx);
    return 0;
}

static const struct v4l2_file_operations privcam_fops = {
    .owner = THIS_MODULE,
    .open = privcam_open,
    .release = privcam_release,
    .poll = v4l2_m2m_fop_poll,
    .unlocked_ioctl = video_ioctl2,
    .mmap = v4l2_m2m_fop_mmap,   // checkgdc
};


static int __init privcam_init(void)
{
    int ret = 0;

    pr_err("Entering Init\n");

    privcam = kzalloc(sizeof(*privcam), GFP_KERNEL);
    if(!privcam)
        return -ENOMEM;

    mutex_init(&privcam->lock);

    pdev = platform_device_register_simple("privcam", -1, NULL, 0);
    if(IS_ERR(pdev)) {
        ret = PTR_ERR(pdev);
        goto err_free;
    }

    pr_err("Cleared pdev registration\n");

    // When this name was not given, it was having issues.
    strscpy(privcam->v4l2_dev.name, "privcam", sizeof(privcam->v4l2_dev.name));

    // v4l2 device register must be done with a parent device which is the platform device, it may compile with NULL
    // but has issues during insmod.
    ret = v4l2_device_register(&pdev->dev, &privcam->v4l2_dev);
    if(ret)
    {
        pr_err("V4l2 device registration failed\n");
        goto err_pdev;
    }

    pr_err("Cleared v4l2 dev registration\n");

    privcam->m2m_dev = v4l2_m2m_init(&privcam_m2m_ops);
    if (IS_ERR(privcam->m2m_dev)) {
        ret = PTR_ERR(privcam->m2m_dev);
        privcam->m2m_dev = NULL;
        pr_err("v4l2_m2m_init failed: %d\n", ret);
        goto err_v4l2;
    }

    pr_err("Cleared v4l2-m2m init\n");

    strscpy(privcam->vdev.name, "privcam", sizeof(privcam->vdev.name));

    privcam->vdev.v4l2_dev = &privcam->v4l2_dev;
    privcam->vdev.fops = &privcam_fops;
    privcam->vdev.ioctl_ops = &privcam_ioctl_ops;
    privcam->vdev.release = video_device_release_empty;
    privcam->vdev.lock = &privcam->lock;
    privcam->vdev.vfl_dir = VFL_DIR_M2M; 
    privcam->vdev.dev_parent = &pdev->dev;
    privcam->vdev.device_caps = PRIVCAM_CAPS;

    video_set_drvdata(&privcam->vdev, privcam);

    ret = video_register_device(&privcam->vdev, VFL_TYPE_VIDEO, -1);
    if(ret)
        goto err_video;

    pr_err("Cleared video dev registration\n");

    pr_info("privcam registered as /dev/video%d\n", privcam->vdev.num);
    return 0;

err_video:
    if(privcam->m2m_dev)
        v4l2_m2m_release(privcam->m2m_dev);
err_v4l2:
    v4l2_device_unregister(&privcam->v4l2_dev);
err_pdev:
    platform_device_unregister(pdev);
    pdev = NULL;
err_free:
    kfree(privcam);
    privcam = NULL;
    return ret;

}

static void __exit privcam_exit(void)
{

    video_unregister_device(&privcam->vdev);
    if(privcam->m2m_dev)
        v4l2_m2m_release(privcam->m2m_dev);
    v4l2_device_unregister(&privcam->v4l2_dev);
    platform_device_unregister(pdev);
    kfree(privcam);

    pr_info("privcam unloaded\n");
}

module_init(privcam_init);
module_exit(privcam_exit);


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Privacy Camera V4l2 M2M Skeleton");
MODULE_AUTHOR("RATH");