#!/bin/sh
# Prueba de CPU diferencial: misma prueba aleatoria en el intérprete del PC y en el dynarec (melonDS).
# uso: tests_own/run.sh <semilla> [funciones]
cd "$(dirname "$0")/.." || exit 1
seed=${1:-1}; nf=${2:-100}
tests_own/build.sh "$seed" "$nf" > /dev/null 2>&1 || { echo "no compila la semilla $seed"; exit 1; }
n=$(( (nf + 3) * 4 ))
w=$(pwd -W | tr / '\')
out/psxblk.exe bios/openbios.bin "$w\tests_own\fuzz.exe" 300 "$w\out\fz" --dump "180000:$n:$w\out\fz_pc.bin" > /dev/null 2>&1
cp tests_own/fuzz.exe ds/nitro_fuzz/selftest.exe
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
rm -f ds/SparStation_test.nds ds/SparStation_test.elf
[ "$(cat ds/build_test/.extra 2>/dev/null)" != "nitro_fuzz -DNO_SND" ] && { rm -rf ds/build_test; mkdir -p ds/build_test; echo "nitro_fuzz -DNO_SND" > ds/build_test/.extra; }; make -C ds SELFTEST=1 NITRODIR=nitro_fuzz EXTRA=-DNO_SND 2>&1 | grep -E "error" && exit 1
cp ds/SparStation_test.nds out/b/fuzz.nds
DSB_SNAP=60 DSB_PSXMEM="180000:$n:$w\out\fz_ds.bin" tools/dsbench.exe "$w\tools\melonds\melondsds_libretro.dll" "$w\out\b\fuzz.nds" ${DSFRAMES:-1500} "$w\out\b\fuzz.png" > /dev/null 2>&1
if cmp -s out/fz_pc.bin out/fz_ds.bin; then echo "semilla $seed: IGUAL"; exit 0; fi
echo "semilla $seed: DISTINTO"
od -A d -t x4 -w4 -v out/fz_pc.bin > out/fz_pc.txt
od -A d -t x4 -w4 -v out/fz_ds.bin > out/fz_ds.txt
diff out/fz_pc.txt out/fz_ds.txt | head -10
exit 1
