#!/bin/bash
# Viteza OS — Launcher per Linux
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "=========================================="
echo "  Viteza OS — Avvio in corso..."
echo "=========================================="
echo ""

if ! command -v qemu-system-x86_64 &>/dev/null; then
    echo "[1/4] QEMU non trovato. Installo..."
    if command -v apt &>/dev/null; then
        sudo apt install -y qemu-system-x86
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y qemu-system-x86
    elif command -v pacman &>/dev/null; then
        sudo pacman -S --noconfirm qemu-system-x86
    else
        echo "Installa QEMU manualmente: https://qemu.org/download"
        exit 1
    fi
else
    echo "[1/4] QEMU trovato!"
fi

if ! command -v ollama &>/dev/null; then
    echo "[2/4] Ollama non trovato. Installo..."
    curl -fsSL https://ollama.com/install.sh | sh
else
    echo "[2/4] Ollama trovato!"
fi

echo "[3/4] Scarico modello AI (solo la prima volta)..."
ollama pull llama3.2 2>&1 | tail -3

echo "[4/4] Avvio Viteza OS + AI..."
echo ""

pgrep ollama >/dev/null || ollama serve &
sleep 2

python3 "$DIR/social_proxy.py" &
PROXY_PID=$!
sleep 2

qemu-system-x86_64 -cdrom "$DIR/viteza.iso" -m 256 -vga std \
    -machine pc,accel=tcg -cpu qemu64 \
    -serial tcp:localhost:9000,server,nowait

kill $PROXY_PID 2>/dev/null
echo ""
echo "Viteza OS terminato. Ciao!"
