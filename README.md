# RhythmLink SiliFork Bridge

This is a separate Geode companion mod. It does not rebuild, replace, patch on
disk, or redistribute `peony.silicate.geode`. Geode loads the bridge DLL next
to the user's existing SiliFork DLL.

The bridge exposes SiliFork's authoritative replay tick, current replay, RNG
state and replay catalog to RhythmOverlay over `127.0.0.1:19438`. Commands are
executed only on Geometry Dash's main thread.

## Exact supported binary

- Mod: `peony.silicate` `v1.0.9`
- Geometry Dash: Windows `2.2081`
- Geode: `5.8.2`
- SiliFork DLL SHA-256:
  `27249D1F61C544A42ECD10E7D3084FB86FAAD935E418E1D690C679710F3C330B`
- Source `.geode` SHA-256:
  `291C7AA04D9223836665CC37DC345F384644B5ABEDFD163CA78FC4D7B80813BD`

The runtime also checks PE metadata and function signatures. A different
SiliFork build is rejected instead of calling unknown addresses.

## Install

1. Keep the existing `peony.silicate.geode` installed and enabled.
2. Put the built `sol.rhythmlink-silifork-bridge.geode` in the same Geode
   `mods` folder.
3. Start Geometry Dash, then start RhythmOverlay.

No ADB, USB forwarding, process injection, or modified SiliFork package is
needed. Both programs must run on the same Windows PC.

## Build

Push this directory as its own GitHub repository and run the included Actions
workflow. The artifact contains only the bridge `.geode` package.

Local format/protocol checks do not require Geode:

```powershell
cmake -S . -B build-tests -DSILIFORK_BRIDGE_HOST_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

To verify the exact SiliFork package used for the offsets:

```powershell
powershell -ExecutionPolicy Bypass -File tools/verify-target.ps1 `
  -GeodePath ..\peony.silicate.geode
```
