#!/bin/sh
# Run Viteza OS with AI (serial proxy + Ollama)
pkill -9 -f social_proxy.py 2>/dev/null
pkill -9 -f qemu-system 2>/dev/null
sleep 1

# Start proxy in background
python3 social_proxy.py &
PROXY_PID=$!
echo "Proxy PID: $PROXY_PID"

# Start QEMU with serial
qemu-system-x86_64 -kernel kernel.bin -m 256 -vga std \
    -machine pc,accel=tcg -cpu qemu64 \
    -serial tcp:localhost:9000,server,nowait \
    -display cocoa &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

trap 'kill $QEMU_PID $PROXY_PID 2>/dev/null; exit' INT TERM
wait
