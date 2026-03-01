#!/bin/bash
set -e
KMOD=/home/user/mar2gpu/kmod

# Build
make -C $KMOD -s
gcc -O2 -o $KMOD/drm_setup $KMOD/drm_setup.c -Wall

echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null

# Start drm_setup in a new VT: it sets the CRTC and writes GTT offset to /tmp/gtt_off
rm -f /tmp/gtt_off
sudo openvt -s -- bash -c "$KMOD/drm_setup > /tmp/drm_setup.log 2>&1" &
OPENVT_PID=$!

# Wait for setup to write the GTT offset
for i in $(seq 20); do
    [ -f /tmp/gtt_off ] && break
    sleep 0.2
done

GTT=$(cat /tmp/gtt_off)
echo "GTT offset: $GTT"
cat /tmp/drm_setup.log

# Load module — animation runs in ring 0 via pci_iomap(BAR2)
sudo insmod $KMOD/paint.ko gtt_off=$GTT

# Module runs for 3 seconds then exits.
# Switch back to GNOME FIRST so it reclaims the display before we destroy
# the drm_setup buffer — avoids the display engine scanning freed memory.
sudo chvt 2
echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null
sudo kill $OPENVT_PID 2>/dev/null || true
sudo rmmod paint 2>/dev/null || true

echo "--- kernel log ---"
sudo dmesg | grep paint | tail -6
