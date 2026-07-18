#!/bin/sh
# Boot Viteza in QEMU
# Usage:
#   ./boot.sh          — quick boot via -kernel (fast, PS/2 + VBE works)
#   ./boot.sh iso      — ISO boot via UEFI (more realistic, requires OVMF)
#   ./boot.sh iso-bios — ISO boot via legacy BIOS (may fail on ARM TCG)

set -e

# Save and lower system volume (PC speaker at 100% = deafening)
VOLUME_SAVED=$(osascript -e 'output volume of (get volume settings)' 2>/dev/null || echo "50")
osascript -e "set volume output volume 6" 2>/dev/null || true
trap 'osascript -e "set volume output volume $VOLUME_SAVED" 2>/dev/null || true' EXIT INT TERM

MACH="pc,accel=tcg"
CPU="qemu64"
MEM="256"
VGA="std"
KERNEL="kernel.bin"
ISO="viteza.iso"
AUDIO="-audiodev coreaudio,id=audio0 -machine pcspk-audiodev=audio0 -device AC97,audiodev=audio0"

# Find OVMF UEFI firmware
OVMF=""
for d in /opt/homebrew/share/qemu /usr/local/share/qemu /usr/share/qemu; do
    for f in edk2-x86_64-code.fd OVMF.fd OVMF-pure-efi.fd; do
        if [ -f "$d/$f" ]; then
            OVMF="$d/$f"
            break 2
        fi
    done
done

case "${1:-kernel}" in
    kernel)
        echo "Booting via -kernel (direct PVH entry)..."
        qemu-system-x86_64 -kernel "$KERNEL" -m "$MEM" -vga "$VGA" \
            -machine "$MACH" -cpu "$CPU" $AUDIO -rtc base=localtime \
            -netdev user,id=net0,hostfwd=tcp::2525-:25,hostfwd=tcp::9999-:9999 \
            -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
            -serial tcp:localhost:9000,server,nowait \
            -display cocoa
        ;;
    iso)
        if [ -z "$OVMF" ]; then
            echo "OVMF firmware not found. Install edk2 via Homebrew:"
            echo "  brew install edk2"
            echo "Or use: ./boot.sh iso-bios"
            exit 1
        fi
        echo "Booting ISO via UEFI (OVMF)..."
        qemu-system-x86_64 \
            -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
            -cdrom "$ISO" -m "$MEM" -vga "$VGA" \
            -machine q35,accel=tcg -cpu "$CPU" -display cocoa
        ;;
    iso-bios)
        echo "Booting ISO via legacy BIOS (SeaBIOS)..."
        echo "NOTE: May fail on ARM TCG. Use './boot.sh iso' for UEFI."
        qemu-system-x86_64 -cdrom "$ISO" -m "$MEM" -vga "$VGA" \
            -machine "$MACH" -cpu "$CPU" -display cocoa
        ;;
    *)
        echo "Usage: $0 [kernel|iso|iso-bios]"
        exit 1
        ;;
esac