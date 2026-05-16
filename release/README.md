# /release — GitHub Release assets

This folder is the **staging area** for binaries attached to a GitHub release.

After a successful PlatformIO build, copy the following files here from
`.pio/build/xiao_esp32s3/` and upload them as assets on the GitHub release page:

| File | Source | Flash offset |
|---|---|---|
| `firmware.bin`   | `.pio/build/xiao_esp32s3/firmware.bin`   | `0x10000` (or `0x0` if merged) |
| `bootloader.bin` | `.pio/build/xiao_esp32s3/bootloader.bin` | `0x0` |
| `partitions.bin` | `.pio/build/xiao_esp32s3/partitions.bin` | `0x8000` |
| `manifest.json`  | this folder (see template below)         | — |

The CI workflow at `.github/workflows/build.yml` automates this on every tag
push matching `v*` — you do not need to upload manually.

## manifest.json template (for esp-web-tools, optional)

```json
{
  "name": "Claude Token Meter",
  "version": "1.1.0",
  "home_assistant_domain": "claude_token_meter",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "firmware.bin",   "offset": 65536 }
      ]
    }
  ]
}
```

## Local build & stage

```bash
pio run -e xiao_esp32s3
cp .pio/build/xiao_esp32s3/firmware.bin   release/
cp .pio/build/xiao_esp32s3/bootloader.bin release/
cp .pio/build/xiao_esp32s3/partitions.bin release/
```

> Binaries are **not** checked into git by default (see `.gitignore`).
> They are produced fresh by CI for each tagged release.
