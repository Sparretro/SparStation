#!/bin/sh
# Compila el banco de pruebas del PC: tools/buildpc.sh [salida.exe] [opciones extra para el compilador]
cd "$(dirname "$0")/.."
out=${1:-out/psxpc.exe}; shift 2>/dev/null
${ZIG:-zig} cc -O2 -DPSX_TTY "$@" -Icore core/*.c pc/main.c pc/dis.c -o "$out"
