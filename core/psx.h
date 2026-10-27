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
static inline void psx_schedule(u32 when) { if ((s32)(when - psx_run_until) < 0) psx_run_until = when; }

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
extern Cpu cpu;
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
void dyn_run(u32 until);
void dyn_cache_use(u32 *used, u32 *size);   // bytes de la caché de código ocupados / total
#define DYN_CHECK_WRITE(off) do { if (dyn_pages[((off) & 0x1fffff) >> DYN_PAGE_BITS]) dyn_smc((off) & 0x1fffff); } while (0)
#else
#define DYN_CHECK_WRITE(off) do { } while (0)
#endif

// ---- GTE ----
u32 gte_read(int reg);
void gte_write(int reg, u32 v);
void gte_command(u32 op);

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
extern u8 gpu_rows_on;         // 1 = dibujar solo las filas marcadas en gpu_row_draw
extern u8 gpu_row_draw[512];   // por fila de la VRAM (lo rellena gpu_rows_for)
extern u16 psx_cpu_scale;      // coste de cada ciclo en 1/256: 256 = CPU al 100%, más = CPU más lenta
void gpu_rows_for(int mode, int out_lines);   // rellena gpu_row_draw según la escala de la pantalla de destino
void gpu_reset(void);
void gpu_write_gp0(u32 v);
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
void timers_advance(u32 cycles);
void timers_hblank(void);
void timers_reset(void);

// ---- CD-ROM ----
typedef int (*psx_sector_reader)(void *user, u32 lba, u8 *out2352);
void cdrom_reset(void);
void cdrom_set_disc(psx_sector_reader rd, void *user, u32 sectors);
u32 cdrom_read(u32 reg);
void cdrom_write(u32 reg, u32 v);
u32 cdrom_dma_word(void);
int cdrom_next_event(u32 *when);
void cdrom_tick(void);

// ---- MDEC (vídeo) ----
void mdec_reset(void);
void mdec_write_cmd(u32 v);
void mdec_write_ctrl(u32 v);
u32 mdec_read_data(void);
u32 mdec_status(void);
int mdec_out_available(void);
void dma_mdec_kick(void);   // el MDEC tiene datos nuevos: reanuda el DMA de salida

// ---- mandos (SIO0) ----
extern u16 psx_pad_buttons;   // activo en bajo, formato de la PS1
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
int psx_sideload_exe_reader(const u8 *header, int (*read)(void *user, u8 *dst, u32 len), void *user);
void psx_run_frame(void);
extern void (*psx_tty)(int c);   // salida de printf de la BIOS (puede ser NULL)
extern void (*psx_line_cb)(void);   // depuración: se llama al final de cada línea de vídeo
