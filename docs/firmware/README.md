# /docs/firmware — Hosted firmware drop

GitHub Pages serves this folder at:

    https://ajvizganapathy-pixel.github.io/claude-token-meter/firmware/

Drop release binaries here if you want to host them alongside the web flasher
(for OTA pulls, esp-web-tools `manifest.json`, or direct downloads).

| File | Flash offset | Notes |
|---|---|---|
| `firmware.bin`   | `0x10000` (or `0x0` if merged) | Application image |
| `bootloader.bin` | `0x0`     | Second-stage bootloader |
| `partitions.bin` | `0x8000`  | Partition table |
| `manifest.json`  | —         | esp-web-tools install manifest (optional) |

Binaries are **not** committed to git by default (see `.gitignore`).
The recommended source of truth is the GitHub Releases page, which is
populated automatically by `.github/workflows/build.yml` on every `v*` tag.
