// SparStation para Nintendo DSi: frontend. La PS1 se dibuja en la pantalla de arriba (escalada por
// hardware) y la de abajo sirve para elegir juego y ver los fps.
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "psx.h"
#include "hud.h"
#include "gx.h"
#include "snd.h"
#ifdef SPAR_PROF
#include "prof.h"
#endif
#ifndef PSX_SELFTEST
#include "openbios_bin.h"
#endif
#include <filesystem.h>
#ifdef PSX_SELFTEST
#include <calico/nds/gbacart.h>
#include <calico/arm/mpu.h>
#include <calico/system/mutex.h>

#ifdef STACK_PROBE
// Medida: cuánto baja como mucho la pila del hilo principal en la DTCM (lo que quede libre puede ser tabla).
// Se pinta el hueco al arrancar; gx_dbg.dyn = bytes usados (lo imprime dsbench al final)
extern u32 __dtcm_bss_end[], __sp_usr[];
static u32 stack_paint_depth;
static __attribute__((noinline)) void stack_paint(void)
{
	u32 *p = __dtcm_bss_end, *sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	stack_paint_depth = (u32)((u8 *)__sp_usr - (u8 *)sp);
	while (p < sp - 64) *p++ = 0x5a5aa5a5;
}
static u32 stack_used(void)
{
	u32 *p = __dtcm_bss_end;
	while (p < __sp_usr && *p == 0x5a5aa5a5) p++;
	return (u32)((u8 *)__sp_usr - (u8 *)p);
}
#endif
#endif

#define GAMES_DIR "sd:/ps1"
#define MAX_GAMES 64

#ifdef DYN_DUMP_PC
u32 dyn_dbg_addr[2];
#endif
static FILE *disc;
static volatile u32 vblanks;

static void flip_in_vblank(void);
static volatile u32 frame_beat;   // lo sube el bucle principal en cada frame de la PS1

// Si el bucle principal lleva ~3 s sin terminar un frame, se muestra dónde está la CPU de la PS1
static void watchdog(void)
{
	static u32 last_beat, still;
	if (frame_beat != last_beat) { last_beat = frame_beat; still = 0; return; }
	if (++still == 180) {
		extern u32 dyn_where;
		printf("\x1b[21;0H\x1b[31;1mCOLGADO pc=%08lx t=%08lx\nhasta=%08lx donde=%lu\x1b[37;1m", cpu.pc, psx_cycles, psx_run_until, dyn_where);
		fflush(stdout);
		still = 0;
	}
}

static void on_vblank(void) { vblanks++; snd_vblanks++; gx_dbg.vbl++; flip_in_vblank(); if (gpu_hw) gx_vblank(); watchdog(); }

static int read_exe(void *user, u8 *dst, u32 len) { return fread(dst, 1, len, (FILE *)user) == len; }

// Disco reducido para pruebas en modo DS (nitro:/disc.idx + disc.dat, ver tools/mksubset.c)
static u32 *sub_lbas, sub_count;
static FILE *disc_xa;   // el del hilo del audio (snd.c): cada hilo con su archivo, como en la SD
static Mutex sub_mtx;   // NitroFS no aguanta dos hilos leyendo a la vez (la SD sí: libfat lleva su mutex)

static int read_subset_from(FILE *f, u32 lba, u8 *out)
{
	u32 lo = 0, hi = sub_count;
	int ok;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		if (sub_lbas[mid] < lba) lo = mid + 1; else hi = mid;
	}
	mutexLock(&sub_mtx);
	ok = lo < sub_count && sub_lbas[lo] == lba && !fseek(f, (long)lo * 2352, SEEK_SET) && fread(out, 1, 2352, f) == 2352;
	mutexUnlock(&sub_mtx);
	if (!ok) memset(out, 0, 2352);   // sector que no se grabó: vacío
	return 1;
}

static int read_subset(void *user, u32 lba, u8 *out) { (void)user; return read_subset_from(disc, lba, out); }
static int read_subset_xa(void *user, u32 lba, u8 *out) { (void)user; return read_subset_from(disc_xa, lba, out); }

#ifdef DA_TEST
// Prueba del hilo de la música en melonDS (que no tiene la SD de la DSi): tono triangular de 441 Hz con la fase
// sacada del sector (sale continuo solo si no se pierde, repite ni desordena nada) y una espera de DA_TEST µs por
// lectura, como la de la SD (el hilo queda bloqueado y la emulación sigue)
static u32 test_da_read(void *user, u32 lba, u32 n, u8 *out)
{
	s16 *p = (s16 *)out;
	(void)user;
	threadSleep(DA_TEST);
	for (u32 i = 0; i < n * 588; i++) {
		u32 t = (lba * 588 + i) % 100;
		s32 tri = t < 50 ? (s32)t * 640 - 16000 : 16000 - (s32)(t - 50) * 640;
		p[i * 2] = (s16)tri;
		p[i * 2 + 1] = (s16)(-tri / 2);
	}
	return n;
}
#endif

static int open_subset(void)
{
	FILE *f = fopen("nitro:/disc.idx", "rb");
	u32 total;
	long n;
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f) / 4 - 1;
	fseek(f, 0, SEEK_SET);
	sub_lbas = malloc(n * 4);
	if (!sub_lbas) { printf("Disco reducido: sin memoria\n"); fclose(f); return 0; }
	if (fread(&total, 4, 1, f) != 1 || fread(sub_lbas, 4, n, f) != (size_t)n) { fclose(f); return 0; }
	fclose(f);
	sub_count = (u32)n;
	disc = fopen("nitro:/disc.dat", "rb");
	if (!disc) return 0;
	cdrom_set_disc(read_subset, NULL, total);
	{   // índice de pistas del disco entero (tools/cuetrk), si el juego tiene pistas de audio
		FILE *t = fopen("nitro:/disc.trk", "rb");
		u32 nt = 0, v[3];
		static PsxTrack trk[99];
		if (t && fread(&nt, 4, 1, t) == 1) {
			if (nt > 99) nt = 99;
			for (u32 i = 0; i < nt && fread(v, 4, 3, t) == 3; i++) { trk[i].audio = (u8)v[0]; trk[i].start = v[1]; trk[i].pre = v[2]; }
			cdrom_set_tracks(trk, (int)nt);
		}
		if (t) fclose(t);
	}
	disc_xa = fopen("nitro:/disc.dat", "rb");
	if (disc_xa) snd_set_disc(read_subset_xa);
#ifdef DA_TEST
	snd_set_cdda(test_da_read);
#endif
	return 1;
}

// Salida de texto de la PS1 en la consola de abajo (solo lo que no sea ruido de la BIOS)
static void tty_out(int c) { putchar(c); }

// El disco son los archivos de su .cue (una pista por archivo, como los volcados de redump) seguidos. Hay tres
// lectores, cada uno con su archivo abierto y su posición: el del CD (lo que lee el juego), el del audio XA y el
// de las pistas de audio, que leen por delante (snd.c). Si compartieran archivo, cada lectura saltaría de un
// sitio a otro del disco: en la SD, buscar cuesta y se pierde el búfer.
static CueDisc cue;
typedef struct { FILE *f; int fi, buf; long pos; } DiscRd;
// El de las pistas de audio lee bloques grandes desde su hilo (snd.c): sin búfer de stdio, directo al anillo.
static DiscRd rd_cd = { NULL, -1, 32 * 1024, -1 }, rd_xa = { NULL, -1, 16 * 1024, -1 }, rd_da = { NULL, -1, 0, -1 };

// n sectores seguidos de una vez (como mucho hasta el final de su archivo); devuelve cuántos se leyeron
static u32 disc_read_n(DiscRd *r, u32 lba, u32 n, u8 *out)
{
	u32 s;
	int i = cue_locate(&cue, lba, &s);
	long want;
	if (i < 0 || !n) return 0;
	if (i != r->fi) {   // otro archivo (otra pista): se cambia el que tiene abierto
		if (r->f) fclose(r->f);
		r->f = fopen(cue.file[i].path, "rb");
		r->fi = r->f ? i : -1;
		r->pos = -1;
		if (!r->f) return 0;
		setvbuf(r->f, NULL, r->buf ? _IOFBF : _IONBF, (size_t)r->buf);
	}
	if (n > cue.file[i].sectors - s) n = cue.file[i].sectors - s;
	want = (long)s * 2352;
	if (want != r->pos && fseek(r->f, want, SEEK_SET)) { r->pos = -1; return 0; }   // leyendo seguido, sin fseek
	if (fread(out, 1, n * 2352, r->f) != n * 2352) { r->pos = -1; return 0; }
	r->pos = want + (long)n * 2352;
	return n;
}

static int disc_read(DiscRd *r, u32 lba, u8 *out) { return disc_read_n(r, lba, 1, out) == 1; }

static int read_sector(void *user, u32 lba, u8 *out) { (void)user; return disc_read(&rd_cd, lba, out); }
static int read_sector_xa(void *user, u32 lba, u8 *out) { (void)user; return disc_read(&rd_xa, lba, out); }
static u32 read_sectors_da(void *user, u32 lba, u32 n, u8 *out) { (void)user; return disc_read_n(&rd_da, lba, n, out); }

static u32 file_sectors(const char *path)
{
	struct stat st;
	return stat(path, &st) ? 0 : (u32)(st.st_size / 2352);
}

// Abre un .cue (con todas sus pistas, en la misma carpeta) o un .bin directamente
static int open_disc(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (ext && strcasecmp(ext, ".cue") == 0) {
		if (!cue_parse(path, GAMES_DIR "/", &cue, file_sectors) || !cue.file[0].sectors) return 0;
	} else {
		memset(&cue, 0, sizeof cue);
		snprintf(cue.file[0].path, sizeof cue.file[0].path, "%s", path);
		cue.file[0].sectors = cue.total = file_sectors(path);
		cue.nfile = 1;
		if (!cue.total) return 0;
	}
	{ u8 probe[2352]; if (!read_sector(NULL, 0, probe)) return 0; }
	cdrom_set_disc(read_sector, NULL, cue.total);
	cdrom_set_tracks(cue.trk, cue.ntrk);
	snd_set_disc(read_sector_xa);
	snd_set_cdda(read_sectors_da);
	return 1;
}

// Ajustes de velocidad: se eligen en la lista de juegos y se guardan en la SD
#define CFG_PATH GAMES_DIR "/sparstation.cfg"
static int opt_quality, opt_skip, opt_cpu;   // calidad 0-3, frames saltados 0-3, CPU 0 = auto, 1-5 = cpu_pct
static int opt_hw = 1;                       // GPU: 1 = motor 3D de la DS (gx.c), 0 = por software
static int opt_snd = 1;                      // sonido (voces de la SPU y audio XA del CD)
static int opt_nwram = 1;                    // código traducido en la NWRAM de la DSi (si la hay, ver nwram_init)
static int cfg_version;                      // versión que escribió el .cfg (ver load_cfg)
static const char *const quality_name[4] = { "normal", "rapida", "muy rapida", "baja res" };
static const int cpu_pct[6] = { 0, 100, 85, 70, 50, 35 };
#define CPU_OPTS 6
// CPU automática: la PS1 corre con los ciclos justos para que el juego vaya a la velocidad real. Los juegos que
// miden el tiempo en vblanks (Silent Hill, Tomb Raider) van a su velocidad con menos imágenes; los demás, igual
// que al 100%. Hasta la v20 el tope era el 30%: Tomb Raider necesita ~23% (sus imágenes duran 3 vblanks en vez
// de 2) y se quedaba al 68% de velocidad. Por debajo del 30% el mando perdía los ACK: ahora sus tiempos van con
// la CPU (sio.c), y Tomb Raider va bien hasta al 15%.
// Hasta dónde baja es un ajuste de cada juego (cpu_min= en su .cfg; por defecto el 20%). Con Tomb Raider, en
// melonDS: tope 15% -> 94% de velocidad y 16,6 imágenes; 20% -> 95% y 17,4; 25% -> 89% y 18,8; 30% -> 85% y 19,0.
#define AUTO_MIN 256u                 // 100%
#ifndef CPU_MIN_DEFAULT
#define CPU_MIN_DEFAULT 20
#endif
static int opt_cpu_min = CPU_MIN_DEFAULT;    // % más bajo de la CPU automática
#ifndef AUTO_TARGET
#define AUTO_TARGET 92   // % de cada frame que la CPU automática intenta llenar (el resto, margen)
#endif
static int opt_target = AUTO_TARGET;        // por juego: objetivo= en su .cfg
static u32 auto_scale = AUTO_MIN, auto_max = 256u * 100 / CPU_MIN_DEFAULT;

static const char *cpu_name(void)
{
	static char s[8];
	if (!opt_cpu) return "auto";
	snprintf(s, sizeof s, "%d%%", cpu_pct[opt_cpu]);
	return s;
}

// Lee unos ajustes (los generales o los de un juego); 0 si no hay archivo
static int cfg_read(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	if (fscanf(f, "calidad=%d saltar=%d cpu=%d", &opt_quality, &opt_skip, &opt_cpu) != 3) opt_quality = opt_skip = opt_cpu = 0;
	if (fscanf(f, " gpu3d=%d", &opt_hw) != 1) opt_hw = 1;
	if (fscanf(f, " sonido=%d", &opt_snd) != 1) opt_snd = 1;
	if (fscanf(f, " nwram=%d", &opt_nwram) != 1) opt_nwram = 1;
	// Antes de la v21 se recomendaba saltar 1: con el 3D de la DS la GPU cuesta poco y saltar solo hacía ver
	// la mitad de las imágenes (Tomb Raider: 20 hechas, 10 vistas). Se vuelve a 0 una vez.
	if (fscanf(f, " version=%d", &cfg_version) != 1 || cfg_version < 21) opt_skip = 0;
	if (fscanf(f, " cpu_min=%d", &opt_cpu_min) != 1) opt_cpu_min = CPU_MIN_DEFAULT;   // desde la v26
	if (fscanf(f, " objetivo=%d", &opt_target) != 1) opt_target = AUTO_TARGET;   // desde la v29
	if (opt_target < 50 || opt_target > 100) opt_target = AUTO_TARGET;
	if (opt_cpu_min < 10 || opt_cpu_min > 100) opt_cpu_min = CPU_MIN_DEFAULT;
	opt_hw = opt_hw != 0;
	opt_snd = opt_snd != 0;
	opt_nwram = opt_nwram != 0;
	fclose(f);
	if (opt_quality < 0 || opt_quality > 3) opt_quality = 0;
	if (opt_skip < 0 || opt_skip > 3) opt_skip = 0;
	if (opt_cpu < 0 || opt_cpu >= CPU_OPTS) opt_cpu = 0;
	return 1;
}

static void load_cfg(void) { cfg_read(CFG_PATH); }

// Ajustes propios de un juego, junto a él y con su nombre (Silent Hill.cue -> Silent Hill.cfg)
static void game_cfg_path(char *buf, size_t n, const char *name)
{
	const char *dot = strrchr(name, '.');
	snprintf(buf, n, GAMES_DIR "/%.*s.cfg", dot ? (int)(dot - name) : (int)strlen(name), name);
}

static void cfg_write(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "calidad=%d saltar=%d cpu=%d\ngpu3d=%d\nsonido=%d\nnwram=%d\nversion=29\ncpu_min=%d\nobjetivo=%d\n", opt_quality, opt_skip,
	        opt_cpu, opt_hw, opt_snd, opt_nwram, opt_cpu_min, opt_target);
	fclose(f);
}

static void save_cfg(void) { cfg_write(CFG_PATH); }

// NWRAM de la DSi para el código traducido (dyn_hot_area): los bancos B y C, 256 KB cada uno, de 32 bits. Desde la
// RAM principal (16 bits) el ARM9 tarda unos 23 ciclos del bus en llenar una línea de su caché de instrucciones y
// desde una memoria de 32 bits unos 11 (GBATEK); con la CPU a 134 MHz el bus sigue igual de lento. La cabecera que
// escribe ndstool da al ARM9 sus 16 huecos de 32 KB, seguidos: C en 03700000h y B en 03740000h. calico no la usa ni le
// da región del MPU: se abre la 3 (libre) con caché de instrucciones y de datos, y se comprueba de verdad escribiendo
// los 512 KB y leyéndolos de vuelta con la caché vaciada. Si algo no cuadra, el código sigue en la RAM principal.
#define NWRAM_BASE 0x03700000u
#define NWRAM_SIZE 0x80000u
static int nwram_ok;

static void mpu_region3(u32 config)   // 0: quitarla
{
	u32 on = config ? 8 : 0;   // bit 3 = región 3; permisos de 4 bits en 12-15
	armMpuSetDataPerm((armMpuGetDataPerm() & ~0xf000u) | (config ? 0x3000u : 0));   // datos: leer y escribir
	armMpuSetCodePerm((armMpuGetCodePerm() & ~0xf000u) | (config ? 0x6000u : 0));   // instrucciones: ejecutar
	armMpuSetDCacheConfig((armMpuGetDCacheConfig() & ~8u) | on);
	armMpuSetICacheConfig((armMpuGetICacheConfig() & ~8u) | on);
	armMpuSetWrBufConfig((armMpuGetWrBufConfig() & ~8u) | on);   // escritura diferida, como la RAM principal
	armMpuSetRegion3(config);
	armDrainWriteBuffer();
}

// Si la prueba colgara la consola (ejecutar desde la NWRAM), queda este archivo y en el siguiente arranque se usa
// la RAM principal sin probar nada. Borrándolo se vuelve a intentar.
#define NWRAM_MARK GAMES_DIR "/nwram_prueba.txt"

// Diagnóstico: por qué no se usa la NWRAM (corto, para el log) y los registros en sd:/ps1/nwram_info.txt.
// En la v20-v25, con TWiLight Menu++, salía «sin NWRAM» sin saber qué comprobación fallaba.
static const char *nwram_why = "";

static void nwram_report(const char *what)
{
	FILE *f = fopen(GAMES_DIR "/nwram_info.txt", what[0] == 'a' && what[1] == 'n' ? "w" : "a");   // «antes» empieza el archivo
	if (!f) return;
	fprintf(f, "%s\nSCFG_EXT=%08lx MBK1(A)=%08lx MBK2-3(B)=%08lx %08lx MBK4-5(C)=%08lx %08lx\nMBK6-8(mapa A,B,C)=%08lx %08lx %08lx MBK9(protegidos)=%08lx\n\n",
	        what, REG_SCFG_EXT, *(vu32 *)0x04004040, *(vu32 *)0x04004044, *(vu32 *)0x04004048, *(vu32 *)0x0400404c,
	        *(vu32 *)0x04004050, REG_MBK_MAP_A, REG_MBK_MAP_B, REG_MBK_MAP_C, REG_MBK_SLOTWRPROT);
	fclose(f);
}

static int nwram_init(void)
{
	vu32 *p = (vu32 *)NWRAM_BASE;
	u32 n = NWRAM_SIZE / 4, i;
	FILE *m;
	int ok;
	if (!isDSiMode()) return 0;
	nwram_report("antes");
	if (!(REG_SCFG_EXT & SCFG_EXT_HAS_NWRAM)) { nwram_why = "SCFG"; nwram_report("SCFG_EXT sin acceso a la NWRAM (solo el ARM7 puede darlo)"); return 0; }
	// Los 16 huecos tienen que estar activos y ser del ARM9 (dueño 0). Con TWiLight Menu++ la cabecera de la ROM no
	// se aplica y pueden venir del ARM7 o del DSP: si el ARM7 no los protegió (MBK9), se los quedamos (nadie más los usa)
	for (i = 0; i < 8; i++) {
		if ((REG_MBK_SLOT_Bx(i) & 0x83) != 0x80 && !(REG_MBK_SLOTWRPROT & (1u << (8 + i)))) REG_MBK_SLOT_Bx(i) = (u8)(0x80 | (i << 2));
		if ((REG_MBK_SLOT_Cx(i) & 0x83) != 0x80 && !(REG_MBK_SLOTWRPROT & (1u << (16 + i)))) REG_MBK_SLOT_Cx(i) = (u8)(0x80 | (i << 2));
	}
	for (i = 0; i < 8; i++)
		if ((REG_MBK_SLOT_Bx(i) & 0x83) != 0x80 || (REG_MBK_SLOT_Cx(i) & 0x83) != 0x80) {
			nwram_why = "huecos";
			nwram_report("huecos B/C de otro procesador y protegidos (MBK9)");
			return 0;
		}
	if ((m = fopen(NWRAM_MARK, "r"))) { fclose(m); nwram_why = "marca"; nwram_report("la prueba anterior colgo (nwram_prueba.txt)"); return 0; }
	if ((m = fopen(NWRAM_MARK, "w"))) {
		fputs("SparStation: probando la NWRAM. Si este archivo sigue aqui, la prueba colgo la consola.\n", m);
		fclose(m);
	}
	REG_MBK_MAP_C = MBK_MAP_START(0xe0) | MBK_MAP_SIZE(MbkMapSize_256K) | MBK_MAP_END(0xe8);   // 03700000h-0373FFFFh
	REG_MBK_MAP_B = MBK_MAP_START(0xe8) | MBK_MAP_SIZE(MbkMapSize_256K) | MBK_MAP_END(0xf0);   // 03740000h-0377FFFFh
	mpu_region3(armMpuDefineRegion(NWRAM_BASE, CP15_PU_512K));
	for (i = 0; i < n; i++) p[i] = i * 0x9e3779b1u ^ 0x5a5a5a5au;   // cada palabra distinta: también se ven los espejos
	DC_FlushRange((void *)NWRAM_BASE, NWRAM_SIZE);
	for (i = 0; i < n && p[i] == (i * 0x9e3779b1u ^ 0x5a5a5a5au); i++) { }
	ok = i == n;
	if (ok) {   // y que el ARM9 ejecute desde ahí: «mov r0, #42; bx lr» al final de la zona
		u32 *f = (u32 *)(NWRAM_BASE + NWRAM_SIZE - 8);
		f[0] = 0xe3a0002a;
		f[1] = 0xe12fff1e;
		DC_FlushRange(f, 8);
		IC_InvalidateRange(f, 8);
		ok = ((int (*)(void))f)() == 42;
	}
	if (!ok) mpu_region3(0);
	remove(NWRAM_MARK);
	if (!ok) nwram_why = "prueba";
	nwram_report(ok ? "en uso para el codigo traducido" : "falla la prueba de lectura o de ejecucion");
	return ok;
}

// Tarjeta de memoria: una por juego, junto al .cue (Silent Hill.cue -> Silent Hill.mcr). Se guarda en la SD
// cuando el juego lleva un segundo sin escribir en ella (una partida son decenas de sectores seguidos).
static char card_path[256];
static u32 card_saved_writes, card_quiet, card_wait;

// Parches GameShark de un archivo de texto (Juego.cht): devuelve cuántos códigos se leyeron
static int load_cht(const char *path)
{
	static char text[16384];
	FILE *f = fopen(path, "rb");
	size_t n;
	cheat_clear();
	if (!f) return 0;
	n = fread(text, 1, sizeof text - 1, f);
	text[n] = 0;
	fclose(f);
	return cheat_parse(text);
}

static void card_open(const char *game)
{
	FILE *f;
	const char *dot = strrchr(game, '.');
	snprintf(card_path, sizeof card_path, "%.*s.mcr", dot ? (int)(dot - game) : (int)strlen(game), game);
	psx_memcard = malloc(128 * 1024);
	if (!psx_memcard) return;
	f = fopen(card_path, "rb");
	if (!f || fread(psx_memcard, 1, 128 * 1024, f) != 128 * 1024) psx_memcard_format(psx_memcard);
	if (f) fclose(f);
}

static void card_frame(void)
{
	FILE *f;
	if (!psx_memcard || psx_memcard_writes == card_saved_writes) return;
	if (psx_memcard_writes != card_quiet) { card_quiet = psx_memcard_writes; card_wait = 60; return; }
	if (card_wait && --card_wait) return;
	f = fopen(card_path, "wb");
	if (f) {
		if (fwrite(psx_memcard, 1, 128 * 1024, f) == 128 * 1024) card_saved_writes = card_quiet;
		fclose(f);
	}
}

static int single_fb;   // con la GPU 3D el banco B es de texturas: un solo buffer de fondo

static void apply_cfg(void)
{
	if (opt_hw && !gpu_hw) {
		gx_init();
		gpu_hw = &gx_hw;
		single_fb = 1;
	}
	gpu_rows_for(opt_quality > 2 ? 2 : opt_quality, 192);
	gpu_half_x = opt_quality == 3;   // baja res: además, la mitad de los píxeles de cada línea
	gpu_frameskip = (u8)opt_skip;
	auto_scale = AUTO_MIN;
	auto_max = 256u * 100 / (u32)opt_cpu_min;
	dyn_set_cpu_scale(opt_cpu ? 256u * 100 / (u32)cpu_pct[opt_cpu] : auto_scale);
}

static u32 name_hash(const char *s)   // FNV-1a sin distinguir mayúsculas (FAT tampoco las distingue)
{
	u32 h = 2166136261u;
	for (; *s; s++) h = (h ^ (u32)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s)) * 16777619u;
	return h;
}

static int pick_game(char *out, size_t outlen)
{
	static char names[MAX_GAMES][128];
	int n = 0, sel = 0;
	DIR *d = opendir(GAMES_DIR);
	struct dirent *e;
	if (!d) return 0;
	load_cfg();
	{   // primera pasada: los .bin que usa algún .cue (sus pistas) no salen en la lista
		static u32 used[512];
		int nused = 0;
		while ((e = readdir(d))) {
			const char *ext = strrchr(e->d_name, '.');
			char path[300], line[300];
			FILE *c;
			if (!ext || strcasecmp(ext, ".cue")) continue;
			snprintf(path, sizeof path, GAMES_DIR "/%s", e->d_name);
			if (!(c = fopen(path, "r"))) continue;
			while (fgets(line, sizeof line, c) && nused < 512) {
				char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
				if (strstr(line, "FILE") && q1 && q2) { *q2 = 0; used[nused++] = name_hash(q1 + 1); }
			}
			fclose(c);
		}
		rewinddir(d);
		while ((e = readdir(d)) && n < MAX_GAMES) {
			const char *ext = strrchr(e->d_name, '.');
			int hide = 0;
			if (!ext || (strcasecmp(ext, ".cue") && strcasecmp(ext, ".bin"))) continue;
			if (!strcasecmp(ext, ".bin")) {
				u32 h = name_hash(e->d_name);
				for (int k = 0; k < nused && !hide; k++) hide = used[k] == h;
			}
			if (!hide) snprintf(names[n++], 128, "%s", e->d_name);
		}
	}
	closedir(d);
	if (!n) return 0;
	// Cada juego recuerda sus ajustes (su .cfg al lado): al marcarlo se cargan; si no tiene, los generales.
	// Cambiar algo con un juego marcado lo apunta para ese juego.
	int gq = opt_quality, gs = opt_skip, gc = opt_cpu, ghw = opt_hw, gsnd = opt_snd, gnw = opt_nwram, gmin = opt_cpu_min, gtg = opt_target, cur = -1, own = 0;
	char gpath[300];
	for (;;) {
		int keys;
		if (sel != cur) {
			cur = sel;
			opt_quality = gq; opt_skip = gs; opt_cpu = gc; opt_hw = ghw; opt_snd = gsnd; opt_nwram = gnw; opt_cpu_min = gmin; opt_target = gtg;
			game_cfg_path(gpath, sizeof gpath, names[sel]);
			own = cfg_read(gpath);
		}
		consoleClear();
		printf("SparStation - elige juego\n(%s)\n\n", GAMES_DIR);
		{   // caben 11 en pantalla: la ventana sigue a la selección
			int top = sel < 11 ? 0 : sel - 10;
			for (int i = top; i < n && i < top + 11; i++) printf("%c %.28s\n", i == sel ? '>' : ' ', names[i]);
			if (n > 11) printf("\x1b[37m  (%d de %d)\x1b[37;1m\n", sel + 1, n);
		}
		printf("\x1b[15;0H\x1b[36;1mAjustes %s\x1b[37;1m\n", own ? "de este juego" : "generales");
		printf("L/R    calidad: %s\n", quality_name[opt_quality]);
		printf("SELECT saltar frames: %d\n", opt_skip);
		printf("Y      CPU PS1: %s\n", cpu_name());
		printf("X      GPU: %s\n", opt_hw ? "3D de la DS (rapida)" : "software (exacta)");
		printf("B      sonido: %s\n", opt_snd ? "si" : "no");
		printf("START  codigo: %s\n", !nwram_ok ? "RAM (sin NWRAM)" : opt_nwram ? "NWRAM (rapido)" : "RAM principal");
		printf("\x1b[37m(calidad: solo GPU software)\x1b[37;1m\n");
		printf("A: jugar");
		fflush(stdout);
		do { swiWaitForVBlank(); scanKeys(); keys = keysDown(); } while (!keys);
		if (keys & KEY_UP) sel = (sel + n - 1) % n;
		if (keys & KEY_DOWN) sel = (sel + 1) % n;
		if (keys & KEY_R) opt_quality = (opt_quality + 1) % 4;
		if (keys & KEY_L) opt_quality = (opt_quality + 3) % 4;
		if (keys & KEY_SELECT) opt_skip = (opt_skip + 1) % 4;
		if (keys & KEY_Y) opt_cpu = (opt_cpu + 1) % CPU_OPTS;
		if (keys & KEY_X) opt_hw ^= 1;
		if (keys & KEY_B) opt_snd ^= 1;
		if ((keys & KEY_START) && nwram_ok) opt_nwram ^= 1;
		if (keys & (KEY_R | KEY_L | KEY_SELECT | KEY_Y | KEY_X | KEY_B | KEY_START)) { cfg_write(gpath); own = 1; }
		if (keys & KEY_A) break;
	}
	save_cfg();   // los del juego elegido pasan a ser los generales (los que ven los juegos sin ajustes propios)
	snprintf(out, outlen, GAMES_DIR "/%s", names[sel]);
	return 1;
}

// Guion de botones para el banco de pruebas: nitro:/press.txt con «--press F:boton[:frames]» como el
// banco del PC; F cuenta frames de la PS1 desde el arranque (o desde el estado cargado)
#define MAX_PRESS 64
static struct { u32 at, len; u16 bit; } press[MAX_PRESS];
static int npress;
static u32 stop_at;   // «--stop F»: parar la emulación tras el frame F (para comparar con el PC)

static void load_script(const char *path)
{
	static const struct { const char *n; u16 b; } names[] = {
		{ "start", PAD_START }, { "select", PAD_SELECT }, { "x", PAD_CROSS }, { "o", PAD_CIRCLE },
		{ "tri", PAD_TRIANGLE }, { "sq", PAD_SQUARE }, { "up", PAD_UP }, { "down", PAD_DOWN },
		{ "left", PAD_LEFT }, { "right", PAD_RIGHT }, { "l1", PAD_L1 }, { "r1", PAD_R1 },
		{ "l2", PAD_L2 }, { "r2", PAD_R2 },
	};
	FILE *f = fopen(path, "r");
	char tok[64];
	if (!f) return;
	while (npress < MAX_PRESS && fscanf(f, "%63s", tok) == 1) {
		unsigned at, len = 6;
		char btn[16];
		if (!strcmp(tok, "--press")) continue;
		if (!strcmp(tok, "--stop") && fscanf(f, "%u", &stop_at) == 1) continue;
		if (sscanf(tok, "%u:%15[a-z0-9]:%u", &at, btn, &len) < 2) continue;
		for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
			if (!strcmp(btn, names[k].n)) {
				press[npress].at = at;
				press[npress].len = len;
				press[npress].bit = names[k].b;
				npress++;
			}
	}
	fclose(f);
}

static u16 script_pad(u32 frame)
{
	u16 b = 0xffff;
	for (int i = 0; i < npress; i++)
		if (frame >= press[i].at && frame < press[i].at + press[i].len) b &= ~press[i].bit;
	return b;
}

#ifdef PSX_SELFTEST
// Pruebas en melonDS: Memory Expansion Pak (8 MB en la ranura de GBA, solo admite escrituras de 16 bits)
#define PAK ((u8 *)0x09000000)

static int pak_init(void)
{
	if (!gbacartOpen()) return 0;   // sin esto, tocar la ranura da un fallo de acceso
	gbacartSetTiming(GBA_WAIT_ROM_MASK, GBA_WAIT_ROM_N_6 | GBA_WAIT_ROM_S_4);
	*(vu16 *)0x08240000 = 1;   // desbloquea la RAM
	*(vu16 *)PAK = 0x5aa5;
	return *(vu16 *)PAK == 0x5aa5;
}

static int is_pak(const void *p) { return (uintptr_t)p >= 0x08000000 && (uintptr_t)p < 0x0a000000; }

// fread que sirve también para el Expansion Pak (copia en medias palabras)
static int read_any(FILE *f, void *dst, u32 len)
{
	static u16 tmp[2048];
	if (!is_pak(dst)) return fread(dst, 1, len, f) == len;
	for (u32 o = 0; o < len; o += sizeof tmp) {
		u32 n = len - o < sizeof tmp ? len - o : sizeof tmp;
		if (fread(tmp, 1, n, f) != n) return 0;
		for (u32 i = 0; i < (n + 1) / 2; i++) ((vu16 *)((u8 *)dst + o))[i] = tmp[i];
	}
	return 1;
}

static int load_bios(void)
{
	FILE *f = fopen("nitro:/bios.bin", "rb");
	int ok;
	if (!f) return 0;
	psx_bios = PAK;
	spu_mem = (u16 *)(PAK + 512 * 1024);
#ifdef BENCH_VRAM_PAK
	psx_vram = (u16 *)(PAK + 1024 * 1024);   // 1 MB más para la caché de código: se parece más a la DSi (3 MB de caché)
#endif
	ok = read_any(f, psx_bios, 512 * 1024);
	fclose(f);
	return ok;
}

static int state_read(void *u, void *p, u32 len) { return read_any(u, p, len); }
#else
static int state_read(void *u, void *p, u32 len) { return fread(p, 1, len, u) == len; }
#endif
static int state_write(void *u, void *p, u32 len) { return fwrite(p, 1, len, u) == len; }

static u16 map_pad(u32 k)
{
	u16 b = 0xffff;
	// misma posición física que el mando de PS1: A=círculo, B=cruz, X=triángulo, Y=cuadrado
	if (k & KEY_A) b &= ~PAD_CIRCLE;
	if (k & KEY_B) b &= ~PAD_CROSS;
	if (k & KEY_X) b &= ~PAD_TRIANGLE;
	if (k & KEY_Y) b &= ~PAD_SQUARE;
	if (k & KEY_START) b &= ~PAD_START;
	if (k & KEY_UP) b &= ~PAD_UP;
	if (k & KEY_DOWN) b &= ~PAD_DOWN;
	if (k & KEY_LEFT) b &= ~PAD_LEFT;
	if (k & KEY_RIGHT) b &= ~PAD_RIGHT;
	if (k & KEY_SELECT) {   // SELECT + L/R = L2/R2; SELECT solo = SELECT
		if (k & KEY_L) b &= ~PAD_L2;
		else if (k & KEY_R) b &= ~PAD_R2;
		else b &= ~PAD_SELECT;
	} else {
		if (k & KEY_L) b &= ~PAD_L1;
		if (k & KEY_R) b &= ~PAD_R1;
	}
	return b;
}

// Doble búfer: dos mapas de bits de 256x256 (bancos A y B). Se dibuja en el oculto y el cambio
// se hace en la vblank, así la DS nunca muestra una imagen a medio copiar.
static u16 *fb[2];
static int back = 1;
static volatile int flip_to = -1;

static void flip_in_vblank(void)
{
	if (flip_to >= 0) {
		REG_BG3CNT = (REG_BG3CNT & ~(0x1f << 8)) | ((flip_to ? 8 : 0) << 8);   // base del mapa en bloques de 16 KB
		flip_to = -1;
	}
}

// Copia la zona visible de la VRAM de la PS1 escalándola a 256x192, de dos en dos píxeles
static void present(void)
{
	static u16 xmap[256], ymap[192];
	static int last_w, last_h, last_even;
	int w = gpu_disp.width, h = gpu_disp.height, even = opt_quality >= 2 && !gpu_disp.color24;
	u32 *dst32;

	if (gpu_disp.stale && !gpu_disp.disabled) return;   // el juego enseña un frame que no se dibujó: dejar el anterior
	if (single_fb) {   // en 24 bits (vídeos) no hay 3D: fuera la capa, que no tape el fondo
		if (gpu_disp.color24) REG_DISPCNT &= ~DISPLAY_BG0_ACTIVE;
		else REG_DISPCNT |= DISPLAY_BG0_ACTIVE;
	}
	if (single_fb && !gpu_disp.disabled) {   // GPU 3D: el fondo no se ve o no ha cambiado
		static int last24 = -1;
		if (!gpu_disp.color24) { last24 = 0; if (gx_bg_hidden || !gx_need_copy()) return; }
		else {   // vídeos (24 bits): solo si llegó imagen nueva a la zona mostrada (o se acaba de cambiar de modo)
			int need = gx_need_copy();
			if (!need && last24 == 1) return;
			last24 = 1;
		}
	}
	while (flip_to >= 0) { }   // el cambio anterior aún no se ha hecho
	if (single_fb) back = 0;
	dst32 = (u32 *)fb[back];
	if (gpu_disp.disabled) {
		dmaFillWords(0x80008000, dst32, 256 * 192 * 2);
	} else {
		if (w != last_w || h != last_h || even != last_even) {
			for (int x = 0; x < 256; x++) xmap[x] = (u16)(x * w / 256);
			for (int y = 0; y < 192; y++) ymap[y] = (u16)((y * h / 192) & (even ? ~1 : ~0));   // igual que gpu_rows_for
			last_w = w;
			last_h = h;
			last_even = even;
		}
		for (int y = 0; y < 192; y++, dst32 += 128) {
			int sy = (gpu_disp.disp_y + ymap[y]) & 511;
			if (gpu_disp.color24) {
				const u8 *line = (const u8 *)&psx_vram[sy * 1024];
				int bx0 = gpu_disp.disp_x * 2;
				for (int x = 0; x < 256; x += 2) {
					const u8 *p = line + ((bx0 + xmap[x] * 3) & 2047), *q = line + ((bx0 + xmap[x + 1] * 3) & 2047);
					u32 a = (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10);
					u32 b = (q[0] >> 3) | ((q[1] >> 3) << 5) | ((q[2] >> 3) << 10);
					dst32[x >> 1] = a | (b << 16) | 0x80008000;
				}
			} else {
				const u16 *src = &psx_vram[sy * 1024];
				int sx = gpu_disp.disp_x;
				for (int x = 0; x < 256; x += 2)
					dst32[x >> 1] = src[(sx + xmap[x]) & 1023] | ((u32)src[(sx + xmap[x + 1]) & 1023] << 16) | 0x80008000;
			}
		}
	}
	flip_to = back;
	if (!single_fb) back ^= 1;
}

// Reloj de 32 bits a 33,51 MHz con los temporizadores 0 y 1 encadenados (calico no los usa: sus ticks van
// en el 2 y el 3). cpuGetTiming pasa por los ticks de calico y cuesta mucho más: se llama miles de veces.
static void fast_timer_start(void)
{
	TIMER_CR(0) = 0;
	TIMER_CR(1) = 0;
	TIMER_DATA(0) = 0;
	TIMER_DATA(1) = 0;
	TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
	TIMER_CR(0) = TIMER_ENABLE | ClockDivider_1;
}

u32 fast_ticks(void)
{
	u32 hi, lo;
	do {
		hi = TIMER_DATA(1);
		lo = TIMER_DATA(0);
	} while (hi != TIMER_DATA(1));
	return (hi << 16) | lo;
}

#ifdef PSX_PROFILE
u32 psx_prof[PROF_N];
u32 psx_prof_ticks(void) { return fast_ticks(); }
#endif

// Totales desde el arranque para el banco de pruebas del PC (tools/dsbench los busca en la RAM por la marca)
struct {
	char magic[8];
	u32 frames, vblanks, pc;
	u8 *ram;   // la RAM de la PS1, para volcarla desde el PC
	u32 code, code_used;   // caché del dynarec (dirección y bytes usados), para desensamblarla desde el PC
	u32 prof[PROF_N];
	PsxStats stat;
} bench = { "SPBENCH" };
#ifdef PSX_SMCLOG
u32 psx_frame_no;
#endif

static void bench_second(void)
{
	for (int i = 0; i < PROF_N; i++) bench.prof[i] += psx_prof[i];
	for (u32 i = 0; i < sizeof psx_stat / 4; i++) ((u32 *)&bench.stat)[i] += ((u32 *)&psx_stat)[i];
}

#ifdef PSX_SELFTEST
// Microprueba para saber cómo cuenta melonDS los accesos a la RAM principal (¿simula las cachés?)
static u32 mb_ram[1024];
static u32 mb_dtcm[1024] __attribute__((section(".dtcm")));

static __attribute__((noinline)) u32 mb_sum(const u32 *p, int reps)
{
	u32 s = 0;
	for (int r = 0; r < reps; r++)
		for (int i = 0; i < 1024; i += 4) s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
	return s;
}

static __attribute__((noinline, section(".itcm"))) u32 mb_sum_itcm(const u32 *p, int reps)
{
	u32 s = 0;
	for (int r = 0; r < reps; r++)
		for (int i = 0; i < 1024; i += 4) s += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
	return s;
}

static void microbench(void)
{
	u32 t[4], s = 0;
	cpuStartTiming(0);
	t[0] = cpuGetTiming(); s += mb_sum(mb_ram, 20);
	t[1] = cpuGetTiming(); s += mb_sum(mb_dtcm, 20);
	t[2] = cpuGetTiming(); s += mb_sum_itcm(mb_ram, 20);
	t[3] = cpuGetTiming(); s += mb_sum_itcm(mb_dtcm, 20);
	{
		u32 t4 = cpuGetTiming();
		printf("MB ram %lu dtcm %lu itcm+ram %lu itcm+dtcm %lu (%lu)\n", t[1] - t[0], t[2] - t[1], t[3] - t[2], t4 - t[3], s);
		printf("MPU dc %02lx ic %02lx wb %02lx dp %08lx\n", armMpuGetDCacheConfig(), armMpuGetICacheConfig(), armMpuGetWrBufConfig(),
		       armMpuGetDataPerm());
		printf("r0 %08lx r1 %08lx\nr2 %08lx r3 %08lx\nr4 %08lx r5 %08lx\nr6 %08lx r7 %08lx\n", armMpuGetRegion0(), armMpuGetRegion1(),
		       armMpuGetRegion2(), armMpuGetRegion3(), armMpuGetRegion4(), armMpuGetRegion5(), armMpuGetRegion6(), armMpuGetRegion7());
	}
	cpuEndTiming();
}
#endif

// Menú de pausa (tocar MENU en la pantalla de abajo): estado guardado junto al juego (.sst, ~3,7 MB). Desde aquí
// también se cambian el salto de imágenes y la CPU en marcha: se aplican al momento y quedan en los ajustes del juego.
static char state_path[256];
static char game_cfg[256];   // ajustes propios del juego en marcha (Silent Hill.cue -> Silent Hill.cfg)

static void wait_release(void) { do { swiWaitForVBlank(); scanKeys(); } while (keysHeld()); }

static void pause_menu(void)
{
	static int has_state = -1;
	snd_pause(1);
	wait_release();
	if (has_state < 0) { FILE *f = fopen(state_path, "rb"); has_state = f != NULL; if (f) fclose(f); }
	for (;;) {
		int k;
		consoleClear();
		printf("\x1b[36;1mSparStation - PAUSA\x1b[37;1m\n\n");
		printf("A      seguir jugando\n\n");
		printf("X      guardar estado\n\n");
		printf("Y      cargar estado%s\n\n", has_state ? "" : " (no hay)");
		printf("SELECT saltar frames: %d\n", opt_skip);
		printf("R      CPU PS1: %s\n\n", cpu_name());
		printf("\x1b[37mEl estado guarda el momento\nexacto (en la SD, junto al\njuego). Las partidas del juego\nvan a la tarjeta de memoria\n(.mcr), que se guarda sola.\x1b[37;1m\n");
		fflush(stdout);
		do { swiWaitForVBlank(); scanKeys(); k = keysDown(); } while (!k);
		if (k & (KEY_A | KEY_B | KEY_START)) break;
		if (k & (KEY_SELECT | KEY_R)) {   // al momento, y apuntado para este juego
			if (k & KEY_SELECT) opt_skip = (opt_skip + 1) % 4;
			if (k & KEY_R) opt_cpu = (opt_cpu + 1) % CPU_OPTS;
			apply_cfg();
			if (game_cfg[0]) cfg_write(game_cfg);
			continue;
		}
		if (k & KEY_X) {
			FILE *f = fopen(state_path, "wb");
			int ok;
			printf("\x1b[20;0HGuardando...");
			fflush(stdout);
			if (f) setvbuf(f, NULL, _IOFBF, 64 * 1024);
			ok = f && psx_state_save(state_write, f);
			if (f && fclose(f)) ok = 0;
			if (ok) has_state = 1;
			printf("\x1b[20;0H%s", ok ? "Estado guardado.   " : "ERROR al guardar.  ");
			fflush(stdout);
			for (int i = 0; i < 60; i++) swiWaitForVBlank();
			if (ok) break;
		}
		if ((k & KEY_Y) && has_state) {
			FILE *f = fopen(state_path, "rb");
			int ok;
			printf("\x1b[20;0HCargando...");
			fflush(stdout);
			if (f) setvbuf(f, NULL, _IOFBF, 64 * 1024);
			ok = f && psx_state_load(state_read, f);
			if (f) fclose(f);
			if (!ok) {   // a medias: la máquina queda inconsistente, mejor empezar de cero
				psx_reset();
				printf("\x1b[20;0HERROR: estado no valido.\nSe reinicia el juego.");
				fflush(stdout);
				for (int i = 0; i < 120; i++) swiWaitForVBlank();
			}
			if (gpu_hw) gpu_hw->vram_written(0, 0, 1024, 512);   // texturas del 3D: todas viejas
			break;
		}
	}
	wait_release();
	hud_redraw();
	snd_pause(0);
}

// title NULL: sin medidor (la autoprueba usa la pantalla de abajo para su texto)
static void run_loop(const char *title, int use_log)
{
	int frames = 0;
	u32 last = vblanks;
	fast_timer_start();
#if defined(SPAR_PROF) && defined(PSX_SELFTEST)
	prof_start("");   // melonDS: dsbench lee las muestras de la RAM (DSB_PROF)
#endif
	if (title) {
		char info[96], nw[24];
		if (nwram_ok) snprintf(nw, sizeof nw, "%s", opt_nwram ? "NWRAM" : "RAM");
		else snprintf(nw, sizeof nw, "RAM (sin NWRAM: %s)", nwram_why[0] ? nwram_why : "?");
		snprintf(info, sizeof info, "%s, salto %d, CPU %s, codigo %s%s", quality_name[opt_quality], opt_skip, cpu_name(), nw,
		         cheat_count() ? ", parches" : "");
		hud_start(title, use_log, info);
	}
#ifdef STACK_PROBE
	stack_paint();   // desde aquí: solo lo que gasta la emulación
#endif
	for (;;) {
		u32 t0 = fast_ticks();
		scanKeys();
		if (title && hud_input()) { pause_menu(); last = vblanks; frames = 0; }
		while (stop_at && bench.frames >= stop_at) swiWaitForVBlank();
		psx_pad_buttons = npress ? script_pad(bench.frames + 1) : map_pad(keysHeld());
		gx_dbg.stage = 1;
		psx_run_frame();
		frame_beat++;
#ifdef DYN_DUMP_PC
		{ extern u32 dyn_dbg_addr[2]; gx_dbg.dyn = dyn_dbg_addr[0]; gx_dbg.n = dyn_dbg_addr[1]; }
#endif
		card_frame();
		snd_xa_pump();
		gx_dbg.stage = 2;
		{ PROF_BEGIN(); present(); PROF_END(PROF_PRESENT); }
		gx_dbg.stage = 3;
		t0 = fast_ticks() - t0;
		psx_prof[PROF_FRAME] += t0;
		hud_frame(t0);
		frames++;
		{   // no ir más rápido que la consola: tiempo en quintos de vblank de la DS (un frame PAL dura 6)
			static u32 lim_base, lim_t;
			lim_t += gpu_disp.pal ? 6 : 5;
			if ((vblanks - lim_base) * 5 > lim_t + 10) lim_base = vblanks - lim_t / 5;   // atrasados: no recuperar a saltos
			while ((s32)(lim_t - (vblanks - lim_base) * 5) > 0) swiWaitForVBlank();
		}
		bench.frames++;
#ifdef GX_HASH
		{ extern u32 gx_hash; if (bench.frames == GX_HASH) gx_dbg.dyn = gx_hash; }
#endif
#ifdef PSX_SMCLOG
		{ extern u32 psx_frame_no; psx_frame_no = bench.frames; }
#endif
		bench.vblanks = vblanks;
		bench.pc = cpu.pc;
		bench.ram = psx_ram;
		{ u32 used, size; extern u32 *dyn_code_base(void); dyn_cache_use(&used, &size); bench.code = (u32)dyn_code_base(); bench.code_used = used; }
		if (vblanks - last >= 60) {   // un segundo real
			if (!opt_cpu && frames) {   // CPU automática: acercar el trabajo de cada frame al tiempo de un frame real
				u32 target = (gpu_disp.pal ? 33514000u / 50 : 33514000u / 60) * (u32)opt_target / 100;
				u32 want = (u32)((u64)auto_scale * (psx_prof[PROF_FRAME] / (u32)frames) / target);
				auto_scale = (auto_scale + want) / 2;
				if (auto_scale < AUTO_MIN) auto_scale = AUTO_MIN;
				if (auto_scale > auto_max) auto_scale = auto_max;
				dyn_set_cpu_scale(auto_scale);
			}
			bench_second();
#ifdef STACK_PROBE
			gx_dbg.dyn = stack_used() << 16 | stack_paint_depth;
#endif
			if (title) hud_second(frames, vblanks - last);
			else { memset(psx_prof, 0, sizeof psx_prof); memset(&psx_stat, 0, sizeof psx_stat); }
			frames = 0;
			last = vblanks;
		}
	}
}

int main(void)
{
	char game[256];
	int bg;

#ifdef STACK_PROBE
	stack_paint();
#endif
	defaultExceptionHandler();   // un fallo del ARM9 enseña los registros en vez de quedarse en negro
	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
	// consola de abajo en el banco H (32 KB): el C queda libre para texturas del 3D (gx.c)
	vramSetBankH(VRAM_H_SUB_BG);
	consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 15, 0, false, true);
	bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	fb[0] = bgGetGfxPtr(bg);
	fb[1] = fb[0] + 256 * 256;   // banco B, base de mapa 8
	dmaFillHalfWords(0x8000, fb[0], 2 * 256 * 256 * 2);

	printf("SparStation v35 - PS1 en DSi\nby Spar Retro\n\n");
#ifdef PSX_SELFTEST
	if (!nitroFSInit(NULL) || !pak_init() || !load_bios()) {
		printf("Pruebas: falta nitro:/bios.bin\no el Expansion Pak.\n");
		for (;;) swiWaitForVBlank();
	}
	microbench();
#else
	psx_bios = (u8 *)openbios_bin;   // la BIOS se usa desde la propia ROM, sin copiarla
#endif
	{
		int ok = psx_init();
#ifdef NO_SND
		spu_cache_size = 0;   // banco de pruebas sin sonido (EXTRA=-DNO_SND)
#else
		if (ok) snd_init(isDSiMode());   // la caché de sonido antes que la del dynarec, que se queda el resto
#endif
		if (!ok || !dyn_init(isDSiMode())) {
			printf("Sin memoria.\n");
			for (;;) swiWaitForVBlank();
		}
	}
	if (!isDSiMode()) {
		// En una DS normal (4 MB) no caben los juegos: solo la demo de la BIOS, que sirve para medir,
		// o un EXE de prueba si la ROM lleva nitro:/selftest.exe
		static u8 hdr[0x800];
#ifdef PSX_SELFTEST
		int nitro = 1;   // ya montado para leer la BIOS
#else
		int nitro = nitroFSInit(NULL);
#endif
		FILE *t = nitro ? fopen("nitro:/selftest.exe", "rb") : NULL;
		if (t && fread(hdr, 1, 0x800, t) == 0x800 && psx_sideload_exe_reader(hdr, read_exe, t)) {
			FILE *q = fopen("nitro:/quiet", "rb");   // sin salida de texto: el dynarec encadena como en los juegos
			printf("Modo DS: autoprueba\n");
			if (q) fclose(q);
			else psx_tty = tty_out;
		} else if (nitro && open_subset()) {
			printf("Modo DS: disco reducido\n");
			psx_fastboot = 1;
			if (load_cht("nitro:/disc.cht")) printf("Parches: si\n");
		} else printf("Modo DS: demo de la BIOS.\nLos juegos necesitan modo DSi.\n");
#ifdef DYN_HOT_TEST
		{   // EXTRA=-DDYN_HOT_TEST=KB: zona caliente en otro búfer, como con la NWRAM de la DSi (melonDS no la tiene en modo DS).
			// dsbench con DSB_MAGIC=SPHOTTST:8:archivo saca dónde quedó (0 = no hubo memoria) y la caché de código de antes
			// Sin memoria libre (las pruebas de instrucciones), la mitad final del búfer de código, y la fría la primera.
			static volatile struct { char magic[8]; u32 hot, old; } hot_dbg = { "SPHOTTST" };
			u32 kb = DYN_HOT_TEST, used, size;
			void *h = malloc(kb * 1024);
			hot_dbg.old = (u32)dyn_code_base();
			dyn_cache_use(&used, &size);
			if (!h) { kb = size / 2048; h = (u8 *)dyn_code_base() + kb * 1024; }
			dyn_hot_area(h, kb * 1024);
			hot_dbg.hot = (u32)h;
			printf("Zona caliente aparte: %s\n", h ? "si" : "NO");
		}
#endif
		psx_reset();
		if (nitro && !psx_tty) {   // banco de pruebas: empezar desde un estado guardado, con guion de botones
			FILE *sf = fopen("nitro:/state.sst", "rb");
			if (sf) {
				printf(psx_state_load(state_read, sf) ? "Estado cargado\n" : "Estado: ERROR\n");
				fclose(sf);
			}
			load_script("nitro:/press.txt");
#ifdef BENCH_QUALITY
			opt_quality = BENCH_QUALITY;   // make SELFTEST=1 EXTRA=-DBENCH_QUALITY=n: medir con esa calidad
#ifdef BENCH_HW
			opt_hw = BENCH_HW;   // EXTRA=-DBENCH_HW=0: GPU por software
#endif
#ifdef BENCH_SKIP
			opt_skip = BENCH_SKIP;   // y saltando frames (EXTRA=-DBENCH_SKIP=n)
#endif
#ifdef BENCH_CPU
			opt_cpu = BENCH_CPU;
#endif
			apply_cfg();
#ifdef BENCH_PCT
			dyn_set_cpu_scale(256u * 100 / BENCH_PCT);   // EXTRA=-DBENCH_PCT=n: CPU fija al n% (cualquier valor)
#endif
#endif
		}
		irqSet(IRQ_VBLANK, on_vblank);
		run_loop(psx_tty ? NULL : "modo DS (prueba)", 0);
	}
	setCpuClock(true);
	if (!fatInitDefault()) {
		printf("No puedo leer la tarjeta SD.\n");
		for (;;) swiWaitForVBlank();
	}
	nwram_ok = nwram_init();
	if (!pick_game(game, sizeof game)) {
		printf("No hay juegos en %s\n(.cue + .bin)\n", GAMES_DIR);
		for (;;) swiWaitForVBlank();
	}
	if (nwram_ok && opt_nwram) dyn_hot_area((void *)NWRAM_BASE, NWRAM_SIZE);   // aún no se ha traducido nada
	if (!open_disc(game)) {
		printf("No puedo abrir\n%s\n", game);
		for (;;) swiWaitForVBlank();
	}
	card_open(game);
	snd_enable(opt_snd);
	snprintf(state_path, sizeof state_path, "%.*s.sst", (int)(strrchr(card_path, '.') - card_path), card_path);
	snprintf(game_cfg, sizeof game_cfg, "%.*s.cfg", (int)(strrchr(card_path, '.') - card_path), card_path);
	{   // parches GameShark del juego (Juego.cht junto al .cue), si los hay
		char cht[256];
		snprintf(cht, sizeof cht, "%.*s.cht", (int)(strrchr(card_path, '.') - card_path), card_path);
		load_cht(cht);
	}
	psx_fastboot = 1;   // sin la animación de la BIOS (unos 25 s en la DSi)
	psx_reset();
	apply_cfg();
	irqSet(IRQ_VBLANK, on_vblank);
	run_loop(strrchr(game, '/') + 1, 1);
	return 0;
}
