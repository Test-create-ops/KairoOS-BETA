#!/bin/sh
# Build distribution package for Viteza
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=== Build distribution package ==="

# 1. Build kernel
echo "[1/4] Building kernel..."
make -f build/Makefile.build

# 2. Build ISO
echo "[2/4] Building ISO..."
bash iso/mkiso.sh

# 3. Prepare dist folder
echo "[3/4] Preparing dist..."
mkdir -p dist
cp viteza.iso dist/
cp social_proxy.py dist/

# Make macOS .app executable
chmod +x dist/Viteza.app/Contents/MacOS/launcher

# Make scripts executable
chmod +x dist/run_macos.command dist/run_linux.sh

# 4. Create zip
echo "[4/4] Creating zip..."
VERSION=$(date +%Y%m%d)
cd dist
zip -r "../Viteza-v${VERSION}.zip" ./*
cd ..
echo "Done: Viteza-v${VERSION}.zip"
ls -lh "Viteza-v${VERSION}.zip"
