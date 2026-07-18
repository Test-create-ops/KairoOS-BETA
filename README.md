# KairoOS

![OS: x86-64](https://img.shields.io/badge/arch-x86__64-blue)
![Boot: Multiboot2](https://img.shields.io/badge/boot-Multiboot2%2BPVH-green)
![Lang: C](https://img.shields.io/badge/lang-C%2Basm-orange)
![Status: Experimental](https://img.shields.io/badge/status-experimental-yellow)
![GitHub last commit](https://img.shields.io/github/last-commit/Test-create-ops/KairoOS-BETA?color=purple)
![GitHub repo size](https://img.shields.io/github/repo-size/Test-create-ops/KairoOS-BETA)

## Sviluppo attivo

Il branch principale di sviluppo è **`master`**.

Questo repository ospita il codice del kernel **KairoOS Computer** (x86-64 PC), non la variante mobile **KairoTouch** (ARM64).

## Build

```bash
make
bash iso/mkiso.sh
```

## Avvio in QEMU

```bash
# Avvio diretto (PVH)
./boot.sh

# Avvio da ISO (UEFI)
./boot.sh iso

# Avvio da ISO (Legacy BIOS)
./boot.sh iso-bios
```

## Struttura

- `kernel/` — sorgenti del kernel (C, ASM)
- `boot/` — stubs di boot (GDT, IDT, loader)
- `iso/` — script per generare ISO
- `dist/` — distribuzione
