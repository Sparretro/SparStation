#!/bin/sh
# Medida de referencia de Silent Hill con los ajustes del usuario (muy rápida, salto 1), sin el perfilador.
# uso: tools/b.sh [nombre] [carpeta nitro]
cd "$(dirname "$0")/.."
touch ds/source/main.c
EXTRA="-DBENCH_QUALITY=2 -DBENCH_SKIP=1 -DDYN_DS_RESERVE_KB=160 $EXTRA" sh tools/bench.sh ${2:-nitro_shg} 3000 1000 ${1:-ref} 2>/dev/null | grep -E 'BENCH|ms/frame'
