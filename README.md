# ios-fuxi

**Apple's iOS 16 userland on a Xiaomi 13 (`fuxi`, Snapdragon 8 Gen 2 / SM8550).**

This project runs Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) on the Xiaomi 13, with the **unmodified iOS 16.7 (20H392) userland of the iPhone X** on top: launchd, the system daemons, backboardd, SpringBoard and the stock apps. It is a fork of [ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the same on the Redmi Note 10 Pro. The Sweet documentation is kept in [docs/sweet.md](docs/sweet.md).

The phone boots to the iOS home screen. You can use the touchscreen and the hardware buttons and open apps such as Settings.

> Experimental bring-up, not a usable phone OS. Most hardware is missing, rendering is done by the CPU, and Apple's security model is largely switched off. Do not put anything personal on it.

## Status

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi bootloader → temporary `fastboot boot` of our loader → XNU. Nothing is flashed to the boot partitions |
| Loader | Bare-metal arm64 loader (`shim/fuxi-loader`) with a USB control interface on a second core for logs, RAM reads and diagnostics while XNU runs |
| CPU | 7 of the 8 cores. The loader caps the cluster clocks through EPSS, which keeps the SoC around 51 °C in sustained use (peaks near 63 °C without the caps) |
| Storage | Own UFS host driver. The iOS root is a journaled HFS+ image stored as a pinned file inside Android's `userdata`; XNU reads it through an extent table (patch 0088) |
| Display | 1080×2400 DSC panel. CoreAnimation renders on the CPU, frames are copied to the bootloader framebuffer at 60 Hz |
| Touch | FTS touch controller over SPI, delivered as HID digitizer events with adaptive polling |
| Buttons | Power (PMK8550 PON) and volume down/up (PMK8550 PON, PM8550 GPIO) over the SPMI arbiter, as HID keys. Power turns the display off and on |
| GPU | Adreno A740 bring-up in the loader: the GMU firmware boots, the HFI handshake completes and the GPU core is powered. Command-processor stages are written but not yet tested on the device. No GPU driver in XNU yet |
| Not working | Wi-Fi, Bluetooth, cellular, audio, camera, sensors, Face ID / SEP, GPU acceleration in iOS |

### Milestone 7 memory layout

The offline 0097 images select 5088 MiB of XNU RAM at `0x8c0000000`, with
32 MiB at `0x9fe000000` reserved for GPU scratch, firmware and handoff. This
uses the observed contiguous 5 GiB bank; upper-bank runtime validation is
pending. `mkadt-fuxi.py --ram-layout 1g` preserves the old 1 GiB XNU window.
See the [memory audit and address map](docs-lanes/memory.md).

## How it boots

```
fastboot boot fuxi-xnu-boot.img
  -> fuxi-loader (Linux Image header, MMU off)
       - reads the live device tree, sets up RAM, display and CPU clocks
       - starts a USB monitor on a second core
       - jumps to XNU with an Apple device tree (ADT)
  -> XNU -> UFS driver -> HFS+ root (file extents inside userdata)
  -> launchd -> backboardd -> SpringBoard
```

The host side, `tools/fuxi-xnu-monitor.py`, talks to the loader over USB. It streams the kernel console, samples temperatures and clocks, applies CPU clock limits and resets the phone back to Android when a session ends.

## Repository layout

| Path | What |
| --- | --- |
| `xnu/patches/` | Kernel patches. 0001–0081 come from ios-sweet, 0082 onwards are Fuxi (RAM, UFS, extent root, SMP, touch, buttons, display) |
| `tools/build-fuxi-native.sh` | Native kernel build on an Apple-silicon Mac |
| `shim/fuxi-loader/` | The Fuxi loader and its boot image build (`build.sh`) |
| `shim/fuxi-probe/` | Hardware probes linked into the loader: display, touch, thermal/EPSS, GPU (`gpu.c`) |
| `tools/fuxi-*.py`, `tools/mkadt-fuxi.py` | USB monitor, GPU probe decoder, ADT builder, root staging, DTB inspection |
| `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py` | Build the iOS root image from the IPSW and apply the bring-up changes |
| `docs-lanes/` | Per-subsystem notes: GPU, thermal, buttons, touch/display |
| `docs/sweet.md` | The original ios-sweet README |

Not in git: partition backups, device trees, recovery images, Apple firmware and root images, GPU firmware and build outputs. They contain device serials and calibration data, or files that cannot be redistributed. The GPU firmware (`gmu_gen70200.bin`, `a740_sqe.fw`) comes from linux-firmware and is passed to the loader build through environment variables.

## Building

1. **Kernel:** build xnu-8796 on macOS with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (`MACOS_VERSION=13.5`) with `xnu/patches/` applied, or use `tools/build-fuxi-native.sh` (see [xnu/README.md](xnu/README.md)).
2. **iOS root:** `tools/mkiosroot.sh` and `tools/fuxi-stage-root.py` build the HFS+ root from the iOS 16.7 IPSW, which you supply.
3. **Payload and loader:** `tools/mkadt-fuxi.py` and `tools/mkpayload.py` pack the kernel and ADT, then `shim/fuxi-loader/build.sh <payload>` builds the boot image with an LLVM toolchain (`CLANG`, `LLD`, `OBJCOPY` and `MKBOOTIMG` environment variables).
4. **Run:** `fastboot boot` the image, then attach `tools/fuxi-xnu-monitor.py`.

You need an unlocked bootloader, a rooted recovery for installing the root file, and backups of your critical partitions (`tools/fuxi-backup-critical.py`). Everything here is temporary-boot only, but you can still lose data.

## Credits

- **[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel)** by OOSEX GANG: the APSL-2.0 iOS Wi-Fi interface, Apple80211 ioctl/station bridge, property-only WLAN discovery, network-stack stand-in and WPA2 arithmetic adapted in `xnu/fuxi-wifi-ios/` (patch 0104). Its [MIT userland notes](https://github.com/oosexgang/hackios-xnu-userland) informed the independent opt-in wifid root-edit tool; no userland code was copied. See [P-WIFI provenance, ABI limits and test plan](docs-lanes/wifi-ios.md).
- **[HackiOS](https://github.com/oosexgang/hackios-xnu-userland)**: MIT ir3 default-rounding fix from
  `hackios-xnu-userland/tools/mesa/ir3-round-default.patch`, adapted for our pinned
  Mesa/Turnip compiler. See the [P-MESA audit](docs-lanes/mesa-port.md) for source,
  retained licence and the other patches reviewed.

- **[metal2vulkan](https://github.com/steelbrain/metal2vulkan)** by Anees Iqbal and contributors: external AIR-to-SPIR-V translator, pinned to `720ef1ce9ae2a2068999a8675717418b9416283e`. Its declared licence is GPL-2.0-or-later OR LGPL-3.0-or-later; the Metal lane selects **LGPL-3.0-or-later** for a separate, replaceable `libmetal2vulkan.dylib` loaded with `dlopen`. No translator source is vendored or statically linked into FuxiMetal/XNU. The ignored build output includes corresponding library/adapter/dependency source, lockfile, LGPL/GPL texts, notices and an offline rebuild recipe. Preserve those materials and the rights/information needed to replace and debug modifications to the library when distributing it; dynamic loading alone does not discharge the obligations. Our original code retains its existing repository terms. See [Metal build, licensing and installation details](docs-lanes/metal.md#m5b-external-translator-and-offscreen-pipeline).

- **[UniXNU](https://github.com/UniXNU/UniXNU) / NativeAdreno, NativeMetal, NativeMobileDisplay and NativeUfs contributors**: MIT NativeAdreno wire ABI and A7xx BR/BV submission framing; retained notice in `xnu/fuxi-adreno/LICENSE.NativeAdreno`. NativeMetal's CA encoder, queue and neutral depth/stencil adapters are adapted in FuxiMetal, with notice in `tools/fuxi-metal/LICENSE.NativeMetal` and the signed/staged bundle. Their cache design informed our process-only Vulkan pipeline cache. See [M9b comparison and provenance](docs-lanes/metal-m9b.md). P-UNIXNU also adapts NativeMobileDisplay’s bounded/differential scanout copy and timer policy, and NativeUfs’s completion, response validation and DMA quarantine policy (source SPDX: BSD-2-Clause-Patent). Notices, including NativeUfs’s BSD-2-Clause `LICENSE`, are retained in `xnu/fuxi-unixnu/LICENSE.*`. See the [driver audit, provenance and default-off options](docs-lanes/unixnu-port.md).

- **[ios-sweet](https://github.com/dipa-studio/ios-sweet) by [dipa-studio](https://github.com/dipa-studio)**: the original project this repository is forked from. The XNU port, the iOS userland bring-up, the kernel patches up to 0081 and most of the tooling come from it.
- **Claude Opus 5.5 ([Anthropic](https://www.anthropic.com))**: AI pair engineer on the Xiaomi 13 bring-up: loader, XNU patches, drivers, thermal work and device testing.
- **[OpenAI Codex](https://openai.com/codex)**: the GPU lane: GMU memory setup, firmware loading, HFI startup, GX power-up and the command-processor probe stages.
- Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), the Linux kernel (msm DRM, Qualcomm clock, interconnect and SMMU drivers) and linux-firmware for hardware details, Qualcomm/Xiaomi's published kernel sources, and [OrangeFox Recovery](https://orangefox.download).

iOS itself is Apple's and is not distributed here.
