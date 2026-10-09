// guest.c - test "game": spinning cube, pure CPU float math, SysV ABI (same as real PS5 code).
// Entry gets pointer to HLE table (rdi). Real games get imports resolved by the loader instead.
typedef struct {
  unsigned *fb; int w, h;
  int  (*flip)(void);
  void (*log)(const char *);
} HLE;

static float sn(float x) {
  const float P = 3.14159265f;
  while (x > P) x -= 2 * P;
  while (x < -P) x += 2 * P;
  float s = x * x;
  return x * (1 - s / 6 * (1 - s / 20 * (1 - s / 42 * (1 - s / 72))));
}
static float cs(float x) { return sn(x + 1.57079633f); }

static void line(HLE *h, int x0, int y0, int x1, int y1, unsigned c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    if ((unsigned)x0 < (unsigned)h->w && (unsigned)y0 < (unsigned)h->h) h->fb[y0 * h->w + x0] = c;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

void _start(HLE *h) {
  static const signed char V[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
  static const unsigned char E[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
  h->log("cube guest started (native x86-64)");
  float a = 0;
  for (;;) {
    for (int i = 0; i < h->w * h->h; i++) h->fb[i] = 0;
    float sa = sn(a), ca = cs(a), sb = sn(a * .7f), cb = cs(a * .7f);
    int px[8], py[8];
    for (int i = 0; i < 8; i++) {
      float x = V[i][0], y = V[i][1], z = V[i][2];
      float x1 = x * ca + z * sa, z1 = -x * sa + z * ca;
      float y2 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
      float k = 700 / (z2 + 4);
      px[i] = h->w / 2 + (int)(x1 * k);
      py[i] = h->h / 2 + (int)(y2 * k);
    }
    for (int i = 0; i < 12; i++)
      line(h, px[E[i][0]], py[E[i][0]], px[E[i][1]], py[E[i][1]], 0xFF00FF88);
    a += .03f;
    if (h->flip()) return;
  }
}
