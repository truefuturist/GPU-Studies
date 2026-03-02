#!/bin/bash
# Build paint3.efi, copy to ESP, then boot into UEFI Shell next boot.
# From the shell: fs0:\EFI\paint\paint3.efi
# After app returns: type 'exit' or 'reset' to come back to Linux.
set -e
DIR=/home/user/mar2gpu/efi

make -C $DIR -s

sudo cp $DIR/paint3.efi   /boot/efi/EFI/paint/paint3.efi
sudo cp $DIR/startup.nsh  /boot/efi/startup.nsh
echo "Copied paint3.efi and startup.nsh to ESP"

# Boot into UEFI Shell on next boot only (0001), then resume normal order
sudo efibootmgr --bootnext 0001
echo "BootNext set to UEFI Shell (0001)"
echo ""
echo "At the shell prompt, run:"
echo "  fs0:\\EFI\\paint\\paint3.efi"
echo ""
echo "Rebooting in 3 seconds... Ctrl-C to cancel"
sleep 3
sudo reboot
