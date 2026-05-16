"""
PlatformIO post-build script.
Copies the compiled firmware.bin to flasher/firmware.bin automatically
after every successful build, so the web flasher always has the latest binary.

Usage: automatically runs when `extra_scripts = post:scripts/copy_firmware.py`
is set in platformio.ini.
"""
Import("env")
import shutil
import os

def copy_firmware(source, target, env):
    src  = str(target[0])
    dest = os.path.join(env.subst("$PROJECT_DIR"), "flasher", "firmware.bin")

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy(src, dest)

    size_kb = os.path.getsize(dest) // 1024
    print(f"\n{'─'*55}")
    print(f"  🚀  firmware.bin  →  flasher/firmware.bin  ({size_kb} KB)")
    print(f"{'─'*55}\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)
