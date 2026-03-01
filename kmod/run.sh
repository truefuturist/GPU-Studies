#!/bin/bash
set -e
KMOD=/home/user/mar2gpu/kmod

make -C $KMOD -s
gcc -O2 -o $KMOD/drm_setup $KMOD/drm_setup.c -Wall 2>/dev/null

echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null
rm -f /tmp/gtt_off /tmp/drm_setup.pid

# Start drm_setup on a new VT — sets CRTC, writes GTT offset + its PID, then waits
sudo openvt -s -- bash -c "$KMOD/drm_setup >> /tmp/drm_setup.log 2>&1" &

# Wait for drm_setup to be ready
for i in $(seq 30); do
    grep -q "ready" /tmp/drm_setup.log 2>/dev/null && break
    sleep 0.2
done

GTT=$(cat /tmp/gtt_off)
DSP=$(cat /tmp/drm_setup.pid)
echo "GTT $GTT  drm_setup PID $DSP"

# Module animates for 3 seconds in ring 0
sudo insmod $KMOD/paint.ko gtt_off=$GTT

# Module is done. Kill drm_setup FIRST — its cleanup disables the CRTC
# and frees the buffer cleanly through i915 before GNOME touches anything.
sudo kill $DSP
sleep 1   # wait for SetCRTC(off) + RMFB + destroy_dumb to complete

# Now switch back — GNOME gets a clean display engine, no orphaned CRTCs
sudo chvt 2
echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null

echo "--- kernel log ---"
sudo dmesg | grep paint | tail -4
