// Medidor de pruebas: tres páginas en la pantalla de abajo (tocar a la izquierda cambia de página,
// a la derecha pone una MARCA) y una línea por segundo en sd:/ps1/sparstation_log.txt para analizarla luego.
#include <nds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include "psx.h"
#include "hud.h"
#include "gx.h"

#define LOG_PATH "sd:/ps1/sparstation_log.txt"
#ifdef SPAR_PROF
#include "prof.h"
#endif
#define HIST 30          // segundos que enseña la gráfica
#define TICKS_MS 33514   // cpuGetTiming va a 33,51 MHz

// colores de la consola de libnds
#define C_W "\x1b[37;1m"
#define C_G "\x1b[32;1m"
#define C_Y "\x1b[33;1m"
#define C_R "\x1b[31;1m"
#define C_C "\x1b[36;1m"
#define C_D "\x1b[37m"

enum { T_CPU, T_GPU, T_MDEC, T_COMPILE, T_PRESENT, T_DISC, T_OTHER, T_N };
static const char *const t_name[T_N] = { "CPU PS1", "GPU", "MDEC", "traducir", "copia", "disco SD", "otros" };

// lo medido en el último segundo
static struct {
	int fps10, game10, drawn10, target, pct[T_N];   // drawn10: imágenes que se ven (sin las saltadas)
	u32 worst_ms, best_ms, log_ms;
	PsxStats st;
} s;

static char game_name[40], settings_str[64];
#ifndef HUD_PAGE
#define HUD_PAGE 0
#endif
static int page = HUD_PAGE, have_snapshot;
static u32 frame_min = ~0u, frame_max;
static u32 seconds, total_frames, marks, mark_shown, fps_sum10, fps_n;
static int fps_min10 = 1 << 30, fps_max10;
static u8 hist[HIST];
static FILE *logf;
static u32 log_lines;

// Escribe una fila entera (rellena con espacios hasta la columna 31 para borrar lo anterior)
static void row(int y, const char *fmt, ...)
{
	char buf[160];
	int vis = 0;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	for (const char *p = buf; *p; p++) {
		if (*p == 0x1b) { while (*p && *p != 'm') p++; if (!*p) break; continue; }
		vis++;
	}
	printf("\x1b[%d;0H%s" C_W, y, buf);
	while (vis++ < (y < 23 ? 32 : 31)) putchar(' ');   // en la última fila, 32 haría subir la pantalla
}

static const char *speed_color(int pct) { return pct >= 90 ? C_G : pct >= 50 ? C_Y : C_R; }

static void bar(char *out, int pct)
{
	int n = (pct + 5) / 10;
	if (n > 10) n = 10;
	strcpy(out, pct >= 40 ? C_R : pct >= 20 ? C_Y : C_G);
	out += strlen(out);
	for (int i = 0; i < 10; i++) {
		if (i == n) { strcpy(out, C_D); out += strlen(out); }
		*out++ = i < n ? '#' : '-';
	}
	*out = 0;
}

static void clock_str(char *out, u32 secs) { sprintf(out, "%02lu:%02lu", secs / 60, secs % 60); }

static void draw_buttons(void)
{
	row(23, C_C "< PAGINA %d/3  " C_Y "MENU" C_C "   MARCA >", page + 1);
}

static void draw_page0(void)
{
	char b[64];
	int speed = s.target ? s.fps10 * 10 / s.target : 0, worst = 0;
	row(0, C_C "SparStation  rendimiento");
	row(1, C_D "%.30s", game_name);
	row(2, "");
	row(3, "FPS PS1  %s%3d.%d" C_W " / %d   %s%d%%", speed_color(speed), s.fps10 / 10, s.fps10 % 10, s.target,
	    speed_color(speed), speed);
	if (s.st.disp_flips && s.drawn10 < s.game10)   // con salto de imágenes: las que se ven
		row(4, "FPS juego %3d.%d" C_D "  vistas %d.%d", s.game10 / 10, s.game10 % 10, s.drawn10 / 10, s.drawn10 % 10);
	else if (s.st.disp_flips) row(4, "FPS juego %3d.%d" C_D "  (todas vistas)", s.game10 / 10, s.game10 % 10);
	else row(4, "FPS juego   --" C_D "  (buffer fijo)");
	row(5, "frame peor %lums  mejor %lums", s.worst_ms, s.best_ms);
	row(6, "");
	row(7, C_C "Tiempo de la DS:");
	for (int i = 0; i < T_N; i++) {
		bar(b, s.pct[i]);
		row(8 + i, "%-9s%s" C_W " %3d%%", t_name[i], b, s.pct[i]);
		if (s.pct[i] > s.pct[worst]) worst = i;
	}
	row(15, "CPU PS1 emulada: %lu%%", 25600ul / psx_cpu_scale);
	row(16, "Freno principal: " C_Y "%s", t_name[worst]);
	row(18, C_D "%.31s", settings_str);
	row(17, C_D "%dx%d%s%s %s", gpu_disp.width, gpu_disp.height, gpu_disp.color24 ? " 24bit" : " 15bit",
	    gpu_disp.interlace ? " entrel." : "", gpu_disp.pal ? "PAL" : "NTSC");
}

static void draw_page1(void)
{
	u32 used, size;
	struct mallinfo mi = mallinfo();
	unsigned bat = getBatteryLevel();
	dyn_cache_use(&used, &size);
	row(0, C_C "SparStation  detalle (por seg.)");
	row(1, C_Y "GPU" C_W "  pixeles %lu mil", s.st.gpu_pixels / 1000);
	row(2, " comandos %lu  triang. %lu", s.st.gpu_cmds, s.st.gpu_tris);
	row(3, " rect %lu  lineas %lu  rell. %lu", s.st.gpu_rects, s.st.gpu_lines, s.st.gpu_fills);
	row(4, " VRAM %luKB copias %lu tex %lu", s.st.vram_upload * 2 / 1024, s.st.gpu_copies, s.st.tex_bands);
	row(5, " pantalla %dx%d en (%d,%d)", gpu_disp.width, gpu_disp.height, gpu_disp.disp_x, gpu_disp.disp_y);
	row(6, C_Y "CD" C_W " sectores %lu (%lu.%lux)", s.st.cd_sectors, s.st.cd_sectors / 75, s.st.cd_sectors * 10 / 75 % 10);
	row(7, C_Y "MDEC" C_W " macrobloques %lu", s.st.mdec_blocks);
	row(8, C_Y "DYNAREC");
	row(9, " bloques nuevos %lu", s.st.dyn_compiled);
	row(10, " instr. via interprete %lu", s.st.dyn_interp_ops);
	row(11, " pasos interp. %lu  esperas %lu", s.st.dyn_interp_steps, s.st.dyn_idle);
	row(12, " SMC %lu   vaciados %lu", s.st.dyn_smc, s.st.dyn_flushes);
	row(13, " cache codigo %lu%% de %lu KB", size ? used * 100 / size : 0, size / 1024);
	row(14, C_Y "DS");
	row(15, " memoria usada %d.%d MB", mi.uordblks / (1024 * 1024), mi.uordblks % (1024 * 1024) * 10 / (1024 * 1024));
	row(16, " bateria %u/15%s", PM_BATT_LEVEL(bat), (bat & PM_BATT_CHARGING) ? " (cargando)" : "");
	row(17, " escribir log %lums", s.log_ms);
	row(18, C_D " pc=%08lx", cpu.pc);
}

static void draw_page2(void)
{
	char t[16];
	int top = 10;
	for (int i = 0; i < HIST; i++) if (hist[i] > top) top = hist[i];
	top = (top + 4) / 5 * 5;
	clock_str(t, seconds);
	row(0, C_C "SparStation  sesion");
	row(1, "tiempo %s   frames %lu", t, total_frames);
	if (fps_n)
		row(2, "media %lu.%lu  min %d.%d  max %d.%d", fps_sum10 / fps_n / 10, fps_sum10 / fps_n % 10,
		    fps_min10 / 10, fps_min10 % 10, fps_max10 / 10, fps_max10 % 10);
	else row(2, "");
	row(3, "marcas %lu", marks);
	row(4, C_Y "FPS ultimos %d s (arriba = %d)", HIST, top);
	for (int r = 0; r < 8; r++) {
		char line[HIST + 1];
		int level = top * (8 - r) / 8;   // la fila se llena si el valor llega a su altura
		for (int i = 0; i < HIST; i++) line[i] = (hist[i] && hist[i] >= level - top / 16) ? '#' : (r == 7 ? '_' : ' ');
		line[HIST] = 0;
		row(5 + r, C_G "%s", line);
	}
	row(13, C_D "-%ds%*s", HIST, HIST - 3, "ahora");
	row(14, "");
	if (logf) {
		row(15, "log: " C_G "%lu lineas" C_W " en la SD", log_lines);
		row(16, C_D "sd:/ps1/sparstation_log.txt");
	} else {
		row(15, "log: " C_R "sin SD" C_W " (modo DS)");
		row(16, "");
	}
	row(17, C_D "A=O B=X X=Tri Y=Cuad L/R=L1/R1");
	row(18, C_D "SELECT+L/R = L2/R2");
	row(19, C_D "MARCA apunta el momento en log");
}

static void draw(void)
{
	if (!have_snapshot) return;
	switch (page) {
	case 0: draw_page0(); break;
	case 1: draw_page1(); break;
	default: draw_page2(); break;
	}
	for (int y = page == 0 ? 19 : page == 1 ? 19 : 20; y < 21; y++) row(y, "");
	if (mark_shown) {
		char t[16];
		clock_str(t, seconds);
		row(20, C_Y "MARCA %lu guardada (%s)", marks, t);
	}
	draw_buttons();
	fflush(stdout);   // la consola guarda el texto hasta un salto de línea: sin esto se ve a trozos
}

void hud_start(const char *game, int use_log, const char *settings)
{
	snprintf(game_name, sizeof game_name, "%s", game);
	snprintf(settings_str, sizeof settings_str, "%s", settings);
	consoleClear();
	row(0, C_C "SparStation  medidor");
	row(1, C_D "%.30s", game_name);
	row(3, "Midiendo el primer segundo...");
	row(5, C_D "A=O B=X X=Tri Y=Cuad L/R=L1/R1");
	row(6, C_D "SELECT+L/R = L2/R2");
	row(8, C_D "Toca abajo: izquierda = pagina,");
	row(9, C_D "derecha = MARCA en el log.");
	draw_buttons();
	fflush(stdout);
	if (!use_log) return;
	logf = fopen(LOG_PATH, "a");
	if (logf) {
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		u32 used, size;
		dyn_cache_use(&used, &size);
		fprintf(logf, "\n# SparStation  %04d-%02d-%02d %02d:%02d  juego=%s  cache=%luKB  %s\n", tm->tm_year + 1900, tm->tm_mon + 1,
		        tm->tm_mday, tm->tm_hour, tm->tm_min, game_name, size / 1024, settings_str);
		fprintf(logf, "t,fps,fps_juego,cpu%%,gpu%%,mdec%%,trad%%,copia%%,sd%%,otros%%,peor_ms,mejor_ms,cmds,tri,rect,"
		              "vram_kb,cd_sect,mdec_mb,bloques,interp,pasos,smc,vaciados,cache%%,res,pc,kpx,gte%%,esperas,es_lentas,arm_por_mips,3d_subdesb,3d_tarde,3d_lineas_min,cpu_ps1%%,fps_vistas,tex_nuevas,descartes_fb\n");
		fflush(logf);
		fsync(fileno(logf));
	}
#ifdef SPAR_PROF
	prof_start(game_name);
#endif
}

void hud_frame(u32 ticks)
{
	if (ticks < frame_min) frame_min = ticks;
	if (ticks > frame_max) frame_max = ticks;
}

static void log_mark(void)
{
	char t[16];
	marks++;
	mark_shown = 3;
	clock_str(t, seconds);
	if (logf) {
		fprintf(logf, "# MARCA %lu  t=%s (%lu s)\n", marks, t, seconds);
		fflush(logf);
		fsync(fileno(logf));
#ifdef SPAR_PROF
		prof_mark(marks);
		prof_sync();
#endif
	}
	draw();
}

int hud_input(void)
{
	if (keysDown() & KEY_TOUCH) {
		touchPosition tp;
		touchRead(&tp);
		if (tp.px >= 96 && tp.px < 160) return 1;   // centro: menú de pausa
		if (tp.px < 128) { page = (page + 1) % 3; consoleClear(); draw(); }
		else log_mark();
	}
	return 0;
}

void hud_redraw(void) { consoleClear(); draw(); }

void hud_second(int frames, u32 vblanks)
{
	u32 *p = psx_prof, total = p[PROF_FRAME] ? p[PROF_FRAME] : 1, other;
	s32 cpu_only = (s32)(p[PROF_CPU] - p[PROF_GPU] - p[PROF_MDEC] - p[PROF_COMPILE]);
	u32 t[T_N];

	if (!vblanks) vblanks = 1;
	s.target = gpu_disp.pal ? 50 : 60;
	s.fps10 = frames * 600 / (int)vblanks;
	s.game10 = (int)(psx_stat.disp_flips * 600 / vblanks);
	s.drawn10 = (int)(psx_stat.disp_drawn * 600 / vblanks);
	other = total > p[PROF_CPU] + p[PROF_PRESENT] + p[PROF_DISC] ? total - p[PROF_CPU] - p[PROF_PRESENT] - p[PROF_DISC] : 0;
	t[T_CPU] = cpu_only > 0 ? (u32)cpu_only : 0;
	t[T_GPU] = p[PROF_GPU];
	t[T_MDEC] = p[PROF_MDEC];
	t[T_COMPILE] = p[PROF_COMPILE];
	t[T_PRESENT] = p[PROF_PRESENT];
	t[T_DISC] = p[PROF_DISC];
	t[T_OTHER] = other;
	for (int i = 0; i < T_N; i++) s.pct[i] = (int)((u64)t[i] * 100 / total);
	s.worst_ms = frames ? frame_max / TICKS_MS : 0;
	s.best_ms = frames ? frame_min / TICKS_MS : 0;
	s.st = psx_stat;
	have_snapshot = 1;

	seconds++;
	total_frames += frames;
	if (seconds > 1) {   // el primer segundo incluye el arranque
		if (s.fps10 < fps_min10) fps_min10 = s.fps10;
		if (s.fps10 > fps_max10) fps_max10 = s.fps10;
		fps_sum10 += s.fps10;
		fps_n++;
	}
	memmove(hist, hist + 1, HIST - 1);
	hist[HIST - 1] = (u8)((s.fps10 + 5) / 10 > 255 ? 255 : (s.fps10 + 5) / 10);
	if (mark_shown) mark_shown--;

	if (logf) {
		u32 t0 = cpuGetTiming(), used, size;
		dyn_cache_use(&used, &size);
		fprintf(logf, "%lu,%d.%d,%d.%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%dx%d%s,%08lx,%lu,%lu,%lu,%lu,%lu.%lu,%lu,%lu,%lu,%lu,%d.%d,%lu,%lu\n",
		        seconds, s.fps10 / 10, s.fps10 % 10, s.game10 / 10, s.game10 % 10, s.pct[T_CPU], s.pct[T_GPU], s.pct[T_MDEC],
		        s.pct[T_COMPILE], s.pct[T_PRESENT], s.pct[T_DISC], s.pct[T_OTHER], s.worst_ms, s.best_ms, s.st.gpu_cmds,
		        s.st.gpu_tris, s.st.gpu_rects, s.st.vram_upload * 2 / 1024, s.st.cd_sectors, s.st.mdec_blocks,
		        s.st.dyn_compiled, s.st.dyn_interp_ops, s.st.dyn_interp_steps, s.st.dyn_smc, s.st.dyn_flushes,
		        size ? used * 100 / size : 0, gpu_disp.width, gpu_disp.height, gpu_disp.color24 ? "x24" : "", cpu.pc,
		        s.st.gpu_pixels / 1000, (u32)((u64)p[PROF_GTE] * 100 / total), s.st.dyn_idle, s.st.io_calls,
		        s.st.dyn_mips ? s.st.dyn_arm / s.st.dyn_mips : 0, s.st.dyn_mips ? s.st.dyn_arm * 10 / s.st.dyn_mips % 10 : 0,
		        gx_underflows, gx_late, gx_rdlines_min == 99 ? 0 : gx_rdlines_min, 25600ul / psx_cpu_scale, s.drawn10 / 10, s.drawn10 % 10,
		        s.st.tex_bands, s.st.fb_skips);
		gx_underflows = gx_late = 0;
		gx_rdlines_min = 99;
		log_lines++;
#ifdef SPAR_PROF
		prof_second(seconds);
		if (log_lines % 10 == 0) prof_sync();
#endif
		if (log_lines % 10 == 0) {   // a la tarjeta cada 10 s: si se apaga la consola se pierde poco
			fflush(logf);
			fsync(fileno(logf));
		}
		s.log_ms = (cpuGetTiming() - t0) / TICKS_MS;
	}

	draw();
	memset(psx_prof, 0, sizeof psx_prof);
	memset(&psx_stat, 0, sizeof psx_stat);
	frame_min = ~0u;
	frame_max = 0;
}
