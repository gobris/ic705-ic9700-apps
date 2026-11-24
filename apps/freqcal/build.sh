#!/bin/sh
# Build FREQCAL.APP (IC-705 and IC-9700) with arm-none-eabi-gcc and python3.
#
# Needs the ICAP SDK by Dawid SQ6EMM: https://github.com/sq6emm/icap-sdk
# By default it is expected next to this repository:
#   parent/
#   ├── icap-sdk/            (git clone https://github.com/sq6emm/icap-sdk)
#   └── ic705-ic9700-apps/
# or point ICAP_SDK at it:  ICAP_SDK=/path/to/icap-sdk ./build.sh
set -e
cd "$(dirname "$0")"
SDK="${ICAP_SDK:-../../../icap-sdk}"
if [ ! -f "$SDK/icap.h" ]; then
    echo "ICAP SDK not found at $SDK (set ICAP_SDK=/path/to/icap-sdk)" >&2
    exit 1
fi
T="$SDK/tools"
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin \
    -fno-math-errno -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors \
    -Wall -I$SDK"
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker"
arm-none-eabi-gcc $CF -c "$T/icap_start.S" -o icap_start.o
arm-none-eabi-gcc $CF -c freqcal.c -o freqcal.o
arm-none-eabi-gcc $CF $LF icap_start.o freqcal.o -lgcc -o freqcal.elf
python3 "$T/mkapp.py" freqcal.elf FREQCAL.APP --name FREQCAL --models 3 --stack 8192 --abi 3 --background
rm -f icap_start.o freqcal.o
echo "built $(pwd)/FREQCAL.APP"
