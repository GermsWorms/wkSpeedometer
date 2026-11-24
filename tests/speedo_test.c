/* The speedometer pictures (speedo.h, ported with it from wkMapZoom): checks on the colours and the pictures' alpha, and with a folder argument
 * a sheet of dials and numbers at several speeds over a background (a 24-bit BMP given as the second argument, else
 * grey) written out as speedo-sheet.bmp to look at. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/speedo.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t *load_bmp(const char *path, int *w, int *h) {   /* (24-bit, top-down or bottom-up) */
    FILE *f = fopen(path, "rb");
    uint8_t hdr[54];
    uint32_t *px, off;
    int32_t bw, bh;
    int x, y, stride, top;
    uint8_t *row;
    if (!f || fread(hdr, 1, 54, f) != 54) { if (f) fclose(f); return NULL; }
    memcpy(&off, hdr + 10, 4); memcpy(&bw, hdr + 18, 4); memcpy(&bh, hdr + 22, 4);
    top = bh < 0; if (bh < 0) bh = -bh;
    stride = (bw * 3 + 3) & ~3;
    px = (uint32_t *)malloc((size_t)bw * bh * 4); row = (uint8_t *)malloc((size_t)stride);
    for (y = 0; px && row && y < bh; y++) {
        fseek(f, (long)(off + (size_t)y * stride), SEEK_SET);
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        for (x = 0; x < bw; x++) px[(size_t)(top ? y : bh - 1 - y) * bw + x] = row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16;
    }
    free(row); fclose(f);
    *w = bw; *h = bh;
    return px;
}

static void save_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t size = 54 + (uint32_t)w * h * 4, off = 54, isz = 40;
    int32_t hh = -h;
    uint16_t planes = 1, bpp = 32;
    if (!f) return;
    memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &isz, 4); memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &hh, 4); memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* a premultiplied picture over the sheet at (x0, y0) */
static void put(uint32_t *sheet, int sw, int sh, const uint32_t *img, int w, int h, int x0, int y0) {
    int x, y, k;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        const uint32_t c = img[y * w + x], a = c >> 24;
        uint32_t *d, o = 0;
        if (x0 + x < 0 || y0 + y < 0 || x0 + x >= sw || y0 + y >= sh || !a) continue;
        d = sheet + (size_t)(y0 + y) * sw + x0 + x;
        for (k = 0; k < 24; k += 8) {
            uint32_t v = ((c >> k) & 0xff) + ((*d >> k) & 0xff) * (255 - a) / 255;
            o |= (v > 255 ? 255 : v) << k;
        }
        *d = o;
    }
}

int main(int argc, char **argv) {
    static const double speeds[6] = {0, 260, 640, 1080, 1700, 2263};
    speedo_dial dial;
    speedo_number num;
    uint32_t *img, *sheet = NULL;
    int i, x, y, sw = 0, sh = 0, d = 176, nw, nh;
    speedo_rgb c;
    memset(&dial, 0, sizeof(dial));
    memset(&num, 0, sizeof(num));
    c = speedo_colour(0, 500); CHECK(c.b > 0.9f && c.r < 0.3f);         /* blue at rest */
    c = speedo_colour(500, 500); CHECK(c.r > 0.9f && c.g < 0.3f);       /* red at the red line */
    c = speedo_colour(2000, 500); CHECK(c.r > 0.9f && c.g < 0.3f);      /* and past it */
    c = speedo_colour(375, 500); CHECK(c.r > 0.9f && c.g > 0.7f);       /* yellow on the way */
    img = (uint32_t *)malloc((size_t)d * d * 4);
    if (!img) { printf("speedo_test: no memory\n"); return 1; }
    CHECK(speedo_dial_paint(&dial, d, 500, 800, 800, 1200, 650, img, d * 4));
    {   /* see-through: the corners clear, the middle (behind the digits' gaps) mostly clear, the ring's lit part opaque */
        float lx, ly;
        CHECK((img[0] >> 24) == 0 && (img[d * d - 1] >> 24) == 0);
        speedo_at(d / 2.0f, d / 2.0f, 330, d / 2.0f - 4.5f * d / 100.0f - 4.25f * d / 100.0f, &lx, &ly);
        CHECK((img[(int)ly * d + (int)lx] >> 24) > 200);
    }
    {   /* every size the module asks for, extreme speeds: each picture painted into a buffer with guard words after it
         * (an overrun shows), clear in its corners; the number mode clear around its digits */
        static const double vs[] = {-5.0, 0.0, 59.0, 2399.0, 2400.0, 2600.0, 9999.0, 12000.0};
        const int sizes[] = {110, 120, 160, 240, 480}, nsizes[] = {22, 34, 45, 68, 136};
        int si, vi, k;
        for (si = 0; si < 5; si++) for (vi = 0; vi < 8; vi++) {
            const int dd = sizes[si];
            uint32_t *b = (uint32_t *)malloc(((size_t)dd * dd + 64) * 4);
            if (!b) continue;
            for (k = 0; k < 64; k++) b[dd * dd + k] = 0xdeadbeefu;
            CHECK(speedo_dial_paint(&dial, dd, vi & 1 ? 800 : 500, vs[vi], vs[vi], vs[vi] * 1.3, vs[vi] * 0.6, b, dd * 4));
            for (k = 0; k < 64; k++) if (b[dd * dd + k] != 0xdeadbeefu) { CHECK(!"dial overrun"); break; }
            CHECK((b[0] >> 24) == 0 && (b[dd - 1] >> 24) == 0 && (b[(size_t)dd * (dd - 1)] >> 24) == 0);
            free(b);
        }
        for (si = 0; si < 5; si++) for (vi = 0; vi < 8; vi++) {
            int w, hh;
            uint32_t *b;
            speedo_number_size(nsizes[si], &w, &hh);
            b = (uint32_t *)malloc(((size_t)w * hh + 64) * 4);
            if (!b) continue;
            for (k = 0; k < 64; k++) b[w * hh + k] = 0xdeadbeefu;
            CHECK(speedo_number_paint(&num, nsizes[si], vs[vi], 800, b, w * 4));
            for (k = 0; k < 64; k++) if (b[w * hh + k] != 0xdeadbeefu) { CHECK(!"number overrun"); break; }
            CHECK((b[0] >> 24) == 0 && (b[w - 1] >> 24) == 0);   /* (see-through: no box) */
            free(b);
        }
    }
    if (argc > 1) {
        char path[MAX_PATH];
        if (argc > 2) {
            int bw, bh;
            uint32_t *bg = load_bmp(argv[2], &bw, &bh);
            if (bg) { sw = bw; sh = bh; sheet = bg; }
        }
        if (!sheet) { sw = 1200; sh = 640; sheet = (uint32_t *)malloc((size_t)sw * sh * 4); for (i = 0; i < sw * sh; i++) sheet[i] = 0x8c8c8c; }
        for (i = 0; i < 6; i++) {
            const double v = speeds[i];
            speedo_dial_paint(&dial, d, 800, v, v, v > 0 ? v * 1.25 : 0, v > 0 ? v * 0.7 : 0, img, d * 4);
            put(sheet, sw, sh, img, d, d, 10 + (i % 3) * (d + 20), 10 + (i / 3) * (d + 20));
        }
        speedo_number_size(40, &nw, &nh);
        free(img);
        img = (uint32_t *)malloc((size_t)nw * nh * 4);
        for (i = 0; i < 4; i++) {
            speedo_number_paint(&num, 40, speeds[i + 2], 800, img, nw * 4);
            put(sheet, sw, sh, img, nw, nh, 10 + i * (nw + 16), 2 * (d + 20) + 10);
        }
        snprintf(path, sizeof(path), "%s\\speedo-sheet.bmp", argv[1]);
        for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) sheet[y * sw + x] |= 0xff000000u;
        save_bmp(path, sheet, sw, sh);
        printf("wrote %s\n", path);
    }
    speedo_dial_free(&dial);
    speedo_number_free(&num);
    free(img); free(sheet);
    printf(failures ? "speedo_test: %d failed\n" : "speedo_test: all passed\n", failures);
    return failures != 0;
}
