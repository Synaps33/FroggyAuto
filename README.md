# FroggyAuto

**Grand Theft Auto III (re3)** ported to the **Data Frog SF2000** and **GB300** handheld game consoles (MIPS32r2, no FPU, bare-metal multicore libretro).

---

## Overview

FroggyAuto brings GTA III (re3) to the low-cost Data Frog SF2000 and GB300 handhelds. Because the hardware lacks a 3D graphics accelerator (GPU) and hardware floating-point unit (FPU), the port uses a custom software rasterizer backend in librw and soft-float optimizations to render Liberty City entirely on the CPU.

---

## Features

- **Custom librw Software Rasterizer:** CPU-driven 3D pipeline with Z-buffering, texture mapping, affine UV interpolation, and alpha blending.
- **Optimized for MIPS Soft-Float:** Tuned mathematical paths and fixed-point fast paths to minimize floating-point emulation penalty on MIPS32r2.
- **Dynamic Resolution Scaling:** Runs the 3D scene at 160x120 upscaled to the console's 320x240 LCD display for maximized performance while rendering 2D HUD and text crisp at native resolution.
- **Direct Handheld Controls:** Full mapping for SF2000 and GB300 buttons (D-Pad, face buttons, shoulder bumpers, START/SELECT).
- **Dual Firmware Support:**
  - Stock FrogOS / FrogUI via `cores/re3/core_87000000` and launcher stub `ROMS/re3;re3;re3.gba`.
  - Multicore / Deimos frontend via `re3.sf2k`.
- **Memory Streaming Optimizations:** Streamlined resource loading and texture halving tailored for the console's limited RAM.

---

## Controls

### On Foot

| Button | Action |
|---|---|
| **D-Pad Up / Down** | Walk Forward / Backward |
| **D-Pad Left / Right** | Turn Left / Right |
| **A** | Sprint |
| **B** | Attack / Fire Weapon |
| **X** | Enter Vehicle / Action |
| **Y** | Jump |
| **L / R** | Cycle Previous / Next Weapon |
| **SELECT** | Change Camera Mode |
| **START** | Pause / Options Menu |

### In Vehicle

| Button | Action |
|---|---|
| **D-Pad Up** | Accelerate |
| **D-Pad Down** | Brake / Reverse |
| **D-Pad Left / Right** | Steer Left / Right |
| **A** | Handbrake |
| **B** | Drive-by Attack / Fire |
| **X** | Exit Vehicle |
| **Y** | Horn |
| **L / R** | Look Left / Look Right |
| **SELECT** | Change Camera Mode |
| **START** | Pause / Options Menu |

---

## Installation

### Requirements

- A micro SD card prepared for SF2000 or GB300 (FrogUI or Multicore).
- Retail PC assets from **Grand Theft Auto III** (v1.0 or v1.1):
  - Folders: `audio/`, `data/`, `models/`, `text/`, `skins/`
  - Files: `txd.img`, `txd.dir`, `gta3.img`, `gta3.dir`, etc.

### Quick Setup

1. Download the release files (`FroggyAuto-v1.0.0.zip` or individual core binaries).
2. Copy the core binary to your SD card depending on your frontend:
   - **FrogUI / Stock firmware:**
     - Copy `core_87000000` to `cores/re3/core_87000000`
     - Create/copy the stub file `ROMS/re3;re3;re3.gba`
   - **Deimos / Multicore:**
     - Copy `re3.sf2k` to `system/Deimos/cores/re3.sf2k` (and `cores/re3.sf2k`)
3. Create a folder `ROMS/re3/` on your SD card.
4. Copy your GTA III PC game data files (`audio`, `data`, `models`, `text`, `skins`) into `ROMS/re3/`.
5. Safely eject the SD card, insert into your console, navigate to the game list, and launch **re3**.

---

## Building from Source

### Prerequisites

- `mipsel-mti-elf` toolchain for MIPS32r2 target.
- Data Frog `sf2000_multicore` repository.
- Host build tools (`gcc`, `g++`, `make`).

### Build Targets

```bash
# Build for Data Frog SF2000
make sf2000-core

# Build for Data Frog GB300
make gb300-core

# Build PC simulator for testing
make gbsim
```

---

## Credits & Acknowledgments

- **re3 team** for the original reverse-engineered GTA III engine.
- **shinovon** for the Symbian re3 baseline and software rasterizer foundations.
- **librw team** for the RenderWare graphics engine reimplementation.
- **Data Frog / SF2000 / GB300 homebrew community** for toolchains, multicore, and reverse-engineering the platform.
