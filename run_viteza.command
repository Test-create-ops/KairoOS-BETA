#!/bin/sh
# Viteza OS Launcher — avvia QEMU + proxy seriale per AI
# Trascina in Terminale o fai doppio-click (.command)

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

pkill -9 -f "social_proxy.py" 2>/dev/null
pkill -9 -f "qemu-system.*kernel.bin" 2>/dev/null
sleep 1

# Avvia proxy in background
python3 social_proxy.py &
PROXY_PID=$!

# Avvia QEMU in foreground (così quando chiudi QEMU, si ferma tutto)
qemu-system-x86_64 -kernel kernel.bin -m 256 -vga std \
    -machine pc,accel=tcg -cpu qemu64 \
    -serial tcp:localhost:9000,server,nowait \
    -display cocoa

# QEMU chiuso → kill proxy
kill $PROXY_PID 2>/dev/null
