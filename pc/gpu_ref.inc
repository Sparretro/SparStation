// Rasterizador original (lento pero simple), incluido por core/gpu.c con -DPSX_GPU_REF para comparar en el PC:
// con la variable de entorno PSX_GPU_REF=1 se dibuja con esto en vez de con los tramos rápidos.
static int gpu_ref = -1, in_check;

static int use_ref(void)
{
	if (gpu_ref < 0) gpu_ref = getenv("PSX_GPU_REF") != NULL;
	return gpu_ref;
}

static void triangle_ref(Vtx *v0, Vtx *v1, Vtx *v2, int textured, int gouraud, int semi_on, int raw)
{
	s32 area;
	int minx, maxx, miny, maxy, b0, b1, b2;
	s32 drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0, dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
	int semi = g.semi;

	area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
	if (area == 0) return;
	if (area < 0) { Vtx *t = v1; v1 = v2; v2 = t; area = -area; }

	minx = v0->x; if (v1->x < minx) minx = v1->x; if (v2->x < minx) minx = v2->x;
	maxx = v0->x; if (v1->x > maxx) maxx = v1->x; if (v2->x > maxx) maxx = v2->x;
	miny = v0->y; if (v1->y < miny) miny = v1->y; if (v2->y < miny) miny = v2->y;
	maxy = v0->y; if (v1->y > maxy) maxy = v1->y; if (v2->y > maxy) maxy = v2->y;
	if (maxx - minx >= 1024 || maxy - miny >= 512) return;
	if (minx < g.da_x1) minx = g.da_x1;
	if (miny < g.da_y1) miny = g.da_y1;
	if (maxx > g.da_x2) maxx = g.da_x2;
	if (maxy > g.da_y2) maxy = g.da_y2;
	if (minx > maxx || miny > maxy) return;

	if (gouraud) {
		grad(v0->r, v1->r, v2->r, v0, v1, v2, area, &drdx, &drdy);
		grad(v0->g, v1->g, v2->g, v0, v1, v2, area, &dgdx, &dgdy);
		grad(v0->b, v1->b, v2->b, v0, v1, v2, area, &dbdx, &dbdy);
	}
	if (textured) {
		grad(v0->u, v1->u, v2->u, v0, v1, v2, area, &dudx, &dudy);
		grad(v0->v, v1->v, v2->v, v0, v1, v2, area, &dvdx, &dvdy);
	}

	b0 = top_left(v1, v2) ? 0 : -1;
	b1 = top_left(v2, v0) ? 0 : -1;
	b2 = top_left(v0, v1) ? 0 : -1;

	{
		s32 w0r = edge(v1, v2, minx, miny), w1r = edge(v2, v0, minx, miny), w2r = edge(v0, v1, minx, miny);
		s32 w0dx = -(v2->y - v1->y), w1dx = -(v0->y - v2->y), w2dx = -(v1->y - v0->y);
		s32 w0dy = v2->x - v1->x, w1dy = v0->x - v2->x, w2dy = v1->x - v0->x;
		int dx0 = minx - v0->x, dy0 = miny - v0->y;
		s32 rr = (v0->r << 16) + drdx * dx0 + drdy * dy0 + 0x8000;
		s32 gr = (v0->g << 16) + dgdx * dx0 + dgdy * dy0 + 0x8000;
		s32 br = (v0->b << 16) + dbdx * dx0 + dbdy * dy0 + 0x8000;
		s32 ur = (v0->u << 16) + dudx * dx0 + dudy * dy0 + 0x8000;
		s32 vr = (v0->v << 16) + dvdx * dx0 + dvdy * dy0 + 0x8000;
		u16 flat = rgb15(v0->r | (v0->g << 8) | (v0->b << 16));

		for (int y = miny; y <= maxy; y++) {
			s32 w0 = w0r, w1 = w1r, w2 = w2r;
			s32 r = rr, gg = gr, b = br, u = ur, v = vr;
			for (int x = minx; x <= maxx; x++) {
				if (((w0 + b0) | (w1 + b1) | (w2 + b2)) >= 0) {
					if (textured) {
						u16 t = texel(u >> 16, v >> 16);
						if (t) {
							if (!raw) t = gouraud ? modulate(t, r >> 16, gg >> 16, b >> 16)
							                      : modulate(t, v0->r, v0->g, v0->b);
							put(x, y, t, semi_on && (t & 0x8000), semi);
						}
					} else if (gouraud) {
						int cr = r >> 16, cg = gg >> 16, cb = b >> 16;
						put(x, y, (u16)((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10)), semi_on, semi);
					} else {
						put(x, y, flat, semi_on, semi);
					}
				}
				w0 += w0dx; w1 += w1dx; w2 += w2dx;
				r += drdx; gg += dgdx; b += dbdx; u += dudx; v += dvdx;
			}
			w0r += w0dy; w1r += w1dy; w2r += w2dy;
			rr += drdy; gr += dgdy; br += dbdy; ur += dudy; vr += dvdy;
		}
	}
}

static void rectangle_ref(void)
{
	u32 cmd = fifo[0] >> 24;
	int tex = cmd & 4, semi_on = cmd & 2, raw = cmd & 1, size = (cmd >> 3) & 3, i = 1;
	u32 col = fifo[0] & 0xffffff, xy = fifo[i++], uv = 0;
	int x0 = sx11(xy) + g.off_x, y0 = sx11(xy >> 16) + g.off_y, w, h;
	int r = col & 0xff, gg = (col >> 8) & 0xff, b = (col >> 16) & 0xff;
	u16 flat = rgb15(col);

	if (tex) uv = fifo[i++];
	if (size == 0) { u32 wh = fifo[i++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; }
	else if (size == 1) w = h = 1;
	else if (size == 2) w = h = 8;
	else w = h = 16;
	if (tex) use_texture(uv >> 16);

	for (int y = 0; y < h; y++) {
		int py = y0 + y;
		if (py < g.da_y1 || py > g.da_y2) continue;
		for (int x = 0; x < w; x++) {
			int px = x0 + x;
			if (px < g.da_x1 || px > g.da_x2) continue;
			if (tex) {
				int tu = (int)(uv & 0xff) + (g.flip_x ? -x : x);
				int tv = (int)((uv >> 8) & 0xff) + (g.flip_y ? -y : y);
				u16 t = texel(tu, tv);
				if (!t) continue;
				if (!raw) t = modulate(t, r, gg, b);
				put(px, py, t, semi_on && (t & 0x8000), g.semi);
			} else put(px, py, flat, semi_on, g.semi);
		}
	}
}

// PSX_GPU_CHECK=1: cada primitivo se dibuja con los dos rasterizadores y se avisa si difieren
static u16 chk_before[512 * 1024], chk_ref[512 * 1024];
static int chk = -1;
static unsigned chk_count, chk_bad;

static int check_on(void)
{
	if (chk < 0) chk = getenv("PSX_GPU_CHECK") ? atoi(getenv("PSX_GPU_CHECK")) : 0;   // valor = desde qué frame
	return chk > 0 && ref_frames >= (unsigned)chk;
}

#define CHECK_BEGIN() memcpy(chk_before, psx_vram, sizeof chk_before)
#define CHECK_REF() do { memcpy(chk_ref, psx_vram, sizeof chk_ref); memcpy(psx_vram, chk_before, sizeof chk_before); } while (0)
static void check_end(const char *what)
{
	if (++chk_count % 20000 == 0) fprintf(stderr, "GPU_CHECK: %u primitivos comparados, %u distintos\n", chk_count, chk_bad);
	for (int i = 0; i < 512 * 1024; i++)
		if (psx_vram[i] != chk_ref[i]) {
			if (chk_bad++ < 20) {
				int n = 0;
				for (int k = i; k < 512 * 1024; k++) n += psx_vram[k] != chk_ref[k];
				fprintf(stderr, "GPU distinta (%s #%u): (%d,%d) ref=%04x nuevo=%04x, %d píxeles; cmd", what, chk_count, i & 1023,
				        i >> 10, chk_ref[i], psx_vram[i], n);
				for (int k = 0; k < fifo_need && k < 12; k++) fprintf(stderr, " %08x", fifo[k]);
				fprintf(stderr, "  tex=%d,%d d%d clut=%d,%d win=%d,%d,%d,%d semi=%d mask=%d%d\n", t_px, t_py, t_depth, t_cx, t_cy,
				        g.tw_mx, g.tw_my, g.tw_ox, g.tw_oy, g.semi, g.set_mask, g.check_mask);
			}
			return;
		}
}
