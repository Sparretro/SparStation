// Controlador de CD-ROM: comandos, respuestas con su retraso, lectura de sectores y FIFO de datos.
#include "psx.h"
#ifdef PSX_TRACE_IO
#include <stdio.h>
#endif

enum { ST_ERROR = 1, ST_MOTOR = 2, ST_SHELL = 0x10, ST_READ = 0x20, ST_SEEK = 0x40, ST_PLAY = 0x80 };

typedef struct { u8 irq, len; u8 data[12]; u32 delay; } Resp;

static psx_sector_reader reader;
static void *reader_user;
static u32 disc_sectors;

static u8 index_reg, int_enable, int_flag;
static u8 params[16], nparams;
static u8 resp[16], resp_len, resp_pos;
static Resp queue[4];
static int qn;
static u32 q_due;

static u8 mode, stat;
static u8 filter_file, filter_chan, muted;   // filtro XA (Setfilter) y Mute
const CdAudioHw *cd_audio_hw;
static u32 seek_lba, cur_lba;
static int seek_pending, reading;
static u32 read_due;

static u8 sector[2352];
static u8 last_header[8];
static u8 data_fifo[2352];
static u32 data_len, data_pos;

static u8 bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 unbcd(u8 v) { return (v >> 4) * 10 + (v & 0xf); }

void cdrom_set_disc(psx_sector_reader rd, void *user, u32 sectors)
{
	reader = rd;
	reader_user = user;
	disc_sectors = sectors;
}

void cdrom_reset(void)
{
	index_reg = int_enable = int_flag = 0;
	nparams = resp_len = resp_pos = 0;
	qn = 0;
	mode = 0;
	stat = reader ? ST_MOTOR : ST_SHELL;
	filter_file = filter_chan = muted = 0;
	seek_lba = cur_lba = 0;
	seek_pending = reading = 0;
	data_len = data_pos = 0;
}

static void push(u8 irq, u32 delay, int n, const u8 *d)
{
	Resp *r;
	if (qn == 4) return;
	r = &queue[qn];
	r->irq = irq;
	r->len = (u8)n;
	memcpy(r->data, d, n);
	r->delay = delay;
	if (qn == 0) { q_due = psx_cycles + delay; psx_schedule(q_due); }
	qn++;
}

static void push_stat(u8 irq, u32 delay) { u8 s = stat; push(irq, delay, 1, &s); }

static void error(u8 code)
{
	u8 d[2] = { (u8)(stat | ST_ERROR), code };
	push(5, 25000, 2, d);
}

static u32 sector_time(void) { return (mode & 0x80) ? PSX_CPU_HZ / 150 : PSX_CPU_HZ / 75; }

#ifdef PSX_CDLOG
// Pruebas: las órdenes al CD (orden | parámetros << 8), para comparar el PC con la DS (DSB_MAGIC=SPCDLOG_)
struct { char magic[8]; u32 n, e[8192]; } cd_log = { "SPCDLOG_" };
#ifdef PSX_TTY_CDLOG
#define CDLOG(x) do { u32 x_ = (x); printf("L %08x\n", (unsigned)x_); if (cd_log.n < 8192) cd_log.e[cd_log.n++] = x_; } while (0)
#else
#define CDLOG(x) do { if (cd_log.n < 8192) cd_log.e[cd_log.n++] = (x); } while (0)
#endif
#endif

static void command(u8 cmd)
{
#ifdef PSX_CDLOG
	CDLOG(cmd | (u32)params[0] << 8 | (u32)params[1] << 16 | (u32)params[2] << 24);
#endif
	if (cd_audio_hw && (cmd == 0x08 || cmd == 0x09 || cmd == 0x0a || cmd == 0x15 || cmd == 0x16 ||
	                    ((cmd == 0x06 || cmd == 0x1b) && seek_pending)))
		cd_audio_hw->stop();   // el cabezal se para o salta: lo que sonaba del XA se acaba
	switch (cmd) {
	case 0x01: push_stat(3, 25000); stat &= ~ST_SHELL; if (reader) stat |= ST_MOTOR; break;   // Getstat
	case 0x02:   // Setloc
		seek_lba = (unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2]);
		seek_lba = seek_lba >= 150 ? seek_lba - 150 : 0;
		seek_pending = 1;
		push_stat(3, 25000);
		break;
	case 0x03: push_stat(3, 25000); break;   // Play: el audio CD todavía no suena
	case 0x06: case 0x1b:                     // ReadN / ReadS
		if (!reader) { error(0x80); break; }
		if (seek_pending) { cur_lba = seek_lba; seek_pending = 0; }
		stat = (stat & ~(ST_SEEK | ST_PLAY)) | ST_READ | ST_MOTOR;
		push_stat(3, 25000);
		reading = 1;
		read_due = psx_cycles + sector_time() + 25000;
		psx_schedule(read_due);
		break;
	case 0x07: push_stat(3, 25000); stat |= ST_MOTOR; push_stat(2, 400000); break;   // MotorOn
	case 0x08:   // Stop
		reading = 0;
		stat &= ~(ST_READ | ST_SEEK | ST_PLAY);
		push_stat(3, 25000);
		stat &= ~ST_MOTOR;
		push_stat(2, 200000);
		break;
	case 0x09:   // Pause
		push_stat(3, 25000);
		reading = 0;
		stat &= ~(ST_READ | ST_SEEK | ST_PLAY);
		push_stat(2, 70000);
		break;
	case 0x0a:   // Init
		push_stat(3, 25000);
		mode = 0;
		reading = 0;
		stat = reader ? ST_MOTOR : 0;
		push_stat(2, 70000);
		break;
	case 0x0b: case 0x0c:   // Mute, Demute
		muted = cmd == 0x0b;
		if (cd_audio_hw) cd_audio_hw->mute(muted);
		push_stat(3, 25000);
		break;
	case 0x0d: filter_file = params[0]; filter_chan = params[1]; push_stat(3, 25000); break;   // Setfilter
	case 0x0e: mode = params[0]; push_stat(3, 25000); break;      // Setmode
	case 0x0f: { u8 d[5] = { stat, mode, 0, 0, 0 }; push(3, 25000, 5, d); break; }   // Getparam
	case 0x10: push(3, 25000, 8, last_header); break;            // GetlocL
	case 0x11: {   // GetlocP
		u32 a = cur_lba + 150, r = cur_lba;
		u8 d[8] = { 1, 1, bcd(r / 4500), bcd(r / 75 % 60), bcd(r % 75), bcd(a / 4500), bcd(a / 75 % 60), bcd(a % 75) };
		push(3, 25000, 8, d);
		break;
	}
	case 0x13: { u8 d[3] = { stat, 1, 1 }; push(3, 25000, 3, d); break; }   // GetTN
	case 0x14: {   // GetTD
		u32 a = (params[0] == 0) ? disc_sectors + 150 : 150;
		u8 d[3] = { stat, bcd(a / 4500), bcd(a / 75 % 60) };
		push(3, 25000, 3, d);
		break;
	}
	case 0x15: case 0x16:   // SeekL / SeekP
		reading = 0;
		cur_lba = seek_lba;
		seek_pending = 0;
		stat = (stat & ~(ST_READ | ST_PLAY)) | ST_MOTOR;
		push_stat(3, 25000);
		push_stat(2, 100000);
		break;
	case 0x19:   // Test
		if (params[0] == 0x20) { u8 d[4] = { 0x94, 0x09, 0x19, 0xc0 }; push(3, 25000, 4, d); }
		else push_stat(3, 25000);
		break;
	case 0x1a:   // GetID
		if (!reader) { u8 d[2] = { 0x11, 0x80 }; push(5, 25000, 2, d); break; }
		push_stat(3, 25000);
		{ u8 d[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' }; push(2, 30000, 8, d); }
		break;
	case 0x1e: push_stat(3, 25000); push_stat(2, 500000); break;   // ReadTOC
	default: error(0x40); break;
	}
	nparams = 0;
}

static void fire(void)
{
	Resp *r = &queue[0];
	memcpy(resp, r->data, r->len);
	resp_len = r->len;
	resp_pos = 0;
	int_flag = r->irq;
#ifdef PSX_CDLOG
	CDLOG(0x40000000u | int_flag | (u32)int_enable << 8 | (u32)resp_len << 16);
#endif
	if (int_flag & int_enable) psx_irq(PSX_IRQ_CDROM);
	qn--;
	memmove(queue, queue + 1, qn * sizeof(Resp));
	if (qn) q_due = psx_cycles + queue[0].delay;
}

int cdrom_next_event(u32 *when)
{
	int have = 0;
	if (qn && !int_flag) { *when = q_due; have = 1; }
	if (reading && (!have || TIME_BEFORE(read_due, *when))) { *when = read_due; have = 1; }
	return have;
}

void cdrom_tick(void)
{
	if (qn && !int_flag && !TIME_BEFORE(psx_cycles, q_due)) fire();
	if (reading && !TIME_BEFORE(psx_cycles, read_due)) {
		if (int_flag || qn) { read_due = psx_cycles + 2000; return; }   // la CPU aún no atendió la anterior
		int ok;
		{ PROF_BEGIN(); ok = cur_lba < disc_sectors && reader(reader_user, cur_lba, sector); PROF_END(PROF_DISC); }
		STAT_ADD(cd_sectors, 1);
		if (!ok) {
			reading = 0;
			error(0x10);
			return;
		}
		memcpy(last_header, sector + 12, 8);
		// XA-ADPCM activado: los sectores de audio (submodo bit 2) van a la SPU y no a la CPU; con el filtro, solo
		// los del archivo/canal elegidos (los demás se ignoran del todo)
		if ((mode & 0x40) && sector[15] == 2 && (sector[18] & 0x04)) {
			int pass = !(mode & 0x08) || (sector[16] == filter_file && sector[17] == filter_chan);
			if (pass && cd_audio_hw) cd_audio_hw->sector(cur_lba, sector);
			cur_lba++;
			read_due = psx_cycles + sector_time();
			return;
		}
#ifdef PSX_TRACE_IO
		printf("CD INT1 sector %u\n", cur_lba);
#endif
#ifdef PSX_TRACE_PC
		{ extern int trace_live; if (cur_lba == 133136) trace_live = 600; }
#endif
		cur_lba++;
		read_due = psx_cycles + sector_time();
		{ u8 s = stat; push(1, 0, 1, &s); }
		q_due = psx_cycles;
		fire();
	}
}

static void load_data(void)
{
	if (mode & 0x20) {
		memcpy(data_fifo, sector + 12, 0x924);
		data_len = 0x924;
	} else {
		int off = sector[15] == 1 ? 16 : 24;
		memcpy(data_fifo, sector + off, 0x800);
		data_len = 0x800;
	}
	data_pos = 0;
}

static u8 read_data(void)
{
	if (data_pos < data_len) return data_fifo[data_pos++];
	return data_len ? data_fifo[data_len - 8 + (data_pos++ & 7)] : 0;
}

u32 cdrom_dma_word(void)
{
	u32 v = read_data();
	v |= read_data() << 8;
	v |= read_data() << 16;
	v |= (u32)read_data() << 24;
	return v;
}

u32 cdrom_read(u32 reg)
{
	switch (reg & 3) {
	case 0:
		return index_reg | ((nparams == 0) << 3) | ((nparams < 16) << 4) | ((resp_pos < resp_len) << 5) |
		       ((data_pos < data_len) << 6);
	case 1:
		if (resp_pos < resp_len) return resp[resp_pos++];
		return 0;
	case 2: return read_data();
	case 3:
		if (index_reg & 1) return int_flag | 0xe0;
		return int_enable | 0xe0;
	}
	return 0;
}

void cdrom_write(u32 reg, u32 v)
{
#ifdef PSX_TRACE_IO
	if ((reg & 3) == 1 && index_reg == 0) {
		printf("CD cmd %02x", v);
		for (int i = 0; i < nparams; i++) printf(" %02x", params[i]);
		printf("  (mode=%02x lba=%u)\n", mode, cur_lba);
	}
	if ((reg & 3) == 3 && index_reg == 0) printf("CD req %02x (data_pos=%u/%u)\n", v, data_pos, data_len);
#endif
#ifdef PSX_CDLOG_REGS
	CDLOG(0x80000000u | (reg & 3) | (u32)index_reg << 4 | (v & 0xff) << 8);
#endif
	switch (reg & 3) {
	case 0: index_reg = v & 3; break;
	case 1: if (index_reg == 0) command((u8)v); break;
	case 2:
		if (index_reg == 0) { if (nparams < 16) params[nparams++] = (u8)v; }
		else if (index_reg == 1) {
			int_enable = v & 0x1f;
			if (int_flag & int_enable) psx_irq(PSX_IRQ_CDROM);
		}
		break;
	case 3:
		if (index_reg == 0) {
			// pedir datos con el buffer ya cargado no lo reinicia: los juegos leen la cabecera y luego el resto
			if (v & 0x80) { if (data_pos >= data_len) load_data(); }
			else { data_len = data_pos = 0; }
		} else if (index_reg == 1) {
			int_flag &= ~(v & 0x1f);
			if (v & 0x40) nparams = 0;
			if (!int_flag && qn) {
				if (TIME_BEFORE(q_due, psx_cycles + 1000)) q_due = psx_cycles + 1000;
				psx_schedule(q_due);
			}
		}
		break;
	}
}

void cdrom_state(PsxState *s)
{
	STATE(s, index_reg); STATE(s, int_enable); STATE(s, int_flag);
	STATE(s, params); STATE(s, nparams);
	STATE(s, resp); STATE(s, resp_len); STATE(s, resp_pos);
	STATE(s, queue); STATE(s, qn); STATE(s, q_due);
	STATE(s, mode); STATE(s, stat);
	STATE(s, seek_lba); STATE(s, cur_lba);
	STATE(s, seek_pending); STATE(s, reading); STATE(s, read_due);
	STATE(s, sector); STATE(s, last_header);
	STATE(s, data_fifo); STATE(s, data_len); STATE(s, data_pos);
}

// ---- XA-ADPCM: 18 grupos de 128 bytes por sector; en 4 bits, 8 unidades de 28 muestras por grupo ----
int xa_decode(const u8 *raw, s16 *out, XaDecoder *d, int *stereo, int *rate)
{
	static const s32 f0[4] = { 0, 60, 115, 98 }, f1[4] = { 0, 0, -52, -55 };
	u8 coding = raw[19];
	int st = coding & 1, bits8 = (coding >> 4) & 1, n = 0;
	*stereo = st;
	*rate = (coding & 4) ? 18900 : 37800;
	if (raw[15] != 2 || !(raw[18] & 0x04)) return 0;
	for (int g = 0; g < 18; g++) {
		const u8 *grp = raw + 24 + g * 128;
		int units = bits8 ? 4 : 8;
		for (int u = 0; u < units; u++) {
			u8 hdr = grp[4 + u];
			int shift = hdr & 15, filt = (hdr >> 4) & 3, ch = st ? (u & 1) : 0;
			s32 *old = &d->old[ch], *older = &d->older[ch];
			if (shift > 12) shift = 9;
			for (int j = 0; j < 28; j++) {
				s32 s;
				if (bits8) s = (s32)((u32)grp[16 + j * 4 + u] << 24) >> 16;
				else s = (s32)((u32)((grp[16 + j * 4 + u / 2] >> ((u & 1) * 4)) & 15) << 28) >> 16;
				s >>= shift;
				s += (*old * f0[filt] + *older * f1[filt] + 32) >> 6;
				if (s > 32767) s = 32767;
				if (s < -32768) s = -32768;
				*older = *old;
				*old = s;
				// mono: las unidades van seguidas; estéreo: pares L/R intercalados en la salida
				if (st) out[(n / 56 * 28 + j) * 2 + ch] = (s16)s;
				else out[n + j] = (s16)s;
			}
			if (!st || (u & 1)) n += st ? 56 : 28;
		}
	}
	return st ? n / 2 : n;   // muestras por canal
}

// ---- arranque rápido: buscar el ejecutable del juego en el sistema de archivos del CD (ISO9660) ----
// Los datos de usuario de un sector en modo 2 forma 1 empiezan en el byte 24 del sector crudo.
static int iso_read(u32 lba, u8 *out2048)
{
	static u8 raw[2352];
	if (!reader || !reader(reader_user, lba, raw)) return 0;
	memcpy(out2048, raw + 24, 2048);
	return 1;
}

static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// Busca name (sin «;1», mayúsculas) en el directorio [lba, size) y devuelve su extensión
static int iso_find(u32 lba, u32 size, const char *name, int len, u32 *out_lba, u32 *out_size)
{
	static u8 buf[2048];
	for (u32 done = 0; done < size; done += 2048, lba++) {
		if (!iso_read(lba, buf)) return 0;
		for (u32 o = 0; o < 2048 && buf[o];) {
			const u8 *r = buf + o;
			int nl = r[32];
			const char *n = (const char *)r + 33;
			int k = 0;
			while (k < nl && n[k] != ';') k++;
			if (k == len) {
				int i = 0;
				while (i < len && (n[i] & ~0x20) == (name[i] & ~0x20)) i++;   // sin distinguir mayúsculas
				if (i == len) { *out_lba = le32(r + 2); *out_size = le32(r + 10); return 1; }
			}
			o += r[0];
		}
	}
	return 0;
}

// Ruta tipo «\DIR\FILE.EXE»: devuelve su LBA y tamaño
static int iso_path(const char *path, u32 *lba, u32 *size)
{
	static u8 pvd[2048];
	u32 l, s;
	if (!iso_read(16, pvd) || memcmp(pvd + 1, "CD001", 5)) return 0;
	l = le32(pvd + 156 + 2);
	s = le32(pvd + 156 + 10);
	while (*path == 0x5c || *path == '/') path++;   // 0x5c = barra invertida
	while (*path) {
		int len = 0;
		while (path[len] && path[len] != 0x5c && path[len] != '/' && path[len] != ';') len++;
		if (!iso_find(l, s, path, len, &l, &s)) return 0;
		path += len;
		if (*path == ';') break;
		while (*path == 0x5c || *path == '/') path++;
	}
	*lba = l;
	*size = s;
	return 1;
}

// LBA del ejecutable de arranque (BOOT de SYSTEM.CNF, o PSX.EXE)
int cdrom_boot_exe(u32 *lba, u32 *size)
{
	static u8 cnf[2049];
	u32 l, s;
	if (iso_path("SYSTEM.CNF", &l, &s) && iso_read(l, cnf)) {
		char *p;
		cnf[s < 2048 ? s : 2048] = 0;
		for (p = (char *)cnf; *p; p++)
			if ((p == (char *)cnf || p[-1] == '\n' || p[-1] == '\r') && !strncmp(p, "BOOT", 4) && p[4] != '2') {
				char path[128];
				int n = 0;
				p += 4;
				while (*p == ' ' || *p == '\t' || *p == '=') p++;
				if (!strncmp(p, "cdrom:", 6)) p += 6;
				while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != ';' && n < 127) path[n++] = *p++;
				path[n] = 0;
				return iso_path(path, lba, size);
			}
	}
	return iso_path("PSX.EXE", lba, size);
}

int cdrom_read_data(u32 lba, u8 *out2048) { return iso_read(lba, out2048); }

// Estado del CD que deja la BIOS de Sony al saltar al shell: interrupciones activadas y motor en marcha.
// Algunos juegos (Tomb Raider) mandan su primera orden sin activarlas y, si no, esperan 16 s a que venza su plazo.
void cdrom_boot_state(void)
{
	int_enable = 0x1f;
	if (reader) stat |= ST_MOTOR;
}

#ifdef PSX_CDLOG
void cdlog_dma(u32 ch, u32 madr, u32 bcr, u32 chcr)   // pruebas: arranques de DMA en el mismo registro
{
	CDLOG(0x20000000u | ch << 24 | (madr & 0xffffff));
	CDLOG(bcr);
	CDLOG(chcr);
}
#endif
#ifdef PSX_CDLOG
void cdlog_any(u32 v) { CDLOG(v); }
#endif
