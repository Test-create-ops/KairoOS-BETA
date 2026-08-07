#!/usr/bin/env bash
# =============================================================
# KairoOS — build della Windows app (.exe + .msix per lo Store)
# Esegui da macOS (toolchain mingw-w64).
#
# Produce in tools/winapp/out/:
#   - KairoOS-win/           cartella pronta (qemu + iso + kairoos.exe)
#   - KairoOS-x86_64.msix    pacchetto per il Microsoft Store
#   - KairoOS-Setup.exe      installer Windows (NSIS) per GitHub
#
# Uso:
#   ./build-winapp.sh                 # download QEMU per Windows + tutto
#   ./build-winapp.sh --local-qemu DIR  # usa binari QEMU già scaricati
#   ./build-winapp.sh --skip-msix     # salta il pacchetto Store
#   ./build-winapp.sh --skip-nsis     # salta l'installer
# =============================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"
PAYLOAD="$OUT/payload"
ISO="$ROOT/viteza.iso"

LOCAL_QEMU=""
SKIP_MSIX=0
SKIP_NSIS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --local-qemu) LOCAL_QEMU="$2"; shift 2 ;;
        --skip-msix) SKIP_MSIX=1; shift ;;
        --skip-nsis) SKIP_NSIS=1; shift ;;
        *) echo "opzione sconosciuta: $1" >&2; exit 1 ;;
    esac
done

MINGW_GCC="$(command -v x86_64-w64-mingw32-gcc || true)"
if [[ -z "$MINGW_GCC" ]]; then
    echo "ERRORE: mingw-w64 non installato.  brew install mingw-w64" >&2
    exit 1
fi

echo "== 1/5 Compilo il launcher (kairoos.exe) =="
mkdir -p "$OUT"
WRES="$(command -v x86_64-w64-mingw32-windres || true)"
if [[ -n "$WRES" ]]; then
    "$WRES" "$HERE/kairoos.rc" -o "$OUT/kairoos_res.o"
    "$MINGW_GCC" -O2 -mwindows -o "$OUT/kairoos.exe" "$HERE/kairoos_launcher.c" \
        "$OUT/kairoos_res.o" -luser32 -lgdi32 -lcomctl32
else
    "$MINGW_GCC" -O2 -mwindows -o "$OUT/kairoos.exe" "$HERE/kairoos_launcher.c" \
        -luser32 -lgdi32 -lcomctl32
fi
ls -lh "$OUT/kairoos.exe"

echo "== 2/5 Asset (icone PNG + ICO) =="
python3 "$HERE/gen_assets.py" >/dev/null
mkdir -p "$OUT/assets"
cp "$HERE/assets/"*.png "$HERE/assets/"*.ico "$OUT/assets/"

echo "== 3/5 QEMU per Windows + ISO =="
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD"
cp "$OUT/kairoos.exe" "$PAYLOAD/"

QEMU_WIN=""
if [[ -n "$LOCAL_QEMU" && -d "$LOCAL_QEMU" ]]; then
    QEMU_WIN="$LOCAL_QEMU"
    echo "   usando QEMU locale: $LOCAL_QEMU"
elif command -v 7z >/dev/null 2>&1; then
    QEMU_SETUP="qemu-w64-setup-20260805.exe"
    QEMU_URL="https://qemu.weilnetz.de/w64/$QEMU_SETUP"
    if [[ ! -f "$OUT/$QEMU_SETUP" ]]; then
        echo "   download QEMU per Windows ($QEMU_SETUP, ~197 MB)..."
        curl -sL -o "$OUT/$QEMU_SETUP" "$QEMU_URL"
    fi
    echo "   estrazione QEMU..."
    rm -rf "$OUT/qemu-win"
    mkdir -p "$OUT/qemu-win"
    7z x -y -o"$OUT/qemu-win" "$OUT/$QEMU_SETUP" >/dev/null 2>&1
    QEMU_WIN="$OUT/qemu-win"
else
    echo "   (installare p7zip per il download automatico: brew install p7zip)" >&2
fi

if [[ -n "$QEMU_WIN" ]]; then
    mkdir -p "$PAYLOAD/qemu"
    cp "$QEMU_WIN/qemu-system-x86_64.exe" "$PAYLOAD/qemu/"
    cp "$QEMU_WIN"/*.dll "$PAYLOAD/qemu/"
    cp "$QEMU_WIN/share/edk2-x86_64-code.fd" "$PAYLOAD/qemu/"
    cp "$QEMU_WIN/COPYING" "$QEMU_WIN/COPYING.LIB" "$QEMU_WIN/VERSION" "$PAYLOAD/qemu/" 2>/dev/null || true
    echo "   pacchetto qemu: $(ls "$PAYLOAD/qemu" | wc -l | tr -d ' ') file"
else
    echo "   ATTENZIONE: QEMU non incluso — scarica da qemu.weilnetz.de/w64/" >&2
fi

if [[ ! -f "$ISO" ]]; then
    echo "ERRORE: $ISO non trovato. Prima esegui: make iso" >&2
    exit 1
fi
cp "$ISO" "$PAYLOAD/viteza.iso"
echo "   viteza.iso ($(du -h "$ISO" | cut -f1)) copiato"

echo "== 4/5 MSIX (Microsoft Store) =="
if [[ $SKIP_MSIX -eq 1 ]]; then
    echo "   (saltato --skip-msix)"
else
    PACK_SAMPLE="${PACK_SAMPLE:-}"
    if [[ -z "$PACK_SAMPLE" ]]; then
        PACK_SAMPLE="$(find "$OUT" "$HOME" -maxdepth 6 -name PackSample -type f 2>/dev/null | head -1 || true)"
    fi
    if [[ -z "$PACK_SAMPLE" ]]; then
        echo "   (PackSample non trovato: salto il pacchetto MSIX)"
        echo "   Compila l'SDK Microsoft msix-packaging ed esporta:"
        echo "   export PACK_SAMPLE=/percorso/PackSample"
    else
        echo "   copio il manifest (modifica REPLACE_ME con la tua identità Partner Center)"
        cp "$HERE/AppxManifest.xml" "$PAYLOAD/AppxManifest.xml"
        cp -r "$HERE/assets" "$PAYLOAD/assets"
        echo "   PackSample -d $PAYLOAD ..."
        (cd "$OUT" && DYLD_LIBRARY_PATH="$(dirname "$PACK_SAMPLE")" "$PACK_SAMPLE" -d "$PAYLOAD" -p "$OUT/KairoOS-x86_64.msix")
        ls -lh "$OUT/KairoOS-x86_64.msix"
    fi
fi

echo "== 5/5 Installer NSIS (GitHub) =="
if [[ $SKIP_NSIS -eq 1 ]]; then
    echo "   (saltato --skip-nsis)"
elif ! command -v makensis >/dev/null 2>&1; then
    echo "   (makensis non installato: brew install makensis)" >&2
else
    makensis -NOCD -DVERSION=1.0.0 \
        -DOUTDIR="$OUT" \
        -DROOTDIR="$ROOT" \
        "$HERE/kairoos.nsi" >/dev/null
    ls -lh "$OUT/KairoOS-Setup.exe"
fi

echo
echo "Fatto! Risultati in $OUT:"
ls -lh "$OUT" | rg -i "msix|setup|exe|payload" || ls -lh "$OUT"
