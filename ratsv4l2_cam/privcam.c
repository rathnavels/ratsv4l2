
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define PRIVCAM_DEF_WIDTH 640
#define PRIVCAM_DEF_HEIGHT 480
#define PRIVCAM_DEF_PIXFMT V4L2_PIX_FMT_RGBX32
#define PRIVCAM_BPP 4

#define PRIVCAM_CAPS (V4L2_CAP_VIDEO_M2M | V4L2_CAP_DEVICE_CAPS)

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
    
    struct v4l2_pix_format out_fmt;
    struct v4l2_pix_format cap_fmt;
    
    u32 sequence;
};

static struct privcam_dev *privcam;
static struct platform_device *pdev;

static void privcam_device_run(void *priv)
{

}

static void privcam_job_abort(void *priv)
{

}

static const struct v4l2_m2m_ops privcam_m2m_ops = {
    .device_run = privcam_device_run,
    .job_abort = privcam_job_abort,
};

static void privcam_fill_fmt(struct v4l2_pix_format *f)
{
    f->width = PRIVCAM_DEF_WIDTH;
    f->height = PRIVCAM_DEF_HEIGHT;
    f->pixelformat = PRIVCAM_DEF_PIXFMT;

    f->field = V4L2_FIELD_NONE;
    f->bytesperline = f->width * 4;
    f->sizeimage = f->bytesperline * f->height;
    f->colorspace = V4L2_COLORSPACE_SRGB;
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
    struct privcam_ctx *ctx = priv;
    struct v4l2_pix_format *pix = &f->fmt.pix;

    if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
        *pix = ctx->out_fmt;
    else
        *pix = ctx->cap_fmt;

    return 0;
}

static int privcam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct v4l2_pix_format *pix = &f->fmt.pix;

    if(pix->pixelformat != PRIVCAM_DEF_PIXFMT)
        return -EINVAL;

    privcam_fill_fmt(pix);

    return 0;
}

static int privcam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct privcam_ctx *ctx = priv;
    int ret;

    ret = privcam_try_fmt(file, priv, f);
    if(ret)
        return ret;

    if(f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
        ctx->out_fmt = f->fmt.pix;
    else
        ctx->cap_fmt = f->fmt.pix;

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
    .vidioc_enum_fmt_vid_cap = privcam_enum_fmt,
    .vidioc_enum_fmt_vid_out = privcam_enum_fmt,

    .vidioc_g_fmt_vid_cap = privcam_g_fmt,
    .vidioc_g_fmt_vid_out = privcam_g_fmt,

    .vidioc_try_fmt_vid_cap = privcam_try_fmt,
    .vidioc_try_fmt_vid_out = privcam_try_fmt,

    .vidioc_s_fmt_vid_cap = privcam_s_fmt,
    .vidioc_s_fmt_vid_out = privcam_s_fmt,

    .vidioc_querycap = privcam_querycap,
};


static int privcam_open(struct file *file)
{
    struct privcam_dev *dev = video_drvdata(file);
    struct privcam_ctx *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if(!ctx)
        return -ENOMEM;

    v4l2_fh_init(&ctx->fh, &dev->vdev);
    v4l2_fh_add(&ctx->fh);

    file->private_data = &ctx->fh;

    ctx->dev = dev;

    privcam_fill_fmt(&ctx->out_fmt);
    privcam_fill_fmt(&ctx->cap_fmt);

    return 0;
}

static int privcam_release(struct file *file)
{
    struct v4l2_fh *fh = file->private_data;
    struct privcam_ctx *ctx = container_of(fh, struct privcam_ctx, fh);

    v4l2_fh_del(&ctx->fh);
    v4l2_fh_exit(fh);

    kfree(ctx);
    return 0;
}

static const struct v4l2_file_operations privcam_fops = {
    .owner = THIS_MODULE,
    .open = privcam_open,
    .release = privcam_release,
    .unlocked_ioctl = video_ioctl2,
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

    strscpy(privcam->v4l2_dev.name, "privcam", sizeof(privcam->v4l2_dev.name));

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