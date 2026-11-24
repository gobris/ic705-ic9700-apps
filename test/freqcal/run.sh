#!/bin/sh
# Host simulation of FREQCAL (no radio needed). Builds the harness with the system gcc and runs
# a few scenarios. The simulated radio error is +0.02 ppm; each scenario prints the last result lines.
# Needs the ICAP SDK header (see apps/freqcal/build.sh): ICAP_SDK=/path/to/icap-sdk ./run.sh
set -e
cd "$(dirname "$0")"
SDK="${ICAP_SDK:-../../../icap-sdk}"
gcc -O2 -I"$SDK" -o harness harness.c -lm

echo "== 1. WWV 10 MHz, clean carrier (expect +0.0200 ppm)"
./harness x 3000 1e9 130000 0 9998500 10000000 | grep -E " L[3567] " | tail -4

echo "== 2. Second carrier 0.93 Hz above, -10 dB (expect +0.0200 ppm, accepted)"
./harness x 3000 1e9 130000 0.3 9998500 10000000 0.93 | grep -E " L[45] " | tail -2

echo "== 3. Second carrier 0.93 Hz above, -2 dB (expect windows rejected: 2 carriers)"
./harness x 3000 1e9 130000 0.8 9998500 10000000 0.93 | grep -E " L4 " | tail -1

echo "== 4. Strong spur 23.4 Hz below the carrier (expect +0.0200 ppm, spur ignored)"
./harness x 3000 1e9 200000 2.0 9998500 10000000 -23.4 | grep -E " L5 " | tail -1

echo "== 5. RWM 9996 kHz picked from the dial (expect ref 9.996000 MHz RWM)"
./harness x 3000 1e9 70000 0 9994500 9996000 | grep -E " L0 | L3 " | tail -2

echo "== 6. Unrelated dial 7.1 MHz (expect a tuning hint)"
./harness x 3000 1e9 3000 0 7100000 7100000 | grep -E " L2 " | head -1

echo "== 7. Step calibration, REF changed 40 s after the popup (expect step=+0.25000)"
./harness cal 3000 40000 300000 | grep -E "POPUP" | head -1
grep -E "^CAL|^step=" FREQCAL.TXT

rm -f harness FREQCAL.TXT
