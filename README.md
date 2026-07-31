# KairoOS

![OS: x86-64](https://img.shields.io/badge/arch-x86__64-blue)
![Boot: Multiboot2](https://img.shields.io/badge/boot-Multiboot2%2BPVH-green)
![Lang: C](https://img.shields.io/badge/lang-C%2Basm-orange)
![Status: Experimental](https://img.shields.io/badge/status-experimental-yellow)
![GitHub last commit](https://img.shields.io/github/last-commit/Test-create-ops/KairoOS-BETA?color=purple)
![GitHub repo size](https://img.shields.io/github/repo-size/Test-create-ops/KairoOS-BETA)

## Preview

![KairoOS desktop concept](docs/kairo-preview.png)

*Concept dell'interfaccia (mockup), non uno screenshot reale del sistema in esecuzione.*

## Active Development

The main development branch is **`main`**.

This repository hosts the **KairoOS Computer** kernel (x86-64 PC), not the **KairoTouch** mobile variant (ARM64).

## Build

```bash
make
bash iso/mkiso.sh
```

## Run in QEMU

```bash
# Direct boot (PVH)
./boot.sh

# Boot from ISO (UEFI)
./boot.sh iso

# Boot from ISO (Legacy BIOS)
./boot.sh iso-bios
```

## What is KairoOS?

KairoOS is a **single-developer** hobby OS made with passion. Boot it in QEMU or a VM and explore.

### Features

- Kernel **x86-64** scritto in **C + ASM**
- Boot **Multiboot2 / PVH** (UEFI e Legacy BIOS)
- Avvio da **ISO** o boot diretto su **QEMU** / VM
- Struttura modulare: `kernel/`, `boot/`, `iso/`, `dist/`

## Structure

- `kernel/` — kernel sources (C, ASM)
- `boot/` — boot stubs (GDT, IDT, loader)
- `iso/` — ISO build scripts
- `dist/` — distribution files
