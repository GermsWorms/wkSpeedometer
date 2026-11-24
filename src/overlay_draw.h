/* overlay_draw.h: wkSpeedometer's pure drawing code, with no game calls (native test: tests/draw_test.c).
 *
 * The movement line: its points (tpoint, in land pixels) turned into screen segments (line_segments), then drawn one of
 * two ways: as triangles for wkDirect3D (line_triangles: a quad a segment), or into WA's 8-bit frame as opaque pixels in
 * the nearest palette colours, the dimmer parts thinned with an ordered dither (line_frame).
 *
 * The speedometer's picture (speedo.h's, premultiplied A8R8G8B8) into WA's 8-bit frame (speedo_frame), as wkMapZoom
 * put it there: bright pixels as the nearest palette colour, the dark shadow as a darker version of the pixel under it.
 *
 * The palette tables both need (pal_tables): from a palette of 256 x R, G, B, flags. */
#ifndef WKSPEEDO_OVERLAY_DRAW_H
#define WKSPEEDO_OVERLAY_DRAW_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "speedo.h"

/* a point of a worm's line. turn: the game's turn it was in; brk: not joined to the point before (a jump); own: in the
 * worm's own turn (or right after it, before the next team's); spd: the worm's speed there (px/s, the game's) */
typedef struct { float x, y; int32_t turn; int8_t brk, own; uint16_t spd; } tpoint;

/* a vertex for wkDirect3D_Triangles: screen pixels; straight (not premultiplied) alpha */
typedef struct { float x, y; uint32_t argb; } wkd3d_vtx;

/* where the map is on the screen: screen = org + (world - cam) * zoom; the screen is w x h */
typedef struct { double zoom; int org_x, org_y, cam_x, cam_y, w, h; } line_view;

/* a segment on the screen: its ends, its opacity (0-1) and its speed as a colour index (0-255 over 0 to SPEEDO_VMAX) */
typedef struct { float x0, y0, x1, y1, a; uint8_t c; } line_seg;

#define LINE_HALF 1.5           /* half the line's width, in screen pixels (3 pixels wide at any zoom) */
#define LINE_SKIP 1.5           /* points closer than this (screen pixels, each way) to the last one drawn are passed over */

/* the segments of a worm's line (ported from wkMapZoom's trail_segments). Points of turn `limit` or later are left out
 * (normally the turn now: the line ends where this turn began). The points of turn hl the worm made itself are bright,
 * fading in toward where it stopped (opacity 0.5 to 1); with all, every other point is dimmer (0.35), else left out.
 * Segments wholly off the screen are left out. out: room for n segments. Returns how many. */
static int line_segments(const tpoint *pt, int n, int32_t limit, int32_t hl, int all, const line_view *v, line_seg *out) {
    double lx = 0.0, ly = 0.0, ls = 0.0;
    int i, first = -1, n_hl = 0, have = 0, count = 0;
    const double m = LINE_HALF + 1.0;
    for (i = 0; i < n; i++) if (pt[i].own && pt[i].turn == hl) { if (first < 0) first = i; n_hl++; }
    for (i = 0; i < n; i++) {
        const tpoint *b = &pt[i];
        const double bx = v->org_x + (b->x - v->cam_x) * v->zoom, by = v->org_y + (b->y - v->cam_y) * v->zoom;
        double alpha;
        if (b->turn >= limit) { have = 0; continue; }
        if (b->own && b->turn == hl) {
            alpha = 0.5 + 0.5 * (i - first) / (n_hl > 1 ? n_hl - 1 : 1);
            if (alpha > 1.0) alpha = 1.0;   /* (other points between the bright ones would push it past 1) */
        } else if (!all) { have = 0; continue; }
        else alpha = 0.35;
        if (!have || b->brk) { lx = bx; ly = by; ls = b->spd; have = 1; continue; }
        if (fabs(bx - lx) < LINE_SKIP && fabs(by - ly) < LINE_SKIP && i + 1 < n && !pt[i + 1].brk) continue;
        if (!((lx < -m && bx < -m) || (ly < -m && by < -m) || (lx > v->w + m && bx > v->w + m) || (ly > v->h + m && by > v->h + m))) {
            const double sp = (ls + b->spd) / 2.0;
            line_seg *s = &out[count++];
            s->x0 = (float)lx; s->y0 = (float)ly; s->x1 = (float)bx; s->y1 = (float)by; s->a = (float)alpha;
            s->c = (uint8_t)(sp >= SPEEDO_VMAX ? 255 : (int)(sp * 255.0 / SPEEDO_VMAX + 0.5));
        }
        lx = bx; ly = by; ls = b->spd;
    }
    return count;
}

/* the line's colours by colour index (0xRRGGBB): the speedometer's (blue slow, red from red_from on), or all red */
static void line_colours(uint32_t lut[256], int by_speed, double red_from) {
    int k;
    for (k = 0; k < 256; k++) {
        if (by_speed) {
            const speedo_rgb c = speedo_colour(k * SPEEDO_VMAX / 255.0, red_from);
            lut[k] = (uint32_t)(c.r * 255.0f + 0.5f) << 16 | (uint32_t)(c.g * 255.0f + 0.5f) << 8 | (uint32_t)(c.b * 255.0f + 0.5f);
        } else lut[k] = 235u << 16 | 30u << 8 | 30u;
    }
}

/* the segments as triangles (a quad of two triangles a segment, LINE_HALF each side), at most max vertices: when they
 * don't all fit, the oldest segments are left out (the newest, the bright last turn, are at the end). Returns the
 * vertex count (a multiple of 6) */
static int line_triangles(const line_seg *s, int n, const uint32_t lut[256], wkd3d_vtx *out, int max) {
    int i, k = 0, start = 0;
    if (max < 0) max = 0;
    if (n * 6 > max) start = n - max / 6;
    for (i = start; i < n; i++) {
        const float dx = s[i].x1 - s[i].x0, dy = s[i].y1 - s[i].y0, len = sqrtf(dx * dx + dy * dy);
        float nx, ny, a = s[i].a;
        uint32_t c;
        if (len < 1e-4f) continue;
        if (k + 6 > max) break;
        nx = -dy / len * (float)LINE_HALF; ny = dx / len * (float)LINE_HALF;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        c = (uint32_t)(a * 255.0f + 0.5f) << 24 | (lut[s[i].c] & 0xffffffu);
        out[k].x = s[i].x0 + nx; out[k].y = s[i].y0 + ny; out[k].argb = c; k++;
        out[k].x = s[i].x1 + nx; out[k].y = s[i].y1 + ny; out[k].argb = c; k++;
        out[k].x = s[i].x1 - nx; out[k].y = s[i].y1 - ny; out[k].argb = c; k++;
        out[k].x = s[i].x0 + nx; out[k].y = s[i].y0 + ny; out[k].argb = c; k++;
        out[k].x = s[i].x1 - nx; out[k].y = s[i].y1 - ny; out[k].argb = c; k++;
        out[k].x = s[i].x0 - nx; out[k].y = s[i].y0 - ny; out[k].argb = c; k++;
    }
    return k;
}

static const uint8_t overlay_bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};   /* (4x4 ordered dither) */

/* the segments into an 8-bit frame (w x h, rows stride bytes apart): each pixel whose middle is within LINE_HALF of a
 * segment set to the segment's palette colour (idx: colour index to palette index); a segment's opacity thins it with
 * the ordered dither (1: every pixel; 0.35: about 6 in 16). Returns how many pixels were set */
static long line_frame(const line_seg *s, int n, const uint8_t idx[256], uint8_t *frame, int stride, int w, int h) {
    const double half = LINE_HALF, half2 = half * half;
    long set = 0;
    int i;
    if (!frame || w <= 0 || h <= 0) return 0;
    for (i = 0; i < n; i++) {
        const double x0 = s[i].x0, y0 = s[i].y0, dx = s[i].x1 - x0, dy = s[i].y1 - y0, len2 = dx * dx + dy * dy;
        const int level = (int)(s[i].a * 16.0f + 0.5f);
        const uint8_t colour = idx[s[i].c];
        int X, Y, xa = (int)floor((x0 < s[i].x1 ? x0 : s[i].x1) - half), xb = (int)ceil((x0 > s[i].x1 ? x0 : s[i].x1) + half);
        int ya = (int)floor((y0 < s[i].y1 ? y0 : s[i].y1) - half), yb = (int)ceil((y0 > s[i].y1 ? y0 : s[i].y1) + half);
        if (level <= 0) continue;
        if (xa < 0) xa = 0;
        if (ya < 0) ya = 0;
        if (xb > w) xb = w;
        if (yb > h) yb = h;
        for (Y = ya; Y < yb; Y++) {
            uint8_t *row = frame + (size_t)Y * stride;
            for (X = xa; X < xb; X++) {
                const double px = X + 0.5 - x0, py = Y + 0.5 - y0;
                double t = len2 > 0.0 ? (px * dx + py * dy) / len2 : 0.0, ex, ey;
                if (overlay_bayer[(X & 3) + (Y & 3) * 4] >= level) continue;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                ex = px - t * dx; ey = py - t * dy;
                if (ex * ex + ey * ey > half2) continue;
                row[X] = colour;
                set++;
            }
        }
    }
    return set;
}

/* the palette colour nearest a colour (as wkMapZoom's pal_nearest: green counts most, blue least) */
static int pal_nearest(const uint8_t *pal, int r, int g, int b) {
    int k, best = 0;
    long bd = 0x7fffffff;
    for (k = 0; k < 256; k++) {
        long dr = pal[4 * k] - r, dg = pal[4 * k + 1] - g, db = pal[4 * k + 2] - b, d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

/* from a palette (256 x R, G, B, flags): the nearest colour for each 4-bit R, G, B (lut12), and each colour's darker one
 * (dark: 30% as bright; the dial's shadow) */
static void pal_tables(const uint8_t *pal, uint8_t lut12[4096], uint8_t dark[256]) {
    int k;
    for (k = 0; k < 4096; k++) lut12[k] = (uint8_t)pal_nearest(pal, (k >> 8) * 17, ((k >> 4) & 15) * 17, (k & 15) * 17);
    for (k = 0; k < 256; k++) dark[k] = (uint8_t)pal_nearest(pal, pal[4 * k] * 30 / 100, pal[4 * k + 1] * 30 / 100, pal[4 * k + 2] * 30 / 100);
}

/* the speedometer's picture (w x h, premultiplied A8R8G8B8, rows w apart) into an 8-bit frame (fw x fh, rows stride
 * bytes apart) at (x0, y0), as wkMapZoom's draw_speedo did: pixels under alpha 100 left alone; the dark shadow darkens
 * the pixel under it; other pixels from alpha 150 on become the nearest palette colour. Returns how many pixels changed */
static long speedo_frame(const uint32_t *img, int w, int h, int x0, int y0, uint8_t *frame, int stride, int fw, int fh,
                         const uint8_t lut12[4096], const uint8_t dark[256]) {
    long set = 0;
    int x, y;
    if (!img || !frame) return 0;
    for (y = 0; y < h; y++) {
        uint8_t *row;
        if (y0 + y < 0 || y0 + y >= fh) continue;
        row = frame + (size_t)(y0 + y) * stride;
        for (x = 0; x < w; x++) {
            const uint32_t c = img[y * w + x], a = c >> 24;
            uint32_t r, g, b;
            if (a < 100 || x0 + x < 0 || x0 + x >= fw) continue;
            r = ((c >> 16) & 0xff) * 255 / a; g = ((c >> 8) & 0xff) * 255 / a; b = (c & 0xff) * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (r < 40 && g < 40 && b < 40) { row[x0 + x] = dark[row[x0 + x]]; set++; }
            else if (a >= 150) { row[x0 + x] = lut12[(r >> 4) << 8 | (g >> 4) << 4 | b >> 4]; set++; }
        }
    }
    return set;
}
#endif
