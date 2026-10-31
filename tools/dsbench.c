// Banco de pruebas de DS: carga el núcleo libretro de melonDS, ejecuta una ROM N frames sin ventana
// y guarda las dos pantallas en PNG. uso: dsbench <núcleo.dll> <rom.nds> <frames> <salida.png> [dsi]
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable { const char *key; const char *value; };
struct retro_log_callback { void (*log)(int level, const char *fmt, ...); };

static const char *sysdir;
static int want_dsi;
static int pixfmt = 0;   // 1 = XRGB8888, 2 = RGB565
static uint8_t *frame;
static unsigned fw, fh;

static void logcb(int level, const char *fmt, ...)
{
	va_list ap;
	if (level < 2) return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static const char *var(const char *key)
{
	if (!strcmp(key, "melonds_console_mode")) return want_dsi ? "dsi" : "ds";
	if (!strcmp(key, "melonds_sysfile_mode")) return want_dsi ? "native" : "builtin";
	if (!strcmp(key, "melonds_boot_mode")) return "direct";
	if (!strcmp(key, "melonds_render_mode")) return "software";
	if (!strcmp(key, "melonds_threaded_renderer")) return "disabled";
	if (!strcmp(key, "melonds_number_of_screen_layouts")) return "1";
	if (!strcmp(key, "melonds_screen_layout1")) return "top-bottom";
	if (!strcmp(key, "melonds_screen_gap")) return "0";
	if (!strcmp(key, "melonds_slot2_device")) return "expansion-pak";   // RAM extra para las pruebas (BIOS)
	if (!strcmp(key, "melonds_jit_enable")) return "disabled";   // intérprete: tiempos más fieles
	if (!strcmp(key, "melonds_show_bios_warnings")) return "disabled";
	return NULL;
}

static bool env(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case 9: case 31: *(const char **)data = sysdir; return true;   // directorios de sistema y partidas
	case 10: pixfmt = *(int *)data; return true;                   // formato de píxel
	case 15: {                                                     // GET_VARIABLE
		struct retro_variable *v = data;
		v->value = var(v->key);
		if (getenv("DSB_VARS")) fprintf(stderr, "var %s = %s\n", v->key, v->value ? v->value : "(nada)");
		return v->value != NULL;
	}
	case 16: return true;                                          // SET_VARIABLES: sin esto no lee ninguna
	case 17: *(bool *)data = false; return true;                   // ¿variables cambiadas?
	case 27: ((struct retro_log_callback *)data)->log = logcb; return true;
	}
	return false;
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data) return;
	if (!frame || w != fw || h != fh) { free(frame); frame = malloc(w * h * 3); fw = w; fh = h; }
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			uint8_t *o = frame + (y * w + x) * 3;
			if (pixfmt == 1) {
				uint32_t p = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
				o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p;
			} else {
				uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
				o[0] = (uint8_t)((p >> 11) << 3); o[1] = (uint8_t)(((p >> 5) & 63) << 2); o[2] = (uint8_t)((p & 31) << 3);
			}
		}
}

// DSB_WAV=archivo: guarda el sonido de la DS (estéreo, 16 bits, a la frecuencia que da el núcleo)
static FILE *wavf;
static uint32_t wav_frames;
static void audio(int16_t l, int16_t r) { int16_t s[2] = { l, r }; if (wavf) { fwrite(s, 2, 2, wavf); wav_frames++; } }
static size_t audio_batch(const int16_t *d, size_t n) { if (wavf) { fwrite(d, 4, n, wavf); wav_frames += (uint32_t)n; } return n; }
static void wav_finish(unsigned rate)
{
	uint32_t h[11] = { 0x46464952, 36 + wav_frames * 4, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, wav_frames * 4 };
	if (!wavf) return;
	fseek(wavf, 0, SEEK_SET);
	fwrite(h, 4, 11, wavf);
	fclose(wavf);
	wavf = NULL;
}
// Pulsaciones simuladas: DSB_PRESS="start:300:60" pulsa START desde el frame 300 durante 60 frames
static int cur_frame;
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
	static const struct { const char *n; unsigned id; } names[] = {
		{ "b", 0 }, { "y", 1 }, { "select", 2 }, { "start", 3 }, { "up", 4 }, { "down", 5 }, { "left", 6 },
		{ "right", 7 }, { "a", 8 }, { "x", 9 }, { "l", 10 }, { "r", 11 },
	};
	const char *spec = getenv("DSB_PRESS");
	char btn[16];
	int from, len;
	(void)idx;
	if (port || dev != 1 || !spec || sscanf(spec, "%15[a-z]:%d:%d", btn, &from, &len) != 3) return 0;
	if (cur_frame < from || cur_frame >= from + len) return 0;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcmp(btn, names[i].n) && names[i].id == id) return 1;
	return 0;
}

// PNG sin comprimir
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
	if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320 ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
	c ^= 0xffffffff;
	while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffff;
}
static void be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n)
{
	uint8_t h[8];
	be(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f);
	if (n) fwrite(d, 1, n, f);
	be(h, crc(crc(0, (const uint8_t *)t, 4), d, n)); fwrite(h, 1, 4, f);
}
static void save_png(const char *path)
{
	uint32_t raw = (fw * 3 + 1) * fh, pos = 0, a = 1, b = 0;
	uint8_t *r = malloc(raw), *z = malloc(raw + raw / 65535 * 5 + 64), ih[13];
	FILE *f = fopen(path, "wb");
	for (unsigned y = 0; y < fh; y++) { r[y * (fw * 3 + 1)] = 0; memcpy(r + y * (fw * 3 + 1) + 1, frame + y * fw * 3, fw * 3); }
	z[pos++] = 0x78; z[pos++] = 1;
	for (uint32_t o = 0; o < raw; o += 65535) {
		uint32_t n = raw - o < 65535 ? raw - o : 65535;
		z[pos++] = o + n >= raw; z[pos++] = (uint8_t)n; z[pos++] = (uint8_t)(n >> 8); z[pos++] = (uint8_t)~n; z[pos++] = (uint8_t)(~n >> 8);
		memcpy(z + pos, r + o, n); pos += n;
	}
	for (uint32_t i = 0; i < raw; i++) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
	be(z + pos, (b << 16) | a); pos += 4;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be(ih, fw); be(ih + 4, fh); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
	chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, pos); chunk(f, "IEND", NULL, 0);
	fclose(f); free(r); free(z);
}

int main(int argc, char **argv)
{
	HMODULE m;
	struct retro_game_info gi = { 0 };
	int frames;
	static char dir[MAX_PATH];
	if (argc < 5) { fprintf(stderr, "uso: dsbench <nucleo.dll> <rom.nds> <frames> <salida.png> [dsi]\n"); return 1; }
	want_dsi = argc > 5 && !strcmp(argv[5], "dsi");
	snprintf(dir, sizeof dir, "%s", argv[1]);
	*strrchr(dir, '\\') = 0;
	strcat(dir, "\\system");
	CreateDirectoryA(dir, NULL);
	sysdir = dir;
	m = LoadLibraryA(argv[1]);
	if (!m) { fprintf(stderr, "no carga el núcleo\n"); return 1; }
#define F(name, ...) __typeof__(__VA_ARGS__) name = (__typeof__(__VA_ARGS__))(void *)GetProcAddress(m, #name)
	F(retro_set_environment, void (*)(bool (*)(unsigned, void *)));
	F(retro_set_video_refresh, void (*)(void (*)(const void *, unsigned, unsigned, size_t)));
	F(retro_set_audio_sample, void (*)(void (*)(int16_t, int16_t)));
	F(retro_set_audio_sample_batch, void (*)(size_t (*)(const int16_t *, size_t)));
	F(retro_set_input_poll, void (*)(void (*)(void)));
	F(retro_set_input_state, void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned)));
	F(retro_init, void (*)(void));
	F(retro_load_game, bool (*)(const struct retro_game_info *));
	F(retro_run, void (*)(void));
	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audio);
	retro_set_audio_sample_batch(audio_batch);
	retro_set_input_poll(poll);
	retro_set_input_state(input);
	retro_init();
	gi.path = argv[2];
	{
		FILE *rf = fopen(argv[2], "rb");
		long n;
		void *buf;
		if (!rf) { fprintf(stderr, "no abre la ROM\n"); return 1; }
		fseek(rf, 0, SEEK_END);
		n = ftell(rf);
		fseek(rf, 0, SEEK_SET);
		buf = malloc(n);
		if (fread(buf, 1, n, rf) != (size_t)n) return 1;
		fclose(rf);
		gi.data = buf;
		gi.size = (size_t)n;
	}
	if (getenv("DSB_WAV") && (wavf = fopen(getenv("DSB_WAV"), "wb"))) fseek(wavf, 44, SEEK_SET);
	if (!retro_load_game(&gi)) { fprintf(stderr, "no carga la ROM\n"); return 1; }
	frames = atoi(argv[3]);
	F(retro_get_memory_data, void *(*)(unsigned));
	F(retro_get_memory_size, size_t (*)(unsigned));
	{
		// DSB_SNAP=N: mide entre el frame N y el final con los totales «SPBENCH» del frontend
		int every = getenv("DSB_EVERY") ? atoi(getenv("DSB_EVERY")) : 0;   // captura cada N frames (salida_NNNNN.png)
		int snap = getenv("DSB_SNAP") ? atoi(getenv("DSB_SNAP")) : -1;
		// DSB_PSX=desde:hasta mide entre esos frames de la PS1 (el mismo trozo de juego vaya rápido o lento)
		unsigned psx_from = 0, psx_to = 0;
		int measuring = 0;
		uint32_t b0[64] = { 0 };
		uint8_t *ram = NULL;
		size_t ramn = 0;
		uint32_t *bench = NULL;
#define BENCH_N 6 + 8 + 28
		if (retro_get_memory_data && retro_get_memory_size) { ram = retro_get_memory_data(2); ramn = retro_get_memory_size(2); }
		if (getenv("DSB_PSX")) sscanf(getenv("DSB_PSX"), "%u:%u", &psx_from, &psx_to);
		// DSB_PROF=archivo: desde el frame DSB_SNAP, las muestras del perfilador (ds/source/prof.c, marca
		// «SPPROFRG»): pares (dirección ARM, PC MIPS del bloque) tras una cabecera con las zonas del dynarec
		const char *prof_path = getenv("DSB_PROF");
		FILE *prof_f = NULL;
		uint32_t *prof = NULL, prof_r = 0;
		for (int i = 0; i < frames; i++) {
			cur_frame = i;
			retro_run();
			if (psx_to && ram && !bench && i >= 1)
				for (size_t o = 0; o + 8 + BENCH_N * 4 <= ramn; o += 4)
					if (!memcmp(ram + o, "SPBENCH", 8)) { bench = (uint32_t *)(ram + o + 8); break; }
			if (psx_to && bench && !measuring && bench[0] >= psx_from) { memcpy(b0, bench, BENCH_N * 4); measuring = 1; }
			if (psx_to && bench && bench[0] >= psx_to) break;
			if (!psx_to && i == snap) measuring = 1;
			if (prof_path && ram && measuring) {
				if (!prof) {
					for (size_t o = 0; o + 64 <= ramn; o += 4)
						if (!memcmp(ram + o, "SPPROFRG", 8)) { prof = (uint32_t *)(ram + o + 8); break; }
					if (prof && (prof_f = fopen(prof_path, "wb"))) { fwrite(prof + 1, 4, 6, prof_f); prof_r = prof[0]; }
					else prof = NULL;
				}
				if (prof) {
					uint32_t w = prof[0], *ring = prof + 7;
					if (w - prof_r > 2048) prof_r = w - 2048;
					for (; prof_r != w; prof_r++) fwrite(&ring[(prof_r & 2047) * 2], 4, 2, prof_f);
				}
			}
			if (every && i && i % every == 0 && frame) {
				char name[1024];
				snprintf(name, sizeof name, "%.*s_%05d.png", (int)(strlen(argv[4]) - 4), argv[4], i);
				save_png(name);
			}
			if (i == snap && ram && !psx_from) {
				for (size_t o = 0; o + 8 + BENCH_N * 4 <= ramn; o += 4)
					if (!memcmp(ram + o, "SPBENCH", 8)) { bench = (uint32_t *)(ram + o + 8); }
				if (bench) memcpy(b0, bench, BENCH_N * 4);
				else printf("sin SPBENCH en la RAM\n");
			}
		}
		if (bench) {
			// tras la cabecera: frames, vblanks, pc, ram, código, código usado, tiempos, contadores
			enum { B_PROF = 6, B_STAT = 14 };
			static const char *pn[8] = { "cpu", "gpu", "present", "frame", "disc", "mdec", "compile", "gte" };
			static const char *sn[28] = { "gpu_cmds", "tris", "rects", "lines", "fills", "copies", "vram_up", "gpu_px", "flips",
			                              "cd_sect", "mdec_blk", "compiled", "flushes", "smc", "interp_ops", "interp_steps", "idle", "mips", "arm", "gte_ops", "io", "sio_b", "sio_ack", "sio_poll", "tri_rapido", "tri_otro", "rect_rapido", "rect_otro" };
			uint32_t d[BENCH_N];
			double secs;
			const char *dump = getenv("DSB_PSXMEM");   // DSB_PSXMEM=desplazamiento:bytes:archivo vuelca RAM de la PS1
			const char *code = getenv("DSB_CODE");     // DSB_CODE=archivo vuelca la caché del dynarec (dirección en el nombre .addr)
			if (code && bench[4] >= 0x02000000u && bench[4] - 0x02000000u + bench[5] <= ramn) {
				FILE *cf = fopen(code, "wb");
				if (cf) { fwrite(ram + bench[4] - 0x02000000u, 1, bench[5], cf); fclose(cf); }
				printf("CODE %08x %u\n", bench[4], bench[5]);
			}
			for (int k = 0; k < BENCH_N; k++) d[k] = bench[k] - b0[k];
			secs = d[1] / 60.0;
			if (secs <= 0) secs = 1;
			printf("BENCH frames=%u vblanks=%u -> %.2f fps (juego %.2f)  pc=%08x\n", d[0], d[1], d[0] / secs, d[B_STAT + 8] / secs, bench[2]);
			printf("  tiempo:");
			for (int k = 0; k < 8; k++) printf(" %s=%.1f%%", pn[k], d[B_PROF + 3] ? d[B_PROF + k] * 100.0 / d[B_PROF + 3] : 0.0);
			if (d[0]) {   // ms por frame de la PS1 (temporizador a 33,514 MHz); «ejec» = CPU sin GPU, MDEC ni traducir
				double f = d[0] * 33514.0;
				printf("\n  ms/frame: ejec=%.1f gte=%.1f gpu=%.1f traducir=%.1f mdec=%.1f copia=%.1f total=%.1f",
				       ((double)d[B_PROF] - d[B_PROF + 1] - d[B_PROF + 5] - d[B_PROF + 6] - d[B_PROF + 7]) / f, d[B_PROF + 7] / f, d[B_PROF + 1] / f, d[B_PROF + 6] / f,
				       d[B_PROF + 5] / f, d[B_PROF + 2] / f, d[B_PROF + 3] / f);
				printf("\n  DSi estimado (sin traducir): %.1f ms/frame", ((double)d[B_PROF + 3] - d[B_PROF + 6]) / f);
			}
			printf("\n  por s:");
			for (int k = 0; k < 28; k++) printf(" %s=%.0f", sn[k], d[B_STAT + k] / secs);
			printf("\n");
			if (dump) {
				unsigned off, len;
				char path[1024];
				uint32_t base = bench[3] - 0x02000000u;
				if (sscanf(dump, "%x:%u:%1023s", &off, &len, path) == 3 && base + off + len <= ramn) {
					FILE *df = fopen(path, "wb");
					if (df) { fwrite(ram + base + off, 1, len, df); fclose(df); }
				} else printf("DSB_PSXMEM: fuera de la RAM\n");
			}
		}
	}
	{   // marca de depuración del frontend (gx_dbg en ds/source/gx.c): en qué paso se quedó
		uint8_t *r = retro_get_memory_data ? retro_get_memory_data(2) : NULL;
		size_t rn = retro_get_memory_size ? retro_get_memory_size(2) : 0;
		for (size_t o = 0; r && o + 16 <= rn; o += 4)
			if (!memcmp(r + o, "SPDBGSTG", 8)) { uint32_t *s = (uint32_t *)(r + o + 8); printf("STAGE %u (n=%u vbl=%u dyn=%u)\n", s[0], s[1], s[2], s[3]); break; }
	}
	if (getenv("DSB_MAGIC")) {   // DSB_MAGIC=MARCA8:bytes:archivo vuelca lo que hay tras esa marca en la RAM de la DS
		char mg[9], path[1024];
		unsigned len;
		uint8_t *r = retro_get_memory_data ? retro_get_memory_data(2) : NULL;
		size_t rn = retro_get_memory_size ? retro_get_memory_size(2) : 0;
		if (sscanf(getenv("DSB_MAGIC"), "%8[^:]:%u:%1023s", mg, &len, path) == 3)
			for (size_t o = 0; r && o + 8 + len <= rn; o += 4)
				if (!memcmp(r + o, mg, 8)) { FILE *f = fopen(path, "wb"); if (f) { fwrite(r + o + 8, 1, len, f); fclose(f); } break; }
	}
	wav_finish((unsigned)(wav_frames * 59.8261 / (frames > 0 ? frames : 1) + 0.5));   // frecuencia sacada de lo recibido
	if (frame) save_png(argv[4]);
	printf("ok %ux%u\n", fw, fh);
	return 0;
}
