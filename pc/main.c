// Banco de pruebas en PC: corre el núcleo sin ventana y guarda capturas en BMP.
// uso: psxpc <bios> [juego.exe|juego.cue|juego.bin] [frames] [salida_prefijo] [--vram]
#include "../core/psx.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static u8 *load_file(const char *path, u32 *len)
{
	FILE *f = fopen(path, "rb");
	u8 *d;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(n);
	if (d && fread(d, 1, n, f) != (size_t)n) { free(d); d = NULL; }
	fclose(f);
	*len = (u32)n;
	return d;
}

// PNG sin comprimir (bloques «stored» de deflate): no necesita zlib
static u32 crc_table[256];

static u32 crc32_buf(u32 c, const u8 *p, size_t n)
{
	if (!crc_table[1])
		for (u32 i = 0; i < 256; i++) {
			u32 v = i;
			for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320 ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	c ^= 0xffffffff;
	while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffff;
}

static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 len)
{
	u8 hdr[8];
	u32 crc;
	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len) fwrite(data, 1, len, f);
	crc = crc32_buf(crc32_buf(0, (const u8 *)type, 4), data, len);
	be32(hdr, crc);
	fwrite(hdr, 1, 4, f);
}

static void write_png(const char *path, int w, int h, const u8 *rgb)
{
	FILE *f = fopen(path, "wb");
	u32 raw_len = (u32)(w * 3 + 1) * h, blocks = (raw_len + 65534) / 65535;
	u32 zlen = 2 + raw_len + blocks * 5 + 4, a = 1, b = 0, pos = 0;
	u8 *raw = malloc(raw_len), *z = malloc(zlen), ihdr[13];
	if (!f) return;
	for (int y = 0; y < h; y++) {
		raw[y * (w * 3 + 1)] = 0;
		memcpy(raw + y * (w * 3 + 1) + 1, rgb + y * w * 3, w * 3);
	}
	z[pos++] = 0x78;
	z[pos++] = 0x01;
	for (u32 off = 0; off < raw_len; off += 65535) {
		u32 n = raw_len - off < 65535 ? raw_len - off : 65535;
		z[pos++] = off + n >= raw_len;
		z[pos++] = (u8)n; z[pos++] = (u8)(n >> 8);
		z[pos++] = (u8)~n; z[pos++] = (u8)(~n >> 8);
		memcpy(z + pos, raw + off, n);
		pos += n;
	}
	for (u32 i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	be32(z + pos, (b << 16) | a);
	pos += 4;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ihdr, w); be32(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
	png_chunk(f, "IHDR", ihdr, 13);
	png_chunk(f, "IDAT", z, pos);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
}

static int ds_rows;   // --rows: la captura toma las filas como la DS (192 líneas)

static void dump_display(const char *path)
{
	int w = gpu_disp.width, h = gpu_disp.height, oh = ds_rows ? 192 : h;
	u8 *rgb = malloc(w * h * 3);
	for (int oy = 0; oy < oh; oy++)
		for (int x = 0; x < w; x++) {
			int y = ds_rows ? (ds_rows == 2 ? (oy * h / 192) & ~1 : oy * h / 192) : oy;
			u8 *o = rgb + (oy * w + x) * 3;
			if (gpu_disp.color24) {
				const u8 *line = (const u8 *)&psx_vram[((gpu_disp.disp_y + y) & 511) * 1024];
				int bx = gpu_disp.disp_x * 2 + x * 3;
				o[0] = line[bx % 2048]; o[1] = line[(bx + 1) % 2048]; o[2] = line[(bx + 2) % 2048];
			} else {
				u16 c = psx_vram[((gpu_disp.disp_y + y) & 511) * 1024 + ((gpu_disp.disp_x + x) & 1023)];
				o[0] = (u8)((c & 31) << 3); o[1] = (u8)(((c >> 5) & 31) << 3); o[2] = (u8)(((c >> 10) & 31) << 3);
			}
		}
	write_png(path, w, oh, rgb);
	free(rgb);
}

static void dump_vram(const char *path)
{
	u8 *rgb = malloc(1024 * 512 * 3);
	for (int i = 0; i < 1024 * 512; i++) {
		u16 c = psx_vram[i];
		rgb[i * 3] = (u8)((c & 31) << 3); rgb[i * 3 + 1] = (u8)(((c >> 5) & 31) << 3); rgb[i * 3 + 2] = (u8)(((c >> 10) & 31) << 3);
	}
	write_png(path, 1024, 512, rgb);
	free(rgb);
}

// Píxeles de VRAM que no coinciden con una captura de referencia (BMP de 24 bits, 1024x512)
static int vram_diff(const char *path)
{
	u32 len;
	u8 *b = load_file(path, &len);
	int bad = 0;
	if (!b || len < 54 + 1024 * 512 * 3) return -1;
	for (int y = 0; y < 512; y++)
		for (int x = 0; x < 1024; x++) {
			const u8 *p = b + 54 + ((511 - y) * 1024 + x) * 3;
			u16 c = psx_vram[y * 1024 + x];
			if ((p[2] >> 3) != (c & 31) || (p[1] >> 3) != ((c >> 5) & 31) || (p[0] >> 3) != ((c >> 10) & 31)) bad++;
		}
	free(b);
	return bad;
}

// ---- disco .bin (sectores de 2352 bytes) ----
static FILE *disc;

static FILE *lba_log;   // --lbas archivo: anota cada sector leído (para montar discos reducidos)

static int read_sector(void *user, u32 lba, u8 *out)
{
	(void)user;
	if (lba_log) fprintf(lba_log, "%u\n", lba);
	if (fseek(disc, (long)lba * 2352, SEEK_SET)) return 0;
	return fread(out, 1, 2352, disc) == 2352;
}

static int open_disc(const char *path)
{
	char bin[1024];
	const char *ext = strrchr(path, '.');
	strncpy(bin, path, sizeof bin - 1);
	bin[sizeof bin - 1] = 0;
	if (ext && (!strcmp(ext, ".cue") || !strcmp(ext, ".CUE"))) {
		// FILE "nombre.bin" BINARY, relativo a la carpeta del .cue
		FILE *c = fopen(path, "r");
		char line[1024];
		if (!c) return 0;
		while (fgets(line, sizeof line, c)) {
			char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			if (strstr(line, "FILE") && q1 && q2) {
				const char *slash = strrchr(path, '/');
				const char *bs = strrchr(path, '\\');
				int dirlen;
				if (bs > slash) slash = bs;
				dirlen = slash ? (int)(slash - path + 1) : 0;
				*q2 = 0;
				snprintf(bin, sizeof bin, "%.*s%s", dirlen, path, q1 + 1);
				break;
			}
		}
		fclose(c);
	}
	disc = fopen(bin, "rb");
	if (!disc) { fprintf(stderr, "no puedo abrir %s\n", bin); return 0; }
	fseek(disc, 0, SEEK_END);
	cdrom_set_disc(read_sector, NULL, (u32)(ftell(disc) / 2352));
	return 1;
}

static int state_write(void *u, void *p, u32 len) { return fwrite(p, 1, len, u) == len; }
static int state_read(void *u, void *p, u32 len) { return fread(p, 1, len, u) == len; }

static void tty(int c) { fputc(c, stdout); fflush(stdout); }

// ---- perfil: PC muestreado al final de cada línea ----
void mips_dis(u32 pc, u32 op);
#define MAX_SAMPLES (1 << 20)
static u32 *samples;
static int nsamples, profiling;

static void sample_pc(void)
{
	if (profiling && nsamples < MAX_SAMPLES) samples[nsamples++] = cpu.pc;
}

static int cmp_u32(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;
	return x < y ? -1 : x > y;
}

static void report_profile(void)
{
	u32 best_pc[16] = { 0 };
	int best_n[16] = { 0 };
	qsort(samples, nsamples, 4, cmp_u32);
	for (int i = 0; i < nsamples;) {
		int j = i;
		while (j < nsamples && samples[j] == samples[i]) j++;
		for (int k = 0; k < 16; k++)
			if (j - i > best_n[k]) {
				memmove(best_n + k + 1, best_n + k, (15 - k) * sizeof(int));
				memmove(best_pc + k + 1, best_pc + k, (15 - k) * sizeof(u32));
				best_n[k] = j - i;
				best_pc[k] = samples[i];
				break;
			}
		i = j;
	}
	printf("\n--- perfil (%d muestras) ---\n", nsamples);
	for (int k = 0; k < 16 && best_n[k]; k++) printf("%08x  %5.1f%%\n", best_pc[k], 100.0 * best_n[k] / nsamples);
	printf("SR=%08x CAUSE=%08x EPC=%08x istat=%03x imask=%03x\n", cpu.cop0[12], cpu.cop0[13], cpu.cop0[14], irq_stat, irq_mask);
	for (int i = 0; i < 32; i++) printf("r%-2d=%08x%s", i, cpu.r[i], i % 4 == 3 ? "\n" : "  ");
	printf("--- código alrededor de %08x ---\n", best_pc[0]);
	for (u32 a = best_pc[0] - 40; a < best_pc[0] + 40; a += 4) mips_dis(a, bus_read32(a));
	printf("--- código alrededor de EPC %08x ---\n", cpu.cop0[14]);
	for (u32 a = cpu.cop0[14] - 24; a < cpu.cop0[14] + 16; a += 4) mips_dis(a, bus_read32(a));
}

int main(int argc, char **argv)
{
	u32 len;
	u8 *bios, *exe = NULL;
	int frames = 300, want_vram = 0, want_prof = 0, dump_from = 0;
	int rows_mode = 0;
	const char *out = "frame";
	clock_t t0;

	if (argc < 2) { fprintf(stderr, "uso: psxpc <bios> [exe|cue|bin] [frames] [prefijo] [--vram] [--prof]\n"); return 1; }
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--vram")) want_vram = 1;
		if (!strcmp(argv[i], "--prof")) want_prof = 1;
		if (!strcmp(argv[i], "--from") && i + 1 < argc) dump_from = atoi(argv[i + 1]);   // guarda cada frame desde aquí
		if (!strcmp(argv[i], "--lbas") && i + 1 < argc) lba_log = fopen(argv[i + 1], "a");
		if (!strcmp(argv[i], "--frameskip") && i + 1 < argc) gpu_frameskip = (u8)atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows_mode = ds_rows = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--halfx")) gpu_half_x = 1;
		// (se aplica tras psx_init)   // como la DS: 1 = filas que se ven, 2 = pares
	}
	if (want_prof) {
		samples = malloc(MAX_SAMPLES * 4);
		psx_line_cb = sample_pc;
	}
	if (!psx_init()) return 1;
	if (rows_mode) gpu_rows_for(rows_mode, 192);
	bios = load_file(argv[1], &len);
	if (!bios || len != 512 * 1024) { fprintf(stderr, "BIOS inválida\n"); return 1; }
	psx_bios = bios;
	psx_tty = tty;

	if (argc >= 3 && strcmp(argv[2], "-")) {
		const char *ext = strrchr(argv[2], '.');
		if (ext && (!strcmp(ext, ".exe") || !strcmp(ext, ".EXE") || !strcmp(ext, ".ps-exe"))) {
			exe = load_file(argv[2], &len);
			if (!exe || !psx_sideload_exe(exe, len)) { fprintf(stderr, "EXE inválido\n"); return 1; }
		} else if (!open_disc(argv[2])) return 1;
	}
	if (argc >= 4) frames = atoi(argv[3]);
	if (argc >= 5 && strncmp(argv[4], "--", 2)) out = argv[4];

#ifdef PSX_GPUSTAT
	{ extern const GpuHw dbg_hw; if (getenv("HWDBG")) gpu_hw = &dbg_hw; }
#endif
	psx_reset();
	for (int i = 1; i + 1 < argc; i++)   // --load archivo: empieza desde un estado guardado
		if (!strcmp(argv[i], "--load")) {
			FILE *sf = fopen(argv[i + 1], "rb");
			if (!sf || !psx_state_load(state_read, sf)) { fprintf(stderr, "no carga el estado %s\n", argv[i + 1]); return 1; }
			fclose(sf);
		}
	t0 = clock();
	for (int f = 1; f <= frames; f++) {
		profiling = f > frames - 60;
		{ extern int gpudump; gpudump = f > frames - 6; }
#ifdef PSX_OPSTAT
		{ extern int psx_opstat_on; psx_opstat_on = profiling; }
#endif
		// --press F:boton mantiene el botón pulsado 6 frames desde el frame F
		psx_pad_buttons = 0xffff;
		for (int i = 1; i + 1 < argc; i++) {
			static const struct { const char *n; u16 b; } names[] = {
				{ "start", PAD_START }, { "select", PAD_SELECT }, { "x", PAD_CROSS }, { "o", PAD_CIRCLE },
				{ "tri", PAD_TRIANGLE }, { "sq", PAD_SQUARE }, { "up", PAD_UP }, { "down", PAD_DOWN },
				{ "left", PAD_LEFT }, { "right", PAD_RIGHT }, { "l1", PAD_L1 }, { "r1", PAD_R1 },
				{ "l2", PAD_L2 }, { "r2", PAD_R2 },
			};
			int at, len = 6;
			char btn[16];
			if (strcmp(argv[i], "--press") || sscanf(argv[i + 1], "%d:%15[a-z0-9]:%d", &at, btn, &len) < 2) continue;
			if (f < at || f >= at + len) continue;
			for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
				if (!strcmp(btn, names[k].n)) psx_pad_buttons &= ~names[k].b;
		}

		psx_run_frame();
		for (int i = 1; i + 1 < argc; i++) {   // --save F:archivo guarda el estado tras el frame F
			int at;
			char sname[512];
			if (!strcmp(argv[i], "--save") && sscanf(argv[i + 1], "%d:%511s", &at, sname) == 2 && at == f) {
				FILE *sf = fopen(sname, "wb");
				printf("estado %s: %s\n", sname, sf && psx_state_save(state_write, sf) ? "guardado" : "ERROR");
				if (sf) fclose(sf);
			}
		}
#ifdef PSX_TRACE_IO
		if (f % 120 == 0) { extern u32 sio_polls; printf("frame %d: %u lecturas del mando\n", f, sio_polls); }
#endif
		if (getenv("PSX_DISPLOG")) {   // registro de cambios del modo de pantalla
			static GpuDisplay last;
			if (memcmp(&last, &gpu_disp, offsetof(GpuDisplay, frame_done)))
				printf("frame %d: %dx%d en (%d,%d)%s%s%s\n", f, gpu_disp.width, gpu_disp.height, gpu_disp.disp_x,
				       gpu_disp.disp_y, gpu_disp.color24 ? " 24bit" : "", gpu_disp.interlace ? " entrelazado" : "",
				       gpu_disp.disabled ? " APAGADA" : "");
			last = gpu_disp;
		}
		if (f % 60 == 0 || f == frames || (dump_from && f >= dump_from)) {
			char name[512];
			snprintf(name, sizeof name, "%s_%04d%s.png", out, f, gpu_disp.stale ? "_saltado" : "");
			dump_display(name);
		}
	}
	{
		double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
		printf("\n[%d frames en %.2f s = %.1f fps; pc=%08x disp %dx%d en (%d,%d)%s]\n", frames, secs, frames / secs,
		       cpu.pc, gpu_disp.width, gpu_disp.height, gpu_disp.disp_x, gpu_disp.disp_y, gpu_disp.disabled ? " APAGADA" : "");
	}
#ifdef PSX_TRACE_IO
	{ extern u32 gp0_hist[256]; for (int i = 0; i < 256; i++) if (gp0_hist[i]) printf("GP0 %02x x%u\n", i, gp0_hist[i]); }
#endif
	if (want_vram) {
		char name[512];
		snprintf(name, sizeof name, "%s_vram.png", out);
		dump_vram(name);
	}
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--ref")) printf("VRAM_DIFF %d\n", vram_diff(argv[i + 1]));
	if (want_prof) report_profile();
#ifdef PSX_GPUSTAT
	{
		extern u32 gpustat[16];
		printf("GPUSTAT polis=%u brillo>1=%u semi0=%u semi1=%u semi2=%u semi3=%u t4=%u t8=%u t15=%u gour=%u tex+semi=%u clut_sin_bit15=%u mezcla=%u todo_bit15=%u\n", gpustat[0], gpustat[1], gpustat[2], gpustat[3], gpustat[4], gpustat[5], gpustat[6], gpustat[7], gpustat[8], gpustat[10], gpustat[11], gpustat[12], gpustat[13], gpustat[14]);
	}
#endif
#ifdef PSX_OPSTAT
	{   // instrucciones por frame en los 60 últimos frames, por tipo
		extern u32 psx_opstat[18];
		static const char *const nm[18] = { "total", "en_delay", "nop", "lw_sp", "lw_ram", "lw_scr", "lw_io", "sw_sp", "sw_ram", "sw_scr", "sw_io", "gte_cmd", "gte_mov", "mul_div_hilo", "jr", "saltos", "cop0", "alu" };
		for (int k = 0; k < 18; k++) printf("OPSTAT %-13s %8.0f/frame  %5.1f%%\n", nm[k], psx_opstat[k] / 60.0, 100.0 * psx_opstat[k] / (psx_opstat[0] ? psx_opstat[0] : 1));
	}
#endif
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--mem")) {   // volcado de memoria: --mem dirección
			u32 a = (u32)strtoul(argv[i + 1], NULL, 16);
			for (u32 k = 0; k < 64; k += 4) printf("%08x: %08x %08x %08x %08x\n", a + k * 4, bus_read32(a + k * 4),
			                                        bus_read32(a + k * 4 + 4), bus_read32(a + k * 4 + 8), bus_read32(a + k * 4 + 12));
		}
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--dump")) {   // --dump desplazamiento:bytes:archivo (RAM en binario)
			unsigned off, n;
			char path[512];
			FILE *df;
			if (sscanf(argv[i + 1], "%x:%u:%511s", &off, &n, path) == 3 && off + n <= 0x200000 && (df = fopen(path, "wb"))) {
				fwrite(psx_ram + off, 1, n, df);
				fclose(df);
			}
		}
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--dis")) {
			u32 a = (u32)strtoul(argv[i + 1], NULL, 16);
			for (u32 k = 0; k < 48; k++) mips_dis(a + k * 4, bus_read32(a + k * 4));
		}
	return 0;
}
u32 gpustat[16];
int gpudump;
#ifdef PSX_GPUSTAT
// receptor de prueba del dibujo por hardware: cuenta orígenes y áreas en el último frame
static void dbg_poly(const GpuHwVtx *v, int n, const GpuHwPoly *p)
{
	extern int gpudump;
	int x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999;
	for (int k = 0; k < n; k++) {
		if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
		if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
	}
	if (gpudump && x0 <= 160 && x1 >= 160 && y0 <= 376 && y1 >= 376)
		printf("HW bbox=%d,%d-%d,%d tex=%d semi=%d raw=%d g=%d c1=%d c2=%d c3=%d col=%d,%d,%d uv=%d,%d clut=%d,%d tp=%d,%d\n", x0, y0, x1, y1, p->tex, p->semi,
		       p->raw, p->gouraud, v[1].r, v[2].r, n > 3 ? v[3].r : -1, v[0].r, v[0].g, v[0].b, v[0].u, v[0].v, p->cx, p->cy, p->tpx, p->tpy);
}
static void dbg_written(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void dbg_flip(int d) { extern int gpudump; if (gpudump) printf("HW flip %d\n", d); }
static void dbg_clear(u16 c) { (void)c; }
const GpuHw dbg_hw = { dbg_poly, dbg_written, dbg_flip, dbg_clear };
#endif
