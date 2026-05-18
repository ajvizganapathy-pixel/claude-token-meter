"""
PlatformIO post-build script.
Produces a MERGED firmware image (bootloader + partition table + boot_app0 + app)
using esptool merge_bin, then copies to flasher/firmware.bin and firmware.bin (root).

ESP32-S3 flash offsets:
  0x00000  bootloader.bin
  0x08000  partitions.bin
  0x0e000  boot_app0.bin  (OTA partition marker — included if found)
  0x10000  firmware.bin   (application)

Configured via platformio.ini:
  extra_scripts = post:scripts/merge_firmware.py
"""
Import("env")
import shutil, os, sys, subprocess

def merge_firmware(source, target, env):
    build_dir     = env.subst("$BUILD_DIR")
    project_dir   = env.subst("$PROJECT_DIR")

    app_bin    = str(target[0])
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    dest       = os.path.join(project_dir, "flasher", "firmware.bin")
    dest_root  = os.path.join(project_dir, "firmware.bin")

    # Locate boot_app0.bin — search PlatformIO packages
    pio_home  = os.path.expanduser("~/.platformio")
    boot_app0 = None
    for root, dirs, files in os.walk(pio_home):
        if "boot_app0.bin" in files and "partitions" in root:
            boot_app0 = os.path.join(root, "boot_app0.bin")
            break

    os.makedirs(os.path.dirname(dest), exist_ok=True)

    # Prefer PlatformIO's bundled esptool.py; fall back to the module form.
    pio_esptool = os.path.join(pio_home, "packages", "tool-esptoolpy", "esptool.py")
    python_exe  = sys.executable   # the Python that PlatformIO is running under

    if os.path.exists(pio_esptool):
        base_cmd = [python_exe, pio_esptool]
    else:
        base_cmd = [python_exe, "-m", "esptool"]

    cmd = base_cmd + ["--chip", "esp32s3", "merge_bin",
                      "-o", dest,
                      "0x0",    bootloader,
                      "0x8000", partitions]
    if boot_app0 and os.path.exists(boot_app0):
        cmd += ["0xe000", boot_app0]
    cmd += ["0x10000", app_bin]

    print(f"\n{'='*55}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  [WARN] merge_bin failed:\n{result.stderr.strip()}")
        print("  Falling back to app-only copy (flash offset must be 0x10000 in this case)")
        shutil.copy(app_bin, dest)
    else:
        shutil.copy(dest, dest_root)

    size_kb = os.path.getsize(dest) // 1024
    print(f"  firmware.bin -> flasher/firmware.bin  ({size_kb} KB)")
    print(f"{'='*55}\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)
