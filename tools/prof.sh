#!/bin/sh
# Perfil en melonDS con los ajustes del usuario: compila con el perfilador y saca el informe de funciones.
# Igual que tools/b.sh: frames 100-250 de la PS1 y VRAM de la PS1 en el Expansion Pak (sin vaciados).
# uso: tools/prof.sh [nombre] [carpeta nitro]
cd "$(dirname "$0")/.."
name=${1:-prof}; dir=${2:-nitro_shg}
root=$(pwd -W | tr / '\')
touch ds/source/main.c
EXTRA="-DBENCH_QUALITY=2 -DBENCH_SKIP=1 -DBENCH_VRAM_PAK -DDYN_DS_RESERVE_KB=160 -DDYN_DS_SLOTS=0x8000 -DSPAR_PROF -DPSX_BLKTAB -DNO_SND $EXTRA" sh tools/bench.sh "$dir" 10 -1 "$name" > /dev/null 2>&1
DSB_PSX=${DSB_PSX:-100:600} DSB_PROF="$root/out/b/$name.prof" tools/dsbench.exe "$root/tools/melonds/melondsds_libretro.dll" "$root/out/b/$name.nds" 9000 "$root/out/b/$name.png" 2>/dev/null | grep -E 'BENCH|ms/frame|DSi'
/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm -n --defined-only ds/SparStation_test.elf > "out/b/$name.sym"
tools/profrep.exe "out/b/$name.prof" "out/b/$name.sym" ${TOP:-40}
