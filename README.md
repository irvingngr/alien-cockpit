<p align="center">
  <img src="AlienCockpit/assets/cockpit.png" alt="AlienCockpit Logo" width="220" />
</p>

# AlienCockpit — AWCC Alternative

**AlienCockpit** is an ultra-lightweight, low-resource command center for **Windows 11** built as a modern fork of [`alienfx-tools`](https://github.com/T-Troll/alienfx-tools). Unlike heavy vendor suites such as Alienware Command Center (AWCC), AlienCockpit consumes **minimal RAM and near-0% idle CPU** with **no bloated background telemetry or updater services**. It pairs the rock-solid hardware stability of the open-source `AlienFX-SDK` and `AlienFan-SDK` C++ engines with a crisp PerMonitorV2 WebView2 interface.

---

## Quick Installation (No Building Required)

You do **not** need Visual Studio or any compiler to use AlienCockpit:

1. Open the [**GitHub Releases**](https://github.com/irvingngr/alien-cockpit/releases/latest) page.
2. Download the latest precompiled `AlienCockpit-v*.zip` package.
3. Extract the folder anywhere on your PC and run **`AlienCockpit.exe`**.

*(Tip: Enable **Always Run as Administrator** in `Settings → System & Boot` if you want automatic elevated access for ACPI BIOS fan curves.)*

---

## Key Features

- **Ultra-Low Resource Usage**: Native C++20 core with minimal memory footprint and no background bloatware compared to AWCC.
- **Intuitive Visual RGB Studio**
  - Interactive **Full-Size Keyboard** (including Numpad & `⏻ Power Button`), **RGB Touchpad**, **Gaming Mouse**, and **Chassis** visual maps that reflect live hardware state.
  - Smooth 30fps crossfade transitions & speed-calibrated effects: `Static`, `Breathing`, `Pulse`, `Morph`, `Spectrum`, `Wave`, and **`Aurora 2025`** multi-color palettes.
  - Non-destructive **Game & Theme Presets** (`Cyberpunk 2077`, `Aurora Borealis`, `Crimson Halo`, etc.) with 1-click **`Reset to Lighting Studio`**.
  - **1-Click Hardware LED Mapper**: Blink any physical LED to locate it and bind custom zones (`Bind → Touchpad`, `Bind → Power Btn`).
- **Thermals, Fans & Power Modes (`AlienFan-SDK`)**
  - Live CPU/GPU/ambient temperature sensors and fan RPM telemetry.
  - Interactive drag-and-drop **Fan Curve Editor** and quick power profile switching (`Quiet`, `Balanced`, `Performance`, `Full Speed`).
- **Connected Device Battery Telemetry**
  - Displays real-time laptop battery percentage and charging status, plus wireless HID / Bluetooth / Windows Battery Class peripherals.
- **Profiles & Automation**
  - Create, activate, and manage custom lighting/thermal profiles with automatic switching by running applications or power source (AC / Battery).
- **Windows 11 Native Integration**
  - **Start with Windows 11** (`--minimized` support), **Always Run as Administrator** (elevated logon task option), and dynamic **System Tray** icon + battery/lights hover tooltip.
  - Immersive dark titlebar (`DWMWA_USE_IMMERSIVE_DARK_MODE`) and **PerMonitorV2** High-DPI sharpness.

---

## Requirements

- **OS**: Windows 11 (64-bit) — also compatible with Windows 10 v1903+
- **Runtime**: Microsoft Edge WebView2 Runtime (preinstalled on Windows 11)
- **Hardware**: Compatible Alienware / Dell G-Series USB/HID lighting device or ACPI BIOS fan interface

---

## Building from Source (Optional)

> Precompiled binaries are already provided in [**Releases**](https://github.com/irvingngr/alien-cockpit/releases). Only follow these steps if you want to modify or compile the source code yourself.

### Prerequisites
- Visual Studio 2022 or 2026 (Desktop development with C++ workload)
- NuGet CLI (`nuget restore AlienCockpit\packages.config -PackagesDirectory packages`)

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/irvingngr/alien-cockpit.git
   ```
2. Restore NuGet packages:
   ```cmd
   nuget restore AlienCockpit\packages.config -PackagesDirectory packages
   ```
3. Open `AlienCockpit.sln` in Visual Studio and build `Release | x64`, or use MSBuild:
   ```cmd
   msbuild AlienCockpit.sln /t:AlienCockpit /p:Configuration=Release /p:Platform=x64
   ```
4. Automated builds & releases are configured via GitHub Actions (`.github/workflows/build.yml`).

---

## Credits & License

AlienCockpit is forked from the open-source [`alienfx-tools`](https://github.com/T-Troll/alienfx-tools) project by **Rik Lain (T-Troll)**. All low-level HID/USB/ACPI hardware communication is powered by `AlienFX-SDK` and `AlienFan-SDK`.

Licensed under the [MIT License](LICENSE).
