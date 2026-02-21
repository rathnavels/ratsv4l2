#sudo dmesg -c;
make clean; make
sudo rmmod privcam 2>/dev/null
sudo insmod privcam.ko
dmesg
