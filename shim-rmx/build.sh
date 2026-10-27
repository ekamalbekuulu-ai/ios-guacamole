#!/bin/bash
# GPU-probe shim for guacamole (SM8150 / Adreno 640), ported from
# ios-rmx1931's shim (same SoC): full a640 bring-up in bare metal with a
# per-stage log persisted to the end of userdata.
#   build.sh [PAYLOAD] [OUT]
# SHIM_CFLAGS=-DSHIM_SHOWLOG shows the previous log on screen instead.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
NM=${NM:-aarch64-linux-gnu-nm}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra
	-DSHIM_GPUPROBE ${SHIM_CFLAGS:-}"
SRCS="start.S shim.c console.c log.c util.c mmu.c fdt.c loader.c ufs.c spmi.c
	sde.c showlog.c diag.c disptest.c gpuprobe.c gpufw.S smmuprobe.c ramlog.c"
mkdir -p out
objs=""
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/${f%.*}.o"
	objs="$objs out/${f%.*}.o"
done
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N $objs -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
size=$($NM out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
lz4 -q -9 -B7 -f out/Image out/Image.lz4
if [ -n "${1:-}" ]; then cp "$1" out/ramdisk.bin; else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
mkbootimg --header_version 2 --kernel out/Image.lz4 --ramdisk out/ramdisk.bin \
	--dtb "${DTB:-../dtb/boot_unpacked/dtb}" --base 0 --kernel_offset 0x8000 \
	--ramdisk_offset 0x1000000 --tags_offset 0x100 --dtb_offset 0x1f00000 \
	--pagesize 4096 --os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o out/shim-rmx-boot.img
ls -l out/shim-rmx-boot.img
