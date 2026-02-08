sudo dmesg -c;
make clean; make
sudo rmmod v4l2_minimal
sudo insmod v4l2_minimal.ko
dmesg
