#!/bin/bash
set -e
DIR=/home/user/mar2gpu/uspace
make -C $DIR -s

echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null
sudo chvt 3
sleep 0.3

sudo $DIR/paint3

sleep 0.3
sudo chvt 2
echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind > /dev/null
