#!/bin/sh
# Genera y compila la prueba de instrucciones en crudo: build2.sh <semilla> <funciones> -> raw.exe
set -e
cd "$(dirname "$0")"
Z=${ZIG:-$HOME/.zig/zig-x86_64-windows-0.17.0/zig.exe}
[ rawgen.exe -nt rawgen.c ] || $Z cc -O2 rawgen.c -o rawgen.exe
./rawgen.exe "${1:-1}" "${2:-100}" > rawfuzz.c
F="-target mipsel-freestanding-none -mcpu=mips1 -O2 -fno-pic -mno-abicalls -ffreestanding -fno-builtin -mno-check-zero-division -Wno-everything"
$Z cc $F -c crt.c -o crt.o
$Z cc $F -c main2.c -o main2.o
$Z cc $F -c rawfuzz.c -o rawfuzz.o
$Z cc -target mipsel-freestanding-none -mcpu=mips1 -nostdlib -T link.ld crt.o main2.o rawfuzz.o -o raw.elf
./elf2exe.exe raw.elf raw.exe
