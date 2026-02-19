# ratsv4l2

This is a simple v4l2 device driver that can take input YUYV buffers and output the same YUYV buffers. Just a pass through driver.
This has an accompanying app that captures a YUYV image using a camera, captures the capture buffer from the camera driver, passes it on the custom driver
as input buffer.  The app eventually gets the capture buffer back from the custom driver and saves it to a file.

