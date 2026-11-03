# SparStation

Emulador de PlayStation (PS1) para una **Nintendo DSi** real, creado por **Spar Retro** · [youtube.com/@sparretro](https://youtube.com/@sparretro)

## Estructura

- `core/` — núcleo portable en C: CPU (intérprete + dynarec), bus, GPU, GTE, DMA, temporizadores, CD-ROM, mando, SPU.
- `ds/` — frontend para DSi (libnds / devkitPro). `make` genera `SparStation.nds`.
- `pc/` — arnés de pruebas para PC y desensamblador MIPS.
- `tools/`, `tests_own/` — herramientas de perfilado y pruebas.
- `parches/` — trucos/parches (`.cht`).

## Uso

Copia `SparStation.nds` a la SD y pon tus juegos (`.cue` + `.bin`) en `sd:/ps1/`. No se incluye ningún juego.

## Créditos

BIOS: [OpenBIOS](https://github.com/grumpycoders/pcsx-redux) de PCSX-Redux (licencia MIT), incluida en `ds/data/openbios.bin`.
