# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CSPBridgeGimp is a C++ project that bridges Clip Studio Paint (CSP) plugins to run GIMP plugins. The CSP plugin acts as a GIMP host emulator, launching GIMP plugin executables as child processes and communicating with them via GIMP's Wire Protocol (IPC over pipes).

This project is currently in the **research/planning phase**. See `docs/GIMP_Bridge_Summary.md` for the technical investigation and architecture details (written in Japanese).

## Architecture

### Core Concept

```
CSP Plugin (C++) → shared memory/temp buffer → spawn GIMP plugin EXE as child process
                ↕ Wire Protocol over pipes (tile transfer)
                → write result back to CSP layer
```

### Key Components (planned)

| Module | Role |
|--------|------|
| `libgimpwire.dll` | Wire Protocol serialization/deserialization — use unmodified from GIMP source |
| `libgimpbase.dll` | Basic types and parameter definitions — use unmodified |
| `MyGimpHost.dll` | Custom host-side stubs for `libgimp` procedures (proprietary) |

### Minimum Host Stubs Required

The host emulator must implement stubs for:
- `gimp_image_list()`
- `gimp_drawable_get()`
- `gimp_tile_get()` / `gimp_tile_put()` — the core tile transfer layer
- `gimp_display_list()`
- UI/dialog calls — stub to no-op (use `GIMP_RUN_NONINTERACTIVE` to minimize these)

### Licensing

All GIMP libraries (`libgimpwire`, `libgimpbase`, `libgimp`) are **LGPL v3**. Use dynamic linking (DLL) to keep the CSP plugin source proprietary. Only modifications to GIMP libraries themselves require disclosure. The GIMP core/app (GPL v3) is never linked directly.

## Build System

No build system exists yet. Planned approach:
- CMake or Meson for C++ compilation
- Dynamic linking against GIMP libraries (`libgimpwire.dll`, `libgimpbase.dll`)

## Reference Documentation

- GIMP 3.0 API Reference: `developer.gimp.org/resource/api/`
- Wire Protocol overview: `developer.gimp.org/resource/about-plugins/`
- Writing a plug-in tutorial (C samples): `developer.gimp.org/resource/writing-a-plug-in/`

## Implementation Strategy

Per the research doc recommendation: build `libgimpwire` + `libgimpbase` directly into the project unmodified, implement only the host-side stubs for `libgimp`, and start with a single filter plugin prototype using `GIMP_RUN_NONINTERACTIVE` to avoid GUI dialog handling complexity.
