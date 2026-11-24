/* wkSpeedometer's own drawing code (overlay_draw.h): the movement line's segments and triangles, the line and the
 * speedometer drawn into a fake 8-bit frame with a fake palette (guard bytes around the frame show any overrun). */
#include <stdio.h>
#include <stdlib.h>
#include "../src/overlay_draw.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define GUARD 4096
#define FW 320
#define FH 200
static uint8_t frame_mem[GUARD + FW * FH + GUARD];
static uint8_t *const frame = frame_mem + GUARD;

static void frame_clear(uint8_t v) {
    memset(frame_mem, 0xA5, sizeof(frame_mem));
    memset(frame, v, FW * FH);
}
static int guards_ok(void) {
    int k;
    for (k = 0; k < GUARD; k++) if (frame_mem[k] != 0xA5 || frame[FW * FH + k] != 0xA5) return 0;
    return 1;
}

static tpoint pt(float x, float y, int turn, int brk, int own, int spd) {
    tpoint p;
    p.x = x; p.y = y; p.turn = turn; p.brk = (int8_t)brk; p.own = (int8_t)own; p.spd = (uint16_t)spd;
    return p;
}

/* a palette: index k is a grey of k for k < 128, then 128 saturated colours spread around */
static void make_palette(uint8_t pal[1024]) {
    int k;
    for (k = 0; k < 256; k++) {
        if (k < 128) { pal[4 * k] = pal[4 * k + 1] = pal[4 * k + 2] = (uint8_t)(k * 2); }
        else {
            const int j = k - 128;
            pal[4 * k] = (uint8_t)((j & 3) * 85); pal[4 * k + 1] = (uint8_t)(((j >> 2) & 3) * 85); pal[4 * k + 2] = (uint8_t)(((j >> 4) & 7) * 36);
        }
        pal[4 * k + 3] = 0;
    }
}

int main(void) {
    static tpoint p[64];
    static line_seg seg[64];
    static wkd3d_vtx vtx[600];
    line_view v;
    uint32_t lut[256];
    uint8_t idx[256], pal[1024], lut12[4096], dark[256];
    int n, i, k;
    v.zoom = 1.0; v.org_x = 0; v.org_y = 0; v.cam_x = 0; v.cam_y = 0; v.w = FW; v.h = FH;

    /* the segments: turn 2's own points bright and fading in, turn 1's dim, turn 3 (this turn) left out */
    n = 0;
    for (i = 0; i < 10; i++) p[n++] = pt(10.0f + i * 10, 50.0f, 1, i == 0, 0, 100);
    for (i = 0; i < 10; i++) p[n++] = pt(110.0f + i * 10, 50.0f, 2, 0, 1, 1200);
    for (i = 0; i < 5; i++) p[n++] = pt(210.0f + i * 10, 50.0f, 3, 0, 1, 300);
    k = line_segments(p, n, 3, 2, 1, &v, seg);
    CHECK(k == 19);                          /* (20 points of turns 1 and 2, joined) */
    CHECK(seg[0].a > 0.34f && seg[0].a < 0.36f);
    CHECK(seg[k - 1].a > 0.99f);             /* (bright where it stopped) */
    CHECK(seg[10].a >= 0.5f && seg[10].a < seg[k - 1].a);
    for (i = 1; i < k; i++) CHECK(seg[i].a >= seg[i - 1].a);
    CHECK(seg[k - 1].c == (int)(1200 * 255.0 / SPEEDO_VMAX + 0.5));
    k = line_segments(p, n, 3, 2, 0, &v, seg);   /* (TurnLine=last: turn 2 only) */
    CHECK(k == 9);
    k = line_segments(p, n, 1000, 3, 1, &v, seg);   /* (the test switch: this turn too, bright) */
    CHECK(k == 24 && seg[k - 1].a > 0.99f);

    /* a break, points too close to draw, and the camera and zoom */
    n = 0;
    p[n++] = pt(0, 0, 1, 1, 1, 0);
    p[n++] = pt(1, 0, 1, 0, 1, 0);           /* (1 px: passed over) */
    p[n++] = pt(10, 0, 1, 0, 1, 0);
    p[n++] = pt(100, 0, 1, 1, 1, 0);         /* (a jump: not joined) */
    p[n++] = pt(110, 0, 1, 0, 1, 0);
    k = line_segments(p, n, 2, 1, 1, &v, seg);
    CHECK(k == 2 && seg[0].x0 == 0.0f && seg[0].x1 == 10.0f && seg[1].x0 == 100.0f);
    v.zoom = 2.0; v.org_x = 160; v.org_y = 100; v.cam_x = 50; v.cam_y = 0;
    k = line_segments(p, n, 2, 1, 1, &v, seg);
    CHECK(k == 3);                           /* (2 px on the screen now: drawn) */
    CHECK(seg[0].x0 == 60.0f && seg[0].y0 == 100.0f && seg[1].x1 == 80.0f);
    v.cam_x = 5000;                          /* (all off the screen: none) */
    k = line_segments(p, n, 2, 1, 1, &v, seg);
    CHECK(k == 0);
    v.zoom = 1.0; v.org_x = 0; v.org_y = 0; v.cam_x = 0; v.cam_y = 0;

    /* the triangles: 6 vertices a segment, 3 px wide, alpha from the opacity, the newest kept at the limit */
    line_colours(lut, 1, 800);
    n = 0;
    for (i = 0; i < 30; i++) p[n++] = pt(10.0f + i * 5, 100.0f, 1, i == 0, 1, (uint16_t)(i * 50));
    k = line_segments(p, n, 2, 1, 1, &v, seg);
    CHECK(k == 29);
    i = line_triangles(seg, k, lut, vtx, 600);
    CHECK(i == 29 * 6);
    CHECK(vtx[0].y == 101.5f && vtx[2].y == 98.5f);   /* (a horizontal segment: 1.5 px each side) */
    CHECK((vtx[i - 1].argb >> 24) == 255 && (vtx[0].argb >> 24) == (uint32_t)(seg[0].a * 255.0f + 0.5f));
    CHECK((vtx[0].argb >> 24) > 128 && (vtx[0].argb >> 24) < 140);   /* (0.5 and a step: the first of 29) */
    CHECK((vtx[0].argb & 0xffffffu) == lut[seg[0].c]);
    i = line_triangles(seg, k, lut, vtx, 60);
    CHECK(i == 60 && vtx[i - 1].x == seg[k - 1].x0 + 0.0f);   /* (the newest 10 segments) */
    i = line_triangles(seg, k, lut, vtx, 5);
    CHECK(i == 0);
    line_colours(lut, 0, 800);
    CHECK(lut[0] == lut[255] && (lut[0] >> 16) == 235);

    /* into the fake frame: an opaque line 3 px wide, a dim one thinned, the edges clipped with no overrun */
    for (i = 0; i < 256; i++) idx[i] = (uint8_t)(200 + (i & 15));
    frame_clear(7);
    seg[0].x0 = 20; seg[0].y0 = 20.5f; seg[0].x1 = 60; seg[0].y1 = 20.5f; seg[0].a = 1.0f; seg[0].c = 3;
    CHECK(line_frame(seg, 1, idx, frame, FW, FW, FH) > 0);
    CHECK(frame[19 * FW + 40] == 203 && frame[20 * FW + 40] == 203 && frame[21 * FW + 40] == 203);
    CHECK(frame[18 * FW + 40] == 7 && frame[22 * FW + 40] == 7);
    {
        long dim, full;
        frame_clear(7);
        seg[0].y0 = seg[0].y1 = 60.5f; seg[0].x0 = 0; seg[0].x1 = 200; seg[0].a = 1.0f;
        full = line_frame(seg, 1, idx, frame, FW, FW, FH);
        frame_clear(7);
        seg[0].a = 0.35f;
        dim = line_frame(seg, 1, idx, frame, FW, FW, FH);
        CHECK(dim * 16 > full * 5 && dim * 16 < full * 7);   /* (about 6 in 16) */
    }
    frame_clear(7);
    seg[0].x0 = -50; seg[0].y0 = -50; seg[0].x1 = FW + 50; seg[0].y1 = FH + 50; seg[0].a = 1.0f;
    seg[1].x0 = -10; seg[1].y0 = FH - 1; seg[1].x1 = FW + 10; seg[1].y1 = FH - 1; seg[1].a = 0.6f;
    seg[2].x0 = FW + 5; seg[2].y0 = 10; seg[2].x1 = FW + 50; seg[2].y1 = 90; seg[2].a = 1.0f;
    line_frame(seg, 3, idx, frame, FW, FW, FH);
    CHECK(guards_ok());

    /* the palette tables and the speedometer into the frame */
    make_palette(pal);
    pal_tables(pal, lut12, dark);
    CHECK(lut12[0xfff] == 127 && lut12[0] == 0);              /* (white: the lightest grey; black) */
    CHECK(pal[4 * lut12[0xf00]] == 255 && pal[4 * lut12[0xf00] + 1] == 0);   /* (red) */
    CHECK(dark[100] < 100 && dark[100] > 20);                 /* (a grey: a darker grey) */
    {
        static speedo_dial dial;
        const int d = 110;
        uint32_t *img = (uint32_t *)malloc((size_t)d * d * 4);
        long changed;
        int lit = 0, darkened = 0, y, x;
        CHECK(img != NULL);
        if (img) {
            memset(&dial, 0, sizeof(dial));
            CHECK(speedo_dial_paint(&dial, d, 800, 1000, 1000, 1500, 700, img, d * 4));
            frame_clear(100);
            changed = speedo_frame(img, d, d, FW - d - 4, FH - d - 4, frame, FW, FW, FH, lut12, dark);
            CHECK(changed > 500);
            for (y = 0; y < FH; y++) for (x = 0; x < FW; x++) {
                if (frame[y * FW + x] == dark[100]) darkened++;
                else if (frame[y * FW + x] != 100) lit++;
            }
            CHECK(lit > 200 && darkened > 50);
            CHECK(frame[0] == 100 && frame[(FH - 1) * FW] == 100);   /* (nothing outside the picture) */
            for (i = 0; i < 8; i++) {           /* (half off every edge: clipped, no overrun) */
                static const int xs[8] = {-60, FW - 50, 100, 100, -60, FW - 50, -60, FW - 50};
                static const int ys[8] = {50, 50, -60, FH - 50, -60, -60, FH - 50, FH - 50};
                frame_clear(100);
                speedo_frame(img, d, d, xs[i], ys[i], frame, FW, FW, FH, lut12, dark);
                CHECK(guards_ok());
            }
            free(img);
        }
        speedo_dial_free(&dial);
    }
    {   /* the number alone into the frame, half off the bottom right corner */
        static speedo_number num;
        int w, hh;
        uint32_t *img;
        memset(&num, 0, sizeof(num));
        speedo_number_size(34, &w, &hh);
        img = (uint32_t *)malloc((size_t)w * hh * 4);
        CHECK(img != NULL);
        if (img) {
            CHECK(speedo_number_paint(&num, 34, 1234, 800, img, w * 4));
            frame_clear(100);
            CHECK(speedo_frame(img, w, hh, 10, 10, frame, FW, FW, FH, lut12, dark) > 100);
            frame_clear(100);
            speedo_frame(img, w, hh, FW - w / 2, FH - hh / 2, frame, FW, FW, FH, lut12, dark);
            CHECK(guards_ok());
            free(img);
        }
        speedo_number_free(&num);
    }
    printf(failures ? "draw_test: %d failed\n" : "draw_test: all passed\n", failures);
    return failures != 0;
}
