# Third-party notices

## Design references
- **SkyCraft** (https://github.com/chasmlol/SkyCraft), MIT licence, Copyright (c) chasmlol.
  The passthrough architecture and protocol design (seqlock slots, SPSC rings, heartbeats) are adapted
  from its design document. If SkyCraft code is copied into this project, its MIT notice goes here
  next to the file list.
- **ValCraft** (https://github.com/LoAlCo/ValCraft), MIT licence (fork of SkyCraft). Design reference only.
- **ArkWeb** (https://github.com/luki-1/ArkWeb). No licence file is published, so it is used as a
  **design reference only**. No ArkWeb code is included. `src/common/` is an independent
  implementation of the same patterns.

## Runtime dependencies (not bundled)
- **BL2 Python SDK** (pyunrealsdk, unrealsdk, mods_base; https://github.com/bl-sdk), installed by the user in
  Borderlands 2. The BL2 mod runs on it.

## Tools
- capstone, pefile and numpy are used by the reverse-engineering scripts in `tools/re/` (installed with pip, not
  bundled).
