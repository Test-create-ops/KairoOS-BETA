# KairoOS

![OS: x86-64](https://img.shields.io/badge/arch-x86__64-blue)
![Boot: Multiboot2](https://img.shields.io/badge/boot-Multiboot2%2BPVH-green)
![Lang: C](https://img.shields.io/badge/lang-C%2Basm-orange)
![Status: Experimental](https://img.shields.io/badge/status-experimental-yellow)
![GitHub last commit](https://img.shields.io/github/last-commit/Test-create-ops/KairoOS-BETA?color=purple)
![GitHub repo size](https://img.shields.io/github/repo-size/Test-create-ops/KairoOS-BETA)

## Preview

![KairoOS desktop preview](docs/kairo-preview.png)

Un assaggio dell'interfaccia di KairoOS: finestre, icone, widget, suoni e
musica — tutto generato dal codice, senza asset esterni.

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

- Kernel x86-64 scritto in **C + ASM**, boot **Multiboot2/PVH**
- **GUI**: finestre, icone vettoriali, 68 icone built-in
- **UI pack**: 28 widget (pulsanti, slider, toggle, rating, tabs, grafici...)
- **Audio sintetizzato**: 66 suoni e 10 musiche generati da codice Python
- **API facile**: namespace C++ `kec::` (Kairo Easy Coding)

## Structure

- `kernel/` — kernel sources (C, ASM)
- `boot/` — boot stubs (GDT, IDT, loader)
- `iso/` — ISO build scripts
- `dist/` — distribution files
