#!/bin/bash
# No drm_setup needed — module does everything itself.
set -e
KMOD=/home/user/mar2gpu/kmod2
make -C $KMOD -s

echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null

# Switch VT so GNOME drops DRM master and stops writing PLANE_SURF
sudo chvt 3
sleep 0.3

# Module: allocs buffer, writes GGTT, flips PLANE_SURF, animates, restores
sudo insmod $KMOD/paint2.ko

# By the time insmod returns, PLANE_SURF is already restored to GNOME's value
sleep 0.3

sudo chvt 2
echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null
sudo rmmod paint2 2>/dev/null || true

echo "--- kernel log ---"
sudo dmesg | grep "p2:" | tail -8
