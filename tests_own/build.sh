#!/bin/sh
# Genera y compila la prueba de CPU: build.sh <semilla> <funciones> -> fuzz.exe
set -e
cd "$(dirname "$0")"
Z=${ZIG:-$HOME/.zig/zig-x86_64-windows-0.17.0/zig.exe}
[ -f gen.exe ] || $Z cc -O2 gen.c -o gen.exe
./gen.exe "${1:-1}" "${2:-100}" > fuzz.c
F="-target mipsel-freestanding-none -mcpu=mips1 -O2 -fno-pic -mno-abicalls -ffreestanding -fno-builtin -mno-check-zero-division -Wno-everything"
$Z cc $F -c crt.c -o crt.o
$Z cc $F -c main.c -o main.o
$Z cc $F -c fuzz.c -o fuzz.o
$Z cc -target mipsel-freestanding-none -mcpu=mips1 -nostdlib -T link.ld crt.o main.o fuzz.o -o fuzz.elf
./elf2exe.exe fuzz.elf fuzz.exe
