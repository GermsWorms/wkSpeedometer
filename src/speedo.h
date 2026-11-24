/* speedo.h: the speedometer as a picture (premultiplied A8R8G8B8): a digital dial, or the number alone. Copied from
 * wkMapZoom 4.3 beta 2. GDI for the small text only; the rest drawn with anti-aliased distances (each pixel's distance
 * to a shape's edge). No game calls (native test: tests/speedo_test.c, which also writes the pictures out to look at).
 *
 * The look (a digital car gauge): see-through (no face, the map shows behind), a ring of LED segments over 270 degrees
 * (0 at the lower left, clockwise over the top, to SPEEDO_VMAX at the lower right) lit up to the speed, blue when slow
 * through green and yellow to red at the red line (RedFrom) and red past it, the unlit ones faint so the scale shows;
 * the speed in seven-segment digits in its colour (faint 8s behind, as on a real display); the turn's best as one lit
 * segment (peak hold), its average as a white mark outside the ring and a line below; WA's top speed marked MAX. A dark
 * shadow behind what is lit keeps it readable over a bright map.
 *
 * Speeds: land pixels a second. WA moves its objects 50 times a second and caps a worm's speed at 32 px a move each way
 * (1600 px/s straight down or across, 32 x sqrt 2 x 50 = 2263 px/s diagonally); fast rope replays peak at 32-36 px a
 * move. */
#ifndef WKSPEEDO_SPEEDO_H
#define WKSPEEDO_SPEEDO_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SPEEDO_VMAX 2400.0
#define SPEEDO_CAP 2263.0
#define SPEEDO_A0 225.0                 /* degrees (y up): 0 px/s here ... */
#define SPEEDO_SWEEP 270.0              /* ... SPEEDO_VMAX this far clockwise */
#define SPEEDO_SEGS 40                  /* the ring's segments (60 px/s each) */
#define SPEEDO_PI 3.14159265358979

typedef struct { float r, g, b; } speedo_rgb;

/* the speed's colour: blue at rest, through cyan, green, yellow and orange, red from red_from on */
static speedo_rgb speedo_colour(double v, double red_from) {
    static const float k[6][4] = {   /* t, r, g, b */
        {0.00f, 0.15f, 0.45f, 1.00f}, {0.30f, 0.00f, 0.82f, 1.00f}, {0.55f, 0.20f, 0.95f, 0.30f},
        {0.75f, 1.00f, 0.88f, 0.10f}, {0.90f, 1.00f, 0.52f, 0.05f}, {1.00f, 1.00f, 0.13f, 0.10f}};
    speedo_rgb c;
    double t = red_from > 1.0 ? v / red_from : v / SPEEDO_CAP;
    int i;
    if (t < 0.0) t = 0.0;
    if (t >= 1.0) { c.r = k[5][1]; c.g = k[5][2]; c.b = k[5][3]; return c; }
    for (i = 0; i < 5 && t > k[i + 1][0]; i++) {}
    {
        const float f = (float)((t - k[i][0]) / (k[i + 1][0] - k[i][0]));
        c.r = k[i][1] + (k[i + 1][1] - k[i][1]) * f;
        c.g = k[i][2] + (k[i + 1][2] - k[i][2]) * f;
        c.b = k[i][3] + (k[i + 1][3] - k[i][3]) * f;
    }
    return c;
}

/* ---- drawing into a premultiplied float picture (4 floats a pixel: r, g, b, a) ---------------------------------- */
typedef struct { float *p; int w, h; } speedo_canvas;

static float speedo_cov(float d) { return d >= 0.5f ? 0.0f : d <= -0.5f ? 1.0f : 0.5f - d; }   /* (d: px outside) */

static void speedo_over(speedo_canvas *c, int x, int y, speedo_rgb col, float a) {
    float *q;
    if (a <= 0.004f || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    if (a > 1.0f) a = 1.0f;
    q = c->p + ((size_t)y * c->w + x) * 4;
    q[0] = col.r * a + q[0] * (1.0f - a);
    q[1] = col.g * a + q[1] * (1.0f - a);
    q[2] = col.b * a + q[2] * (1.0f - a);
    q[3] = a + q[3] * (1.0f - a);
}

/* a line from (ax, ay) to (bx, by), r thick (half), round ends; with a soft glow of glow px around it (0: none) */
static void speedo_line(speedo_canvas *c, float ax, float ay, float bx, float by, float r, speedo_rgb col, float a,
                        float glow, float ga) {
    const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, m = r + glow * 3.0f + 1.0f;
    int x, y;
    const int x0 = (int)floorf((ax < bx ? ax : bx) - m), x1 = (int)ceilf((ax > bx ? ax : bx) + m);
    const int y0 = (int)floorf((ay < by ? ay : by) - m), y1 = (int)ceilf((ay > by ? ay : by) + m);
    for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) {
        const float px = x + 0.5f - ax, py = y + 0.5f - ay;
        float h = l2 > 0.0f ? (px * dx + py * dy) / l2 : 0.0f, ex, ey, d;
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        ex = px - dx * h; ey = py - dy * h;
        d = sqrtf(ex * ex + ey * ey) - r;
        if (glow > 0.0f && d > 0.0f) speedo_over(c, x, y, col, ga * expf(-d / glow));
        speedo_over(c, x, y, col, a * speedo_cov(d));
    }
}

/* small text centred on (cx, cy), px tall (GDI, anti-aliased: its grey is the coverage), a dark shadow behind */
static void speedo_text(speedo_canvas *c, const char *s, float cx, float cy, int px, speedo_rgb col, float a) {
    static const speedo_rgb black = {0, 0, 0};
    BITMAPINFO bi;
    uint32_t *bits = NULL;
    HDC dc;
    HBITMAP bmp, old_bmp;
    HFONT font, old_font;
    SIZE sz;
    int tw, th, x, y, n = (int)strlen(s), ox, oy, i, j;
    if (px < 6 || !n) return;
    dc = CreateCompatibleDC(NULL);
    if (!dc) return;
    font = CreateFontA(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    old_font = (HFONT)SelectObject(dc, font);
    GetTextExtentPoint32A(dc, s, n, &sz);
    tw = sz.cx + 4; th = sz.cy + 2;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp && bits) {
        old_bmp = (HBITMAP)SelectObject(dc, bmp);
        memset(bits, 0, (size_t)tw * th * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 2, 1, s, n);
        GdiFlush();
        ox = (int)floorf(cx - tw / 2.0f + 0.5f); oy = (int)floorf(cy - th / 2.0f + 0.5f);
        for (y = 0; y < th; y++) for (x = 0; x < tw; x++) {   /* (the shadow: the text spread a pixel each way) */
            float k = 0.0f;
            for (j = -1; j <= 1; j++) for (i = -1; i <= 1; i++) {
                const int xx = x + i, yy = y + j;
                if (xx >= 0 && yy >= 0 && xx < tw && yy < th) {
                    const float kk = (float)((bits[yy * tw + xx] >> 8) & 0xff) / 255.0f;
                    if (kk > k) k = kk;
                }
            }
            if (k > 0.0f) speedo_over(c, ox + x, oy + y, black, 0.6f * a * k);
        }
        for (y = 0; y < th; y++) for (x = 0; x < tw; x++) {
            const float k = (float)((bits[y * tw + x] >> 8) & 0xff) / 255.0f;
            if (k > 0.0f) speedo_over(c, ox + x, oy + y, col, a * k);
        }
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
}

/* ---- seven-segment digits ------------------------------------------------------------------------------------------ */
/* digit n (0-9; -1: none lit) h px tall with its top left at (x, y), slanted a little; its segments lit in col with a
 * glow and a shadow, the unlit ones faint in col */
static void speedo_digit(speedo_canvas *c, float x, float y, float h, int n, speedo_rgb col, float a) {
    static const unsigned char segs[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};   /* gfedcba */
    static const speedo_rgb black = {0, 0, 0};
    const float w = h * 0.52f, t = h * 0.065f, m = t * 1.6f, sk = 0.10f;
    /* the segments as lines (x0, y0, x1, y1) in the digit's box: a top, b upper right, c lower right, d bottom, e lower
     * left, f upper left, g middle */
    const float L[7][4] = {
        {m, t, w - m, t}, {w - t, m, w - t, h / 2 - m * 0.6f}, {w - t, h / 2 + m * 0.6f, w - t, h - m},
        {m, h - t, w - m, h - t}, {t, h / 2 + m * 0.6f, t, h - m}, {t, m, t, h / 2 - m * 0.6f}, {m, h / 2, w - m, h / 2}};
    const unsigned lit = n >= 0 && n <= 9 ? segs[n] : 0;
    int k, pass;
    for (pass = 0; pass < 3; pass++)        /* the shadow, the faint ones, the lit ones */
        for (k = 0; k < 7; k++) {
            const int on = (lit >> k) & 1;
            const float x0 = x + L[k][0] + (h - L[k][1]) * sk, y0 = y + L[k][1], x1 = x + L[k][2] + (h - L[k][3]) * sk, y1 = y + L[k][3];
            if (pass == 0 && on) speedo_line(c, x0, y0, x1, y1, t + h * 0.035f, black, 0.55f * a, 0.0f, 0.0f);
            else if (pass == 1 && !on) speedo_line(c, x0, y0, x1, y1, t, col, 0.10f * a, 0.0f, 0.0f);
            else if (pass == 2 && on) speedo_line(c, x0, y0, x1, y1, t, col, a, h * 0.05f, 0.35f * a);
        }
}

/* the speed in 4 digits (blank leading ones), centred on (cx, cy), h px tall; its width */
static float speedo_digits(speedo_canvas *c, float cx, float cy, float h, int v, speedo_rgb col, float a) {
    const float w = h * 0.52f, gap = h * 0.16f, total = 4 * w + 3 * gap + h * 0.10f;
    int k, d[4];
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    d[0] = v / 1000; d[1] = v / 100 % 10; d[2] = v / 10 % 10; d[3] = v % 10;
    if (v < 1000) d[0] = -1;
    if (v < 100) d[1] = -1;
    if (v < 10) d[2] = -1;
    for (k = 0; k < 4; k++) speedo_digit(c, cx - total / 2 + k * (w + gap), cy - h / 2, h, d[k], col, a);
    return total;
}

/* ---- the dial ---------------------------------------------------------------------------------------------------- */
/* What doesn't move is drawn once a size and red line (the unlit ring, the numbers, MAX, px/s); the ring's pixels keep
 * their segment and what they look like lit (each frame only picks); the average's line is drawn when it changes. A
 * frame of the dial is then a copy, the lit ring, the marks and four digits (well under a millisecond). */
typedef struct {
    int d;                              /* the picture: d x d */
    double red_from;
    float *f;                           /* this frame's picture (premultiplied floats) */
    float *stat;                        /* what doesn't move */
    int ring_n, *ring_i;                /* the ring's pixels: where, their segment, them lit (and as the best) */
    unsigned char *ring_k;
    float *ring_lit, *ring_peak;
    int avg_v, avg_w, avg_h;            /* the average's line ("avg 812"): for which value, its picture */
    float *avg_img;
} speedo_dial;

static void speedo_dial_free(speedo_dial *s) {
    free(s->f); free(s->stat); free(s->ring_i); free(s->ring_k); free(s->ring_lit); free(s->ring_peak); free(s->avg_img);
    memset(s, 0, sizeof(*s));
}

/* a value's point on the dial at radius r */
static void speedo_at(float cx, float cy, double v, float r, float *x, float *y) {
    const double a = (SPEEDO_A0 - SPEEDO_SWEEP * v / SPEEDO_VMAX) * SPEEDO_PI / 180.0;
    *x = cx + r * (float)cos(a);
    *y = cy - r * (float)sin(a);
}

/* one premultiplied float pixel over another */
static void speedo_px_over(float *dst, const float *src) {
    const float k = 1.0f - src[3];
    dst[0] = src[0] + dst[0] * k; dst[1] = src[1] + dst[1] * k; dst[2] = src[2] + dst[2] * k; dst[3] = src[3] + dst[3] * k;
}

/* the parts that don't move, and the ring's pixels; 0: no memory */
static int speedo_dial_setup(speedo_dial *s, int d, double red_from) {
    static const speedo_rgb white = {1, 1, 1}, black = {0, 0, 0}, grey = {0.80f, 0.83f, 0.90f}, red = {1.0f, 0.13f, 0.10f};
    const float u = d / 100.0f, cx = d / 2.0f, cy = d / 2.0f, r_out = d / 2.0f - 4.5f * u, r_in = r_out - 8.5f * u;
    const double step = SPEEDO_SWEEP / SPEEDO_SEGS, gap = 1.7;       /* (degrees: a segment, and the dark gap between) */
    speedo_canvas c;
    int x, y, i, n = 0;
    char t[16];
    if (s->stat && s->d == d && s->red_from == red_from) return 1;
    speedo_dial_free(s);
    s->f = (float *)malloc(sizeof(float) * 4 * (size_t)d * d);
    s->stat = (float *)calloc((size_t)d * d * 4, sizeof(float));
    s->ring_i = (int *)malloc(sizeof(int) * (size_t)d * d);
    s->ring_k = (unsigned char *)malloc((size_t)d * d);
    s->ring_lit = (float *)malloc(sizeof(float) * 4 * (size_t)d * d);
    s->ring_peak = (float *)malloc(sizeof(float) * 4 * (size_t)d * d);
    if (!s->f || !s->stat || !s->ring_i || !s->ring_k || !s->ring_lit || !s->ring_peak) { speedo_dial_free(s); return 0; }
    s->d = d; s->red_from = red_from; s->avg_v = -1;
    c.p = s->stat; c.w = d; c.h = d;
    /* the ring: each pixel's nearest segment (its distance in px: across the ring, or along it to the segment's end) */
    for (y = 0; y < d; y++) for (x = 0; x < d; x++) {
        const float px = x + 0.5f - cx, py = cy - (y + 0.5f), r = sqrtf(px * px + py * py);
        double del = SPEEDO_A0 - atan2(py, px) * 180.0 / SPEEDO_PI, mid;
        int k;
        float dr, da, dist, a0, a1, lit[4] = {0, 0, 0, 0}, pk[4] = {0, 0, 0, 0};
        speedo_rgb col;
        speedo_canvas one;
        if (r < r_in - 8.0f * u || r > r_out + 8.0f * u) continue;
        while (del < -45.0) del += 360.0;
        while (del >= 315.0) del -= 360.0;
        k = (int)floor(del / step);
        if (k < 0) k = 0;
        if (k >= SPEEDO_SEGS) k = SPEEDO_SEGS - 1;
        a0 = (float)(k * step + gap / 2); a1 = (float)((k + 1) * step - gap / 2);
        dr = r > r_out ? r - r_out : r_in - r;
        da = (float)((del < a0 ? a0 - del : del > a1 ? del - a1 : -(del - a0 < a1 - del ? del - a0 : a1 - del)) * SPEEDO_PI / 180.0 * r);
        dist = dr > da ? dr : da;
        mid = (k + 0.5) * (SPEEDO_VMAX / SPEEDO_SEGS);
        col = speedo_colour(mid, red_from);
        speedo_over(&c, x, y, col, 0.20f * speedo_cov(dist));     /* (unlit: faint) */
        one.w = one.h = 1;
        one.p = lit;                                               /* lit: a shadow, a glow and the segment */
        speedo_over(&one, 0, 0, black, 0.5f * speedo_cov(dist - 1.6f * u));
        if (dist > 0.0f) speedo_over(&one, 0, 0, col, 0.45f * expf(-dist / (2.2f * u)));
        memcpy(pk, lit, sizeof(pk));
        speedo_over(&one, 0, 0, col, speedo_cov(dist));
        one.p = pk;                                                /* the best: the same, a little less bright */
        speedo_over(&one, 0, 0, col, 0.8f * speedo_cov(dist));
        if (lit[3] <= 0.004f) continue;
        s->ring_i[n] = y * d + x; s->ring_k[n] = (unsigned char)k;
        memcpy(s->ring_lit + 4 * (size_t)n, lit, sizeof(lit)); memcpy(s->ring_peak + 4 * (size_t)n, pk, sizeof(pk));
        n++;
    }
    s->ring_n = n;
    for (i = 0; i <= (int)SPEEDO_VMAX; i += 400) {                  /* the numbers, in hundreds */
        float lx, ly;
        snprintf(t, sizeof(t), "%d", i / 100);
        speedo_at(cx, cy, i, r_in - 7.5f * u, &lx, &ly);
        speedo_text(&c, t, lx, ly, (int)(8.5f * u + 0.5f), red_from > 0.0 && i >= red_from ? red : grey, 0.9f);
    }
    {   /* WA's top speed: a white tick across the ring's outside, and MAX */
        float x0, y0, x1, y1;
        speedo_at(cx, cy, SPEEDO_CAP, r_out + 0.5f * u, &x0, &y0);
        speedo_at(cx, cy, SPEEDO_CAP, r_out + 4.0f * u, &x1, &y1);
        speedo_line(&c, x0, y0, x1, y1, 1.4f * u, black, 0.6f, 0.0f, 0.0f);
        speedo_line(&c, x0, y0, x1, y1, 0.8f * u, white, 1.0f, 0.0f, 0.0f);
        speedo_text(&c, "MAX", cx + 0.37f * d, cy + 0.44f * d, (int)(6.5f * u + 0.5f), white, 0.85f);
    }
    speedo_text(&c, "px/s", cx, cy + 0.12f * d, (int)(8.0f * u + 0.5f), grey, 0.95f);
    return 1;
}

/* premultiplied floats to premultiplied A8R8G8B8 bytes */
static void speedo_out(const float *f, int w, int h, uint32_t *out, int pitch) {
    int x, y;
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * pitch);
        for (x = 0; x < w; x++) {
            const float *q = f + ((size_t)y * w + x) * 4;
            const uint32_t a = (uint32_t)(q[3] * 255.0f + 0.5f), r = (uint32_t)(q[0] * 255.0f + 0.5f),
                           g = (uint32_t)(q[1] * 255.0f + 0.5f), b = (uint32_t)(q[2] * 255.0f + 0.5f);
            row[x] = (a > 255 ? 255u : a) << 24 | (r > 255 ? 255u : r) << 16 | (g > 255 ? 255u : g) << 8 | (b > 255 ? 255u : b);
        }
    }
}

/* the dial (d x d, premultiplied A8R8G8B8, rows pitch bytes apart): the ring lit up to v_ring, the digits showing
 * v_digits (px/s), the turn's best and average (0: none); red_from: where it goes red. 0: no memory */
static int speedo_dial_paint(speedo_dial *s, int d, double red_from, double v_ring, double v_digits, double best, double avg,
                             uint32_t *out, int pitch) {
    static const speedo_rgb white = {1, 1, 1}, black = {0, 0, 0};
    const float u = d / 100.0f, cx = d / 2.0f, cy = d / 2.0f, r_out = d / 2.0f - 4.5f * u;
    const double seg_v = SPEEDO_VMAX / SPEEDO_SEGS;
    const int peak = best > 0.0 ? (int)(best / seg_v) : -1;
    speedo_canvas c;
    int i;
    if (!speedo_dial_setup(s, d, red_from)) return 0;
    memcpy(s->f, s->stat, sizeof(float) * 4 * (size_t)d * d);
    c.p = s->f; c.w = d; c.h = d;
    for (i = 0; i < s->ring_n; i++) {                               /* the ring lit up to the speed, and the best */
        const int k = s->ring_k[i], on = k * seg_v < v_ring && v_ring > 1.0;
        if (on) speedo_px_over(s->f + 4 * (size_t)s->ring_i[i], s->ring_lit + 4 * (size_t)i);
        else if (k == peak) speedo_px_over(s->f + 4 * (size_t)s->ring_i[i], s->ring_peak + 4 * (size_t)i);
    }
    if (avg > 0.0) {                                                /* the turn's average: a white mark, and its line */
        float x0, y0, x1, y1;
        const double va = avg > SPEEDO_VMAX ? SPEEDO_VMAX : avg;
        const int av = (int)(avg + 0.5);
        speedo_at(cx, cy, va, r_out + 0.8f * u, &x0, &y0);
        speedo_at(cx, cy, va, r_out + 2.8f * u, &x1, &y1);   /* (with its shadow, inside the picture) */
        speedo_line(&c, x0, y0, x1, y1, 1.6f * u, black, 0.6f, 0.0f, 0.0f);
        speedo_line(&c, x0, y0, x1, y1, 1.0f * u, white, 1.0f, 0.0f, 0.0f);
        if (av != s->avg_v || !s->avg_img) {                        /* (GDI only when it changes) */
            char t[24];
            speedo_canvas a;
            s->avg_w = d; s->avg_h = (int)(12.0f * u) + 4;
            free(s->avg_img);
            s->avg_img = (float *)calloc((size_t)s->avg_w * s->avg_h * 4, sizeof(float));
            s->avg_v = -1;
            if (s->avg_img) {
                a.p = s->avg_img; a.w = s->avg_w; a.h = s->avg_h;
                snprintf(t, sizeof(t), "avg %d", av);
                speedo_text(&a, t, s->avg_w / 2.0f, s->avg_h / 2.0f, (int)(8.0f * u + 0.5f), white, 0.95f);
                s->avg_v = av;
            }
        }
        if (s->avg_img) {
            const int oy = (int)floorf(cy + 0.36f * d - s->avg_h / 2.0f + 0.5f);
            int x, y;
            for (y = 0; y < s->avg_h; y++) {
                if (oy + y < 0 || oy + y >= d) continue;
                for (x = 0; x < s->avg_w && x < d; x++)
                    speedo_px_over(s->f + 4 * ((size_t)(oy + y) * d + x), s->avg_img + 4 * ((size_t)y * s->avg_w + x));
            }
        }
    }
    speedo_digits(&c, cx, cy - 0.02f * d, 0.165f * d, (int)(v_digits + 0.5), speedo_colour(v_digits, red_from), 1.0f);
    speedo_out(s->f, d, d, out, pitch);
    return 1;
}

/* ---- the number alone ---------------------------------------------------------------------------------------------- */
typedef struct {
    int h, w, hh;                       /* the digits' height; the picture's size */
    float *f, *stat;                    /* this frame's picture; what doesn't move ("px/s") */
} speedo_number;

static void speedo_number_free(speedo_number *s) { free(s->f); free(s->stat); memset(s, 0, sizeof(*s)); }

/* its size for digits h px tall: w x hh */
static int speedo_number_top(int h) { return 2 + (int)ceilf(0.12f * h); }   /* (room above the digits: their glow) */
static void speedo_number_size(int h, int *w, int *hh) { *w = h * 3 + h / 2 + 4; *hh = h + h * 3 / 4 + 2 + speedo_number_top(h); }

/* the speed in seven-segment digits (its colour) and "px/s" under them, see-through (speedo_number_size) */
static int speedo_number_paint(speedo_number *s, int h, double v, double red_from, uint32_t *out, int pitch) {
    static const speedo_rgb grey = {0.80f, 0.83f, 0.90f};
    speedo_canvas c;
    if (!s->stat || s->h != h) {
        speedo_number_free(s);
        speedo_number_size(h, &s->w, &s->hh);
        s->f = (float *)malloc(sizeof(float) * 4 * (size_t)s->w * s->hh);
        s->stat = (float *)calloc((size_t)s->w * s->hh * 4, sizeof(float));
        if (!s->f || !s->stat) { speedo_number_free(s); return 0; }
        s->h = h;
        c.p = s->stat; c.w = s->w; c.h = s->hh;
        speedo_text(&c, "px/s", s->w / 2.0f, (float)speedo_number_top(h) + h + h * 0.38f, h * 9 / 20, grey, 0.95f);
    }
    memcpy(s->f, s->stat, sizeof(float) * 4 * (size_t)s->w * s->hh);
    c.p = s->f; c.w = s->w; c.h = s->hh;
    speedo_digits(&c, s->w / 2.0f, (float)speedo_number_top(h) + h / 2.0f, (float)h, (int)(v + 0.5), speedo_colour(v, red_from), 1.0f);
    speedo_out(s->f, s->w, s->hh, out, pitch);
    return 1;
}
#endif
