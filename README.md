<p align="center">
  <img src="AlienCockpit/assets/cockpit.png" alt="AlienCockpit Logo" width="220" />
</p>

# AlienCockpit — AWCC Alternative

**AlienCockpit** is a fast, lightweight, and stable command center for **Windows 11** built as a modern fork of [`alienfx-tools`](https://github.com/T-Troll/alienfx-tools). It pairs the rock-solid hardware stability of the open-source `AlienFX-SDK` and `AlienFan-SDK` C++ engines with an intuitive, space-gaming cockpit interface powered by Microsoft Edge WebView2.

---

## Key Features

- **Intuitive Visual RGB Studio**
  - Interactive **Keyboard** (click or drag-select keys, quick-select groups like `WASD`, `Arrows`, `F-Keys`, `Modifiers`), **Gaming Mouse**, and **Chassis** visual maps.
  - Real-time color & effect assignment: `Static`, `Breathing`, `Pulse`, `Morph`, `Spectrum`, and `Wave`.
  - **1-Click Hardware LED Mapper**: Blink any physical LED on your device to identify and bind it effortlessly.
- **Thermals, Fans & Power Modes (`AlienFan-SDK`)**
  - Live CPU/GPU/ambient temperature sensors and fan RPM telemetry.
  - Interactive drag-and-drop **Fan Curve Editor** and quick power profile switching (`Quiet`, `Balanced`, `Performance`, `Full Speed`).
- **Connected Device Battery Telemetry**
  - Displays real-time battery percentage and charging status for your laptop battery as well as connected wireless peripherals (Bluetooth / 2.4GHz HID mice, keyboards, and headsets) via Windows 11 PnP battery reporting.
- **Profiles & Automation**
  - Create, activate, and manage custom lighting/thermal profiles with automatic switching by running applications or power source (AC / Battery).
- **Windows 11 Native Integration**
  - **Start with Windows 11** (`--minimized` support) and **System Tray** quick-switch menu.
  - Immersive dark titlebar (`DWMWA_USE_IMMERSIVE_DARK_MODE`) and PerMonitorV2 High-DPI support.

---

## Requirements

- **OS**: Windows 11 (64-bit) — also compatible with Windows 10 v1903+
- **Runtime**: Microsoft Edge WebView2 Runtime (preinstalled on Windows 11)
- **Hardware**: Compatible Alienware / Dell G-Series USB/HID lighting device or ACPI BIOS fan interface

---

## Building from Source

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
4. Automated builds are also configured via GitHub Actions (`.github/workflows/build.yml`).

---

## Credits & License

AlienCockpit is forked from the open-source [`alienfx-tools`](https://github.com/T-Troll/alienfx-tools) project by **Rik Lain (T-Troll)**. All low-level HID/USB/ACPI hardware communication is powered by `AlienFX-SDK` and `AlienFan-SDK`.

Licensed under the [MIT License](LICENSE).
