#!/bin/sh
set -e

mkdir -p iso_root/boot/grub
cp kernel.bin iso_root/boot/kernel.bin
cp iso/grub.cfg iso_root/boot/grub/grub.cfg

x86_64-elf-grub-mkrescue -o viteza.iso iso_root
