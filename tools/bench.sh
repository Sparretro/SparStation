#!/bin/sh
# Compila la ROM de pruebas con una carpeta NitroFS y la mide en melonDS (modo DS, sin ventana).
# uso: tools/bench.sh <carpeta nitro en ds/> [frames DS] [frame de inicio de la medida] [nombre]
# Ej.: tools/bench.sh nitro_klg 6000 600 klg   -> out/b/klg.png y las cifras BENCH
set -e
cd "$(dirname "$0")/.."
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
dir=$1; frames=${2:-6000}; snap=${3:-600}; name=${4:-$1}
[ -f "ds/$dir/bios.bin" ] || cp bios/openbios.bin "ds/$dir/bios.bin"
rm -f ds/SparStation_test.nds ds/SparStation_test.elf
make -C ds SELFTEST=1 NITRODIR="$dir" EXTRA="$EXTRA" 2>&1 | grep -E "error|Error" && exit 1
mkdir -p out/b
cp ds/SparStation_test.nds "out/b/$name.nds"
root=$(pwd -W | tr / '\')
DSB_SNAP=$snap tools/dsbench.exe "$root/tools/melonds/melondsds_libretro.dll" "$root/out/b/$name.nds" "$frames" "$root/out/b/$name.png" 2>/dev/null
