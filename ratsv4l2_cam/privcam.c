
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

#define PRIVCAM_DEF_WIDTH 640
#define PRIVCAM_DEF_HEIGHT 480
#define PRIVCAM_DEF_PIXFMT V4L2_PIX_FMT_YUYV

struct privcam_ctx {
    struct v4l2_fh fh;
    struct privcam_dev *dev;
    struct v4l2_m2m_ctx *m2m_ctx;

    struct v4l2_pix_format out_fmt;
    struct v4l2_pix_format cap_fmt;

    u32 sequence;
};

static void privcam_fill_fmt(struct v4l2_pix_format *f)
{
    f->width = PRIVCAM_DEF_WIDTH;
    f->height = PRIVCAM_DEF_HEIGHT;
    f->pixelformat = PRIVCAM_DEF_PIXFMT;

    f->field = V4L2_FIELD_NONE;
    f->bytesperline = f->width * 2;
    f->sizeimage = f->bytesperline * f->height;
    f->colorspace = V4L2_COLORSPACE_SRGB;
}

static int privcam_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
    if(f->index > 0)
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

static const struct v4l2_ioctl_ops privcam_ioctl_ops = {
    .vidioc_enum_fmt_vid_cap = privcam_enum_fmt,
    .vidioc_enum_fmt_vid_out = privcam_enum_fmt,

    .vidioc_g_fmt_vid_cap = privcam_g_fmt,
    .vidioc_g_fmt_vid_out = privcam_g_fmt,

    .vidioc_try_fmt_vid_cap = privcam_try_fmt,
    .vidioc_try_fmt_vid_out = privcam_try_fmt,

    .vidioc_s_fmt_vid_cap = privcam_s_fmt,
    .vidioc_s_fmt_vid_out = privcam_s_fmt,


};