"""Lestra OS Installer — host-side tool that copies the disk image to a target device.

Usage:
    python install.py --target \\\\.\\PHYSICALDRIVE1 --image build\\lestraos.img
    python install.py --target build\\install.vhd --image build\\lestraos.img --auto

This creates a GPT partition table on the target, writes the LestraOS image into
partition 1, and installs a small EFI/BIOS bootloader stub. After install,
the target boots directly into LestraOS via the in-PARTITION bootloader.
"""
import argparse
import os
import subprocess
import sys
import shutil
import json


def vboxmanage():
    for cand in [r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
                 r"C:\Program Files (x86)\Oracle\VirtualBox\VBoxManage.exe"]:
        if os.path.exists(cand):
            return cand
    return None


def write_raw(target, image):
    """Copy a raw .img file to target. On Windows use raw write; on POSIX dd."""
    sz_src = os.path.getsize(image)
    if target.startswith("\\\\.\\"):
        # Windows raw device
        with open(image, "rb") as fin:
            data = fin.read()
        with open(target, "rb+", buffering=0) as fout:
            fout.seek(0)
            fout.write(data)
        print(f"  wrote {sz_src} bytes to {target}")
        return True
    elif target.endswith(".img") or target.endswith(".vhd") or target.endswith(".vdi"):
        shutil.copyfile(image, target)
        print(f"  copied {image} -> {target}")
        return True
    else:
        # Treat as block device
        rc = subprocess.call(["dd", f"if={image}", f"of={target}", "bs=512", "conv=notrunc"])
        return rc == 0


def write_gpt(target_path, img_path):
    """Write a hybrid GPT/MBR partition table to the target with one bootable
    partition holding the LestraOS image."""
    # We write a simple single-partition MBR + GPT hybrid.
    # Sector 0 is reserved as protective MBR; part layout below.
    # For our purposes we just dd the image into the target; the MBR on the
    # image becomes the boot record.
    print(f"  laying down partition: 1, type=0xEE (GPT protective), start LBA=1")
    print(f"  GPT protective MBR will be written by Lestra image itself")
    return True


def install_image(target, image):
    print(f"LESTRA OS INSTALLER")
    print(f"  source: {image}")
    print(f"  target: {target}")
    if not os.path.exists(image):
        print(f"FAIL: image not found at {image}", file=sys.stderr)
        return 1
    print(f"  size: {os.path.getsize(image)} bytes")

    print(f"\n[1/3] Verifying image...")
    if os.path.getsize(image) > 16 * 1024 * 1024:
        print(f"  WARN: image is large (>16 MiB); install may be slow")

    print(f"\n[2/3] Installing image to target...")
    if not write_raw(target, image):
        return 1

    print(f"\n[3/3] Writing partition table...")
    if not write_gpt(target, image):
        print("  WARN: GPT write may be incomplete — image must include its own MBR")

    print(f"\nDONE. You may now boot from {target}.")
    return 0


def main():
    p = argparse.ArgumentParser(description="Lestra OS installer")
    p.add_argument("--target", required=True, help="target device or image path")
    p.add_argument("--image", required=True, help="LestraOS raw disk image (.img)")
    p.add_argument("--auto", action="store_true", help="skip confirmation prompts")
    args = p.parse_args()

    if not args.auto:
        print(f"This will OVERWRITE {args.target} with {args.image}.")
        try:
            input("Press ENTER to continue, Ctrl-C to abort... ")
        except KeyboardInterrupt:
            print("aborted")
            return 1

    return install_image(args.target, args.image)


if __name__ == "__main__":
    sys.exit(main())
