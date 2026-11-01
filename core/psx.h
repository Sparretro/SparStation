// SparStation: emulador de PlayStation pensado para la Nintendo DSi.
// Núcleo en C portable: el mismo código corre en el banco de pruebas del PC y en la DSi.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define PSX_CPU_HZ 33868800u
#define PSX_LINES 263
#define PSX_LINE_CYCLES 2146
#define PSX_VBLANK_LINE 240

// Datos que se tocan en cada instrucción: en la DS van a la DTCM (memoria interna, sin fallos de caché)
#ifdef ARM9
#define PSX_FAST __attribute__((section(".dtcm")))
#define PSX_ITCM __attribute__((section(".itcm"), noinline))   // código caliente suelto en la ITCM
#else
#define PSX_FAST
#define PSX_ITCM
#endif

// Los ciclos son absolutos y dan la vuelta: se comparan siempre por diferencia con signo.
#define TIME_BEFORE(a, b) ((s32)((a) - (b)) < 0)

// ---- memoria ----
extern u8 *psx_ram;      // 2 MB
extern u8 *psx_bios;     // 512 KB
extern u8 psx_scratch[1024];
extern u32 psx_cycles;
extern u32 psx_run_until;   // hasta dónde corre la CPU ahora; los dispositivos lo adelantan
// Un dispositivo programó algo para «when»: la CPU debe pararse ahí para atenderlo a tiempo
extern u32 psx_sched_req;   // la petición más temprana (cpu_interp_step la recoge: dentro de un paso el plazo es otro)
static inline void psx_schedule(u32 when)
{
	if ((s32)(when - psx_run_until) < 0) psx_run_until = when;
	if ((s32)(when - psx_sched_req) < 0) psx_sched_req = when;
}

u32 bus_read32(u32 a);
u32 bus_read16(u32 a);
u32 bus_read8(u32 a);
void bus_write32(u32 a, u32 v);
void bus_write16(u32 a, u32 v);
void bus_write8(u32 a, u32 v);

// ---- interrupciones ----
enum { PSX_IRQ_VBLANK, PSX_IRQ_GPU, PSX_IRQ_CDROM, PSX_IRQ_DMA, PSX_IRQ_TMR0, PSX_IRQ_TMR1, PSX_IRQ_TMR2, PSX_IRQ_SIO0, PSX_IRQ_SIO, PSX_IRQ_SPU, PSX_IRQ_PIO };
extern u32 irq_stat, irq_mask;
void psx_irq(int n);
void irq_update(void);

// ---- CPU ----
typedef struct {
	u32 r[32];
	u32 pc, next_pc, cur_pc, hi, lo;
	u32 cop0[32];
	u32 lr, lv;        // carga retrasada pendiente (se aplica tras la instrucción siguiente)
	int branching;     // la instrucción anterior fue un salto: la actual está en su delay slot
	int in_delay;
} Cpu;
// Registros de la GTE (gte.c los usa por su nombre). Van delante de cpu, en el mismo bloque de la DTCM: el
// dynarec los alcanza desde r11 (&cpu) con desplazamientos cortos (las medias palabras admiten hasta 255).
// Mismos tipos y tamaños de siempre: los estados guardados no cambian.
typedef struct {
	u8 RGBC[4] __attribute__((aligned(4)));
	u8 CF[3][4];   // FIFO de colores RGB0..RGB2
	u32 RES1, LZCS, LZCR, FLAG;
	s32 MAC[4], TR[3], BK[3], FC[3], OFX, OFY, DQB;
	s16 V[3][3], IR[4], SX[3], SY[3], RT[3][3], LM[3][3], LC[3][3], DQA, ZSF3, ZSF4;
	u16 OTZ, SZ[4], H;
} GteRegs;
typedef struct { GteRegs gte; Cpu cpu_; } PsxCore;
extern PsxCore psx_core;
#define cpu (psx_core.cpu_)
extern u32 psx_hook_pc;   // si no es 0, psx_hook() se llama cuando el PC llega aquí
void psx_hook(void);
void cpu_reset(void);
void cpu_run(u32 until);

// ---- dynarec (solo en la DS: traduce MIPS a ARM) ----
int cpu_dyn_interp(u32 op, u32 pc);
void cpu_dyn_exception(u32 code, u32 pc);
int cpu_dyn_irq(void);
void cpu_interp_step(void);
#ifdef PSX_DYNAREC
#define DYN_PAGE_BITS 10                              // trozos de 1 KB: una escritura de datos cerca del código borra poco
#define DYN_NPAGES (0x200000 >> DYN_PAGE_BITS)
extern u8 dyn_pages[DYN_NPAGES];   // trozos de RAM con código traducido
void dyn_smc(u32 ram_off);         // la RAM cambió en una página con código: invalidarla
void dyn_ram_written(u32 ram_off, u32 len);
int dyn_init(int big);
void dyn_set_cpu_scale(u32 scale);   // cambia psx_cpu_scale en marcha (sin volver a traducir)
void dyn_run(u32 until);
void dyn_cache_use(u32 *used, u32 *size);   // bytes de la caché de código ocupados / total
void dyn_hot_area(void *p, u32 size);   // zona caliente de la caché de código en otra memoria (NWRAM de la DSi)
u32 *dyn_code_base(void);               // principio de la zona caliente
void dyn_regions(u32 r[8]);   // zonas de la caché de código (para el perfilador)
u32 dyn_block_of(u32 arm_pc);   // PC MIPS del bloque traducido que contiene esa dirección (0 = ninguno)
#define DYN_CHECK_WRITE(off) do { if (dyn_pages[((off) & 0x1fffff) >> DYN_PAGE_BITS]) dyn_smc((off) & 0x1fffff); } while (0)
#else
#define DYN_CHECK_WRITE(off) do { } while (0)
#endif

// ---- GTE ----
u32 gte_read(int reg);
void gte_write(int reg, u32 v);
void gte_command(u32 op);
void gte_reset(void);                 // tablas (antes de ejecutar nada)
void (*gte_entry(u32 op))(u32);       // función directa para esa operación (para el dynarec)
// Para el dynarec: dónde vive el registro reg (0-63) si leerlo o escribirlo es solo copiar memoria.
// Hasta dos trozos: el primero con los 16 o 32 bits bajos, el segundo con los 16 altos. 0 = no es simple.
typedef struct { void *addr; u8 bits, sign; } GteRegPart;
int gte_reg_parts(int reg, int write, GteRegPart part[2]);

// ---- GPU ----
extern u16 *psx_vram;    // 1024x512, 15 bits por píxel
typedef struct {
	int disp_x, disp_y;       // esquina del framebuffer visible en VRAM
	int width, height;        // resolución nominal
	int color24, disabled, interlace, pal;
	int frame_done;
	int stale;                // lo que se muestra es un frame saltado: el frontend puede dejar la imagen anterior
} GpuDisplay;
extern GpuDisplay gpu_disp;
// ajustes de velocidad (los pone el frontend)
extern u8 gpu_frameskip;       // 0 = dibujar todo; N = de cada N+1 frames del juego, dibujar 1
extern u8 gpu_half_x;          // triángulos a media resolución horizontal (un píxel calculado por par)

// Dibujo por hardware (opcional, lo pone el frontend): los polígonos y sprites que van a la pantalla se
// le pasan en vez de dibujarlos en la VRAM. Lo que se dibuja fuera (texturas hechas por el juego) sigue
// por software.
typedef struct { s16 x, y, u, v; u8 r, g, b; } GpuHwVtx;   // x, y absolutos en la VRAM (con el desplazamiento); u, v pueden pasar de 255 (sprites)
typedef struct {
	u8 tex;                  // 0 = sin textura; 1/2/3 = 4/8/15 bits
	u8 raw, gouraud, semi;   // raw: textura sin modular; semi: 0 = opaco, si no modo de mezcla + 1
	u8 mask;                 // no pinta sobre píxeles con el bit de máscara (y lo pone): cada píxel una sola vez
	s16 tpx, tpy, cx, cy;    // página de textura y paleta en la VRAM
	s16 ox, oy;              // origen del área de dibujo (esquina del buffer)
} GpuHwPoly;
// Polígono tal cual viene en el paquete de la DMA (comando 20h-3Fh entero en w): lo que no está en sus palabras
typedef struct {
	u32 cmd;
	s16 dx, dy;              // a sumar a x, y del paquete: desplazamiento de dibujo menos el origen del buffer
	u8 tex, semi, mask;      // como en GpuHwPoly
	s16 tpx, tpy, cx, cy;
} GpuHwRaw;
typedef struct {
	void (*poly)(const GpuHwVtx *v, int n, const GpuHwPoly *p);   // n = 3 o 4 (cuadrado: 0,1,2 y 1,2,3)
	void (*vram_written)(int x, int y, int w, int h);              // para invalidar texturas y la imagen
	void (*flip)(int drawn);   // el juego cambia el buffer mostrado; drawn = 0 si ese frame se saltó
	void (*clear)(u16 color);  // relleno de casi todo un buffer de pantalla: en vez de escribir la VRAM
	void (*poly_raw)(const u32 *w, const GpuHwRaw *r);   // opcional: el polígono sin pasar por GpuHwVtx (más rápido)
} GpuHw;
extern const GpuHw *gpu_hw;
extern u8 gpu_rows_on;         // 1 = dibujar solo las filas marcadas en gpu_row_draw
extern u8 gpu_row_draw[512];   // por fila de la VRAM (lo rellena gpu_rows_for)
extern u16 psx_cpu_scale;      // coste de cada ciclo en 1/256: 256 = CPU al 100%, más = CPU más lenta
void gpu_rows_for(int mode, int out_lines);   // rellena gpu_row_draw según la escala de la pantalla de destino
void gpu_reset(void);
void gpu_write_gp0(u32 v);
void gpu_write_gp0_n(const u32 *w, u32 n);   // n palabras seguidas (paquete de la DMA)
void gpu_write_gp1(u32 v);
u32 gpu_read(void);
u32 gpu_stat(void);
void gpu_vblank(void);
void gpu_set_line(int line);

// ---- DMA ----
u32 dma_read(u32 off);
void dma_write(u32 off, u32 v);

// ---- timers ----
u32 timer_read(u32 off);
void timer_write(u32 off, u32 v);
void timers_sync(void);              // pone los contadores al día con psx_cycles
int timers_next_event(u32 *when);    // próxima interrupción posible de un temporizador
void timers_line_origin(u32 c);      // ciclo en que empieza el frame (las líneas para el modo hblank)
void timers_reset(void);

// ---- CD-ROM ----
typedef int (*psx_sector_reader)(void *user, u32 lba, u8 *out2352);
void cdrom_reset(void);
void cdrom_set_disc(psx_sector_reader rd, void *user, u32 sectors);
u32 cdrom_read(u32 reg);
void cdrom_write(u32 reg, u32 v);
u32 cdrom_dma_word(void);
u32 cdrom_dma_words(u32 *dst, u32 n);   // la DMA del CD, de golpe
int cdrom_next_event(u32 *when);
void cdrom_tick(void);
// Audio XA: los sectores de audio que el juego manda a la SPU se le pasan al frontend tal cual (2352 bytes)
typedef struct {
	void (*sector)(u32 lba, const u8 *raw);   // sector de audio que pasa el filtro, en el momento de leerlo
	void (*stop)(void);                       // el juego para el CD o salta a otro sitio
	void (*mute)(int on);
	void (*cdda)(u32 lba, u32 end);           // audio de CD (pistas de audio): suena desde lba hasta end (sin incluir)
} CdAudioHw;
// Pistas del disco (las del .cue). start: LBA del INDEX 01 desde el principio del disco; pre: el del INDEX 00
// (donde empieza su pausa previa; = start si no tiene). Sin pistas, el disco es una sola de datos.
typedef struct { u8 audio; u32 start, pre; } PsxTrack;
void cdrom_set_tracks(const PsxTrack *t, int n);
// .cue con varias pistas (cue.c): archivos seguidos en el disco y las pistas que contienen
#define CUE_MAX_FILES 100
typedef struct { char path[256]; u32 start, sectors; } CueFile;
typedef struct { CueFile file[CUE_MAX_FILES]; int nfile; PsxTrack trk[99]; int ntrk; u32 total; } CueDisc;
int cue_parse(const char *cue_path, const char *dir, CueDisc *d, u32 (*file_sectors)(const char *path));
int cue_locate(const CueDisc *d, u32 lba, u32 *sector_in_file);
extern const CdAudioHw *cd_audio_hw;
typedef struct { s32 old[2], older[2]; } XaDecoder;   // historia del filtro por canal (se pone a cero al empezar)
// Descodifica un sector XA (4032 muestras mono o 2016 pares L/R intercalados en out); devuelve muestras por
// canal (0 si no es de audio)
int xa_decode(const u8 *raw, s16 *out, XaDecoder *d, int *stereo, int *rate);

// ---- MDEC (vídeo) ----
void mdec_reset(void);
void mdec_write_cmd(u32 v);
void mdec_write_words(const u32 *w, u32 n);   // la DMA de entrada, de golpe
void mdec_write_ctrl(u32 v);
u32 mdec_read_data(void);
u32 mdec_read_words(u32 *dst, u32 n);   // salida de golpe (palabras enteras); devuelve cuántas
u32 mdec_status(void);
int mdec_out_available(void);
void dma_mdec_kick(void);   // el MDEC tiene datos nuevos: reanuda el DMA de salida

// ---- mandos (SIO0) ----
extern u16 psx_pad_buttons;   // activo en bajo, formato de la PS1

// ---- parches GameShark (core/cheat.c): se aplican en cada vblank ----
void cheat_clear(void);
int cheat_parse(const char *text);   // añade los códigos de un texto (.cht); devuelve cuántos
int cheat_count(void);
void cheat_apply(void);
extern u8 *psx_memcard;           // tarjeta de memoria de la ranura 1 (128 KB) o NULL
extern u32 psx_memcard_writes;    // sectores escritos desde el arranque (el frontend guarda al ver cambios)
void sio_memcard_reset(void);     // tarjeta recién puesta (bandera de «nueva»)
void psx_memcard_format(u8 *card);   // tarjeta vacía formateada
void sio_reset(void);
u32 sio_read(u32 off, int size);
void sio_write(u32 off, u32 v, int size);
int sio_next_event(u32 *when);
void sio_tick(void);

// ---- SPU ----
extern u16 *spu_mem;
void spu_reset(void);
u32 spu_read16(u32 off);
void spu_write16(u32 off, u32 v);
void spu_dma_write(u32 v);
u32 spu_dma_read(void);
void spu_advance(u32 cycles);   // avanza envolventes y posiciones de las voces (lo llama psx_run_frame)
// Salida: cada voz de la PS1 la toca un canal del frontend con la muestra ya en PCM de 16 bits.
// pitch: 0x1000 = 44100 Hz; volúmenes 0..0x7fff por lado; loop = muestra de la vuelta o -1 (una vez).
typedef struct {
	void (*key_on)(int v, const s16 *pcm, u32 len, s32 loop, u32 pitch);
	void (*update)(int v, u32 pitch, u32 l, u32 r);
	void (*stop)(int v);
	void (*mix)(u32 samples);   // opcional (PC): se han emulado tantas muestras a 44100 Hz
} SpuHw;
extern const SpuHw *spu_hw;
extern u8 *spu_cache_mem;    // caché de muestras en PCM (si es NULL, psx_reset la reserva)
extern u32 spu_cache_size;

// botones del mando (bit a 0 = pulsado)
enum {
	PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
	PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
	PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
	PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

// ---- perfil opcional: el frontend da psx_prof_ticks() si compila con PSX_PROFILE ----
#ifdef PSX_PROFILE
u32 psx_prof_ticks(void);
// tiempos anidados: GPU, MDEC y traducir ocurren dentro de CPU; el disco va fuera
enum { PROF_CPU, PROF_GPU, PROF_PRESENT, PROF_FRAME, PROF_DISC, PROF_MDEC, PROF_COMPILE, PROF_GTE, PROF_N };
extern u32 psx_prof[PROF_N];
#define PROF_BEGIN() u32 prof_t0 = psx_prof_ticks()
#define PROF_END(i) (psx_prof[i] += psx_prof_ticks() - prof_t0)
// contadores para el medidor de pruebas (el frontend los pone a cero cada segundo)
typedef struct {
	u32 gpu_cmds, gpu_tris, gpu_rects, gpu_lines, gpu_fills, gpu_copies;
	u32 vram_upload;   // píxeles que la CPU/DMA sube a la VRAM
	u32 gpu_pixels;    // píxeles que recorren triángulos y rectángulos
	u32 disp_flips;    // GP1(05) que cambia el área mostrada: frames que de verdad dibuja el juego
	u32 cd_sectors, mdec_blocks;
	u32 dyn_compiled, dyn_flushes, dyn_smc, dyn_interp_ops, dyn_interp_steps;
	u32 dyn_idle;      // bucles de espera saltados (se avanza el reloj en vez de dar vueltas)
	u32 dyn_mips, dyn_arm;   // instrucciones MIPS traducidas y palabras ARM generadas
	u32 gte_ops, io_calls;   // comandos de la GTE y accesos a memoria por el camino lento (E/S, scratchpad)
	u32 sio_bytes, sio_acks, sio_polls;   // mando: bytes enviados, ACK y lecturas completas (0x42)
	u32 gpu_px_kind[4];   // píxeles: triángulos por el bucle rápido / el resto, rectángulos rápido / el resto
	u32 disp_drawn;    // de los disp_flips, los que se dibujaron de verdad (sin los saltados por gpu_frameskip)
	u32 tex_bands;     // bandas de textura nuevas en la caché del 3D de la DS (gx.c)
	u32 fb_skips;      // polígonos sin dibujar: su textura es un búfer de pantalla que solo tiene el 3D (gpu.c, fb_tex)
} PsxStats;
extern PsxStats psx_stat;
#define STAT_ADD(f, n) (psx_stat.f += (n))
#else
#define PROF_BEGIN()
#define PROF_END(i)
#define STAT_ADD(f, n)
#endif

// ---- estados guardados: cada módulo pasa sus variables por psx_state_io en un orden fijo ----
typedef struct {
	int (*io)(void *user, void *p, u32 len);   // escribe o lee len bytes; 0 = error
	void *user;
	int load, ok;
} PsxState;
void psx_state_io(PsxState *s, void *p, u32 len);
#define STATE(s, v) psx_state_io(s, &(v), sizeof(v))
void bus_state(PsxState *s);
void cpu_state(PsxState *s);
void dma_state(PsxState *s);
void gpu_state(PsxState *s);
void gte_state(PsxState *s);
void mdec_state(PsxState *s);
void cdrom_state(PsxState *s);
void cdrom_state_ext(PsxState *s);   // lo añadido después (va al final del estado: los viejos no lo tienen)
void cdrom_ext_defaults(void);
void sio_state(PsxState *s);
void timers_state(PsxState *s);
void spu_state(PsxState *s);
// Guardar / cargar el estado entero (unos 3,6 MB). Al cargar, el disco tiene que ser el mismo.
int psx_state_save(int (*write)(void *user, void *p, u32 len), void *user);
int psx_state_load(int (*read)(void *user, void *p, u32 len), void *user);
#ifdef PSX_DYNAREC
void dyn_reset(void);   // olvida todo el código traducido
#endif

// ---- API del sistema ----
int psx_init(void);   // reserva RAM y VRAM; psx_bios (512 KB) lo pone el frontend
void psx_reset(void);
int psx_sideload_exe(const u8 *exe, u32 len);   // carga un PS-X EXE cuando la BIOS llega al shell
extern u8 psx_fastboot;   // 1: los juegos de CD arrancan sin la animación de la BIOS
int cdrom_boot_exe(u32 *lba, u32 *size);
int cdrom_read_data(u32 lba, u8 *out2048);
void cdrom_boot_state(void);
int psx_sideload_exe_reader(const u8 *header, int (*read)(void *user, u8 *dst, u32 len), void *user);
void psx_run_frame(void);
extern void (*psx_tty)(int c);   // salida de printf de la BIOS (puede ser NULL)
extern void (*psx_line_cb)(void);   // depuración: se llama al final de cada línea de vídeo
