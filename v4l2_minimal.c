// Very minimal V4L2 driver: just registers /dev/videoX and does nothing.

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

#define MINIMAL_CID_OVERLAY_ENABLE (V4L2_CID_USER_BASE + 0x1000)


struct v4l2_minimal_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct mutex lock;

	struct v4l2_ctrl_handler ctrl_handler;
	bool overlay_enable;
};

// Rathinavel has arrived

static struct v4l2_minimal_dev *min_dev;
static struct platform_device *min_pdev;

// struct minimal_fmt {
// 	u32 pixelformat;
// 	const char *desc;
// };

// static const struct minimal_fmt minimal_formats[] = {
// 	{ V4L2_PIX_FMT_GREY, "8-bit Greyscale" },
// 	{ V4L2_PIX_FMT_YUYV, "YUYV 4:2:2" },
// 	{ V4L2_PIX_FMT_RGB24, "RGB24" },
// };

// #define MINIMAL_NUM_FORMATS ARRAY_SIZE(minimal_formats)

static int minimal_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_minimal_dev *dev = video_get_drvdata(vdev);
	struct v4l2_fh *fh;

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	if(!fh)
		return -ENOMEM;

	v4l2_fh_init(fh, vdev);
	v4l2_fh_add(fh);

	file->private_data = fh;

	pr_info("minimal_open\n");
	return 0;
}

static int minimal_release(struct file *file)
{
	struct v4l2_fh *fh = file->private_data;

	pr_info("minimal_release\n");

	v4l2_fh_del(fh);
	v4l2_fh_exit(fh);
	kfree(fh);

	return 0;
}

static int minimal_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_minimal_dev *vdev = 
		container_of(ctrl->handler, struct v4l2_minimal_dev, ctrl_handler);

	switch(ctrl->id)
	{
		case MINIMAL_CID_OVERLAY_ENABLE:
			vdev->overlay_enable = ctrl->val;
			pr_info("v4l2-minimal: overlay_enable: %d\n", vdev->overlay_enable);
			return 0;
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops minimal_ctrl_ops = {
	.s_ctrl = minimal_s_ctrl,
};

/* No real functionality yet */
static const struct v4l2_file_operations minimal_fops = {
	.owner = THIS_MODULE,
	.open = minimal_open,
	.release = minimal_release,
	.unlocked_ioctl = video_ioctl2,
};

static int minimal_vidioc_querycap(struct file *file, void *priv,
				     struct v4l2_capability *cap)
{
 /* set device_caps first */
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

    /* use fixed strings — don't dereference driver state here */
    strscpy(cap->driver, "v4l2_minimal", sizeof(cap->driver));
    strscpy(cap->card,   "v4l2-minimal", sizeof(cap->card));
    strscpy(cap->bus_info, "platform:v4l2-minimal", sizeof(cap->bus_info));

	return 0;
}



// static int minimal_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
// 					    struct v4l2_fmtdesc *f)
// {

// 	pr_alert("enum_fmt: type=%u index=%u\n", f->type, f->index);


// 	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
// 		pr_info("enum_fmt: bad type\n");
// 		return -EINVAL;
// 	}

// 	if (f->index >= MINIMAL_NUM_FORMATS) {
// 		pr_info("enum_fmt: index too big\n");
// 		return -EINVAL;
// 	}


// 	f->pixelformat = minimal_formats[f->index].pixelformat;
// 	strscpy(f->description, minimal_formats[f->index].desc, sizeof(f->description));

// 	return 0;
// }

static const struct v4l2_ioctl_ops minimal_ioctl_ops = {
	.vidioc_querycap = minimal_vidioc_querycap,
//	.vidioc_enum_fmt_vid_cap = minimal_vidioc_enum_fmt_vid_cap
};

static void v4l2_minimal_vdev_release(struct v4l2_minimal_dev *dev)
{
	kfree(dev);
}

static int __init v4l2_minimal_init(void)
{
	int ret;
	struct video_device *vdev;

	pr_info("v4l2_minimal: init\n");

	min_dev = kzalloc(sizeof(*min_dev), GFP_KERNEL);
	if (!min_dev)
		return -ENOMEM;

	mutex_init(&min_dev->lock);

	/* Create a simple parent device so v4l2_device has a struct device */
	min_pdev = platform_device_register_simple("v4l2-minimal", -1, NULL, 0);
	if (IS_ERR(min_pdev)) {
		ret = PTR_ERR(min_pdev);
		pr_err("v4l2_minimal: platform_device_register_simple failed: %d\n",
		       ret);
		goto err_free_dev;
	}

	/* Register v4l2_device (core-level V4L2 handle) */
	strscpy(min_dev->v4l2_dev.name, "v4l2-minimal",
		sizeof(min_dev->v4l2_dev.name));

	ret = v4l2_device_register(&min_pdev->dev, &min_dev->v4l2_dev);
	if (ret) {
		pr_err("v4l2_minimal: v4l2_device_register failed: %d\n", ret);
		goto err_unreg_pdev;
	}

	v4l2_ctrl_handler_init(&min_dev->ctrl_handler, 1);

	v4l2_ctrl_new_custom(&min_dev->ctrl_handler, &(struct v4l2_ctrl_config)
			{
			 .ops = &minimal_ctrl_ops,
			 .id = MINIMAL_CID_OVERLAY_ENABLE,
			 .name = "Overlay Enable",
			 .type = V4L2_CTRL_TYPE_BOOLEAN,
			 .min = 0, .max = 1, .step = 1, .def = 0,
			 
			}, NULL);

	if(min_dev->ctrl_handler.error)
	{
		ret = min_dev->ctrl_handler.error;
		pr_err("ctrl_handler_error: %d\n", ret);
		goto err_unreg_v4l2;
	}

	
	/* Set up video_device */
	vdev = &min_dev->vdev;
	memset(vdev, 0, sizeof(*vdev));
	
	strscpy(vdev->name, "v4l2-minimal", sizeof(vdev->name));
	vdev->v4l2_dev   = &min_dev->v4l2_dev;
	vdev->fops       = &minimal_fops;
	vdev->ioctl_ops  = &minimal_ioctl_ops;
	vdev->release    = video_device_release_empty;
	vdev->lock       = &min_dev->lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->vfl_type   = VFL_TYPE_VIDEO;
	vdev->vfl_dir    = VFL_DIR_RX;
	vdev->dev_parent = &min_pdev->dev;
	
	vdev->ctrl_handler = &min_dev->ctrl_handler;
	min_dev->v4l2_dev.ctrl_handler = &min_dev->ctrl_handler;

	//pr_info("v4l2_minimal: formats=%zu\n", (size_t)MINIMAL_NUM_FORMATS);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, 0);
	if (ret) {
		pr_err("v4l2_minimal: video_register_device failed: %d\n", ret);
		v4l2_device_unregister(&min_dev->v4l2_dev);
		goto err_unreg_v4l2;
	}

	video_set_drvdata(vdev, min_dev);

	pr_info("v4l2_minimal: registered as /dev/video%d\n", vdev->num);
	return 0;

err_unreg_v4l2:
	v4l2_ctrl_handler_free(&min_dev->ctrl_handler);
	v4l2_device_unregister(&min_dev->v4l2_dev);
	goto err_unreg_pdev;

err_unreg_pdev:
	platform_device_unregister(min_pdev);
	min_pdev = NULL;
err_free_dev:
	kfree(min_dev);
	pr_info("v4l2_minimal: init failed\n");
	min_dev = NULL;
	return ret;
}

static void __exit v4l2_minimal_exit(void)
{
	pr_info("v4l2_minimal: exit\n");

	
	if (min_dev) {
		video_unregister_device(&min_dev->vdev);
		v4l2_ctrl_handler_free(&min_dev->ctrl_handler);
		v4l2_device_unregister(&min_dev->v4l2_dev);

		kfree(min_dev);
		min_dev = NULL;
	}

	if (min_pdev) {
		platform_device_unregister(min_pdev);
		min_pdev = NULL;
	}
}

module_init(v4l2_minimal_init);
module_exit(v4l2_minimal_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal V4L2 dummy driver");
MODULE_AUTHOR("Rath");

