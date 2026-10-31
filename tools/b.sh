#!/bin/sh
# Medida de referencia de Silent Hill con los ajustes del usuario (muy rápida, salto 1), sin el perfilador.
# Sin sonido (NO_SND): en el modo DS su memoria le quitaría caché de código al dynarec (en la DSi sobra).
# Tabla de bloques completa (como la DSi) en el Expansion Pak: con una pequeña los bloques chocan y se retraducen.
# Mide siempre los mismos frames de la PS1 (DSB_PSX), y con la VRAM de la PS1 en el Expansion Pak la caché
# de código cabe entera (como en la DSi, que tiene 3 MB): sin vaciados que falseen la medida.
# uso: tools/b.sh [nombre] [carpeta nitro]
cd "$(dirname "$0")/.."
touch ds/source/main.c
export DSB_PSX=${DSB_PSX:-100:600}
EXTRA="-DBENCH_QUALITY=2 -DBENCH_SKIP=1 -DBENCH_VRAM_PAK -DDYN_DS_RESERVE_KB=160 -DDYN_DS_SLOTS=0x80000 -DDYN_SLOTS_AT=0x09300000 -DNO_SND $EXTRA" sh tools/bench.sh ${2:-nitro_shg} 16000 -1 ${1:-ref} 2>/dev/null | grep -E 'BENCH|ms/frame|DSi'
