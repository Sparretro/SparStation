#!/bin/sh
# Perfil en melonDS con los ajustes del usuario: compila con el perfilador y saca el informe de funciones.
# uso: tools/prof.sh [nombre] [carpeta nitro] [frames] [inicio]
cd "$(dirname "$0")/.."
name=${1:-prof}; dir=${2:-nitro_shg}; frames=${3:-3000}; snap=${4:-1000}
root=$(pwd -W | tr / '\')
touch ds/source/main.c
EXTRA="-DBENCH_QUALITY=2 -DBENCH_SKIP=1 -DSPAR_PROF -DPSX_BLKTAB $EXTRA" sh tools/bench.sh "$dir" 10 -1 "$name" > /dev/null 2>&1
DSB_SNAP=$snap DSB_PROF="$root/out/b/$name.prof" tools/dsbench.exe "$root/tools/melonds/melondsds_libretro.dll" "$root/out/b/$name.nds" "$frames" "$root/out/b/$name.png" 2>/dev/null | grep -E 'BENCH|ms/frame'
/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm -n --defined-only ds/SparStation_test.elf > "out/b/$name.sym"
tools/profrep.exe "out/b/$name.prof" "out/b/$name.sym" ${TOP:-40}
