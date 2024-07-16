#!/usr/bin/env bash
set -e

# PMBOOTSTRAP_OPTIONS="-o"
PMBOOTSTRAP_OPTIONS=""

make O=.output jf_postmarketos_defconfig 
cd .output 
make -j10 
cd .. 
~/proj/pmbootstrap/pmbootstrap.py $PMBOOTSTRAP_OPTIONS build --envkernel linux-postmarketos-qcom-apq8064 --force 
~/proj/pmbootstrap/pmbootstrap.py $PMBOOTSTRAP_OPTIONS checksum device-samsung-jflte 
~/proj/pmbootstrap/pmbootstrap.py $PMBOOTSTRAP_OPTIONS --no-cross build device-samsung-jflte --force 
~/proj/pmbootstrap/pmbootstrap.py $PMBOOTSTRAP_OPTIONS export

echo "flashing..."
OUT=$(sudo nix run nixpkgs#heimdall -- flash --BOOT ~/proj/postmarketos/chroot_rootfs_samsung-jflte/boot/boot.img)
echo $OUT | grep "BOOT upload failed!"
