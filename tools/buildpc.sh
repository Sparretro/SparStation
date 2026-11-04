#!/bin/sh
# Compila el banco de pruebas del PC: tools/buildpc.sh [salida.exe] [opciones extra para el compilador]
cd "$(dirname "$0")/.."
out=${1:-out/psxpc.exe}; shift 2>/dev/null
${ZIG:-$HOME/.zig/zig-x86_64-windows-0.17.0/zig.exe} cc -O2 -DPSX_TTY "$@" -Icore core/*.c pc/main.c pc/dis.c $PCX -o "$out"
