/*
 * fxlab — run glitchESP effects on the PC.
 *
 *   fxlab --synth out.ppm                         write a 720x1280 synthetic test image
 *   fxlab in.ppm out.ppm [options] fx[:k=v,...] ...  apply an effect chain
 *
 * Options: --seed N   --frame N   --amount A(0..1)   --repeat N (timing loop)
 * Effects are applied in the order given (max 3). Parameters override the defaults, or
 * the one-knob mapping when --amount is given.
 * Images are binary PPM (P6). Convert with tools/fxlab/img.py (Pillow).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fx.h"

static uint16_t *load_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    char magic[3] = { 0 };
    int maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 || strcmp(magic, "P6") != 0 || maxv != 255) {
        fprintf(stderr, "%s: need binary PPM (P6, maxval 255)\n", path);
        fclose(f);
        return NULL;
    }
    fgetc(f); /* single whitespace after maxval */
    size_t n = (size_t)(*w) * (size_t)(*h);
    unsigned char *rgb = malloc(n * 3);
    uint16_t *px = malloc(n * sizeof(uint16_t));
    if (!rgb || !px || fread(rgb, 3, n, f) != n) {
        fprintf(stderr, "%s: short read\n", path);
        fclose(f); free(rgb); free(px);
        return NULL;
    }
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        px[i] = fx_rgb565(rgb[i * 3] >> 3, rgb[i * 3 + 1] >> 2, rgb[i * 3 + 2] >> 3);
    }
    free(rgb);
    return px;
}

static int save_ppm(const char *path, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    size_t n = (size_t)w * (size_t)h;
    unsigned char *rgb = malloc(n * 3);
    for (size_t i = 0; i < n; i++) {
        unsigned r = fx_r5(px[i]), g = fx_g6(px[i]), b = fx_b5(px[i]);
        rgb[i * 3]     = (unsigned char)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (unsigned char)((b << 3) | (b >> 2));
    }
    fwrite(rgb, 3, n, f);
    fclose(f);
    free(rgb);
    return 0;
}

/* Synthetic test image: gradients, colour bars, a grid and a few discs. */
static uint16_t *synth(int w, int h)
{
    uint16_t *px = malloc((size_t)w * h * sizeof(uint16_t));
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned r = (unsigned)(x * 31 / (w - 1));
            unsigned g = (unsigned)(y * 63 / (h - 1));
            unsigned b = (unsigned)(((x + y) / 8) & 31);
            if (y < h / 6) {                       /* colour bars on top */
                static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
                px[(size_t)y * w + x] = bars[x * 8 / w];
                continue;
            }
            if ((x % 80) < 2 || (y % 80) < 2) {  /* grid */
                px[(size_t)y * w + x] = 0xFFFF;
                continue;
            }
            int cx = w / 2, cy = h * 2 / 3, d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 < 150 * 150) {                  /* disc */
                px[(size_t)y * w + x] = d2 < 60 * 60 ? 0x0000 : 0xF800;
                continue;
            }
            px[(size_t)y * w + x] = fx_rgb565(r, g, b);
        }
    }
    return px;
}

static int parse_fx(fx_chain_t *chain, const char *spec, int have_amount, float amount)
{
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0;
    const fx_desc_t *fx = fx_registry_find(buf);
    if (!fx) {
        fprintf(stderr, "unknown effect '%s'. Available:", buf);
        for (int i = 0; i < fx_registry_count(); i++) fprintf(stderr, " %s", fx_registry_get(i)->id);
        fprintf(stderr, "\n");
        return -1;
    }
    int slot = fx_chain_add(chain, fx);
    if (slot < 0) { fprintf(stderr, "chain full (max %d)\n", FX_CHAIN_MAX); return -1; }
    if (have_amount && fx->from_amount) fx->from_amount(amount, chain->slots[slot].params);
    if (colon) {
        char *tok = strtok(colon + 1, ",");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                int pi = fx_param_index(fx, tok);
                if (pi < 0) { fprintf(stderr, "%s: unknown param '%s'\n", fx->id, tok); return -1; }
                chain->slots[slot].params[pi] = (float)atof(eq + 1);
            }
            tok = strtok(NULL, ",");
        }
    }
    return 0;
}

static void list_effects(void)
{
    for (int i = 0; i < fx_registry_count(); i++) {
        const fx_desc_t *fx = fx_registry_get(i);
        printf("%-10s %s\n", fx->id, fx->name);
        for (int p = 0; p < fx->n_params; p++) {
            printf("    %-12s %-16s [%g .. %g] default %g\n", fx->params[p].id, fx->params[p].name,
                   fx->params[p].min, fx->params[p].max, fx->params[p].def);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--list") == 0) { list_effects(); return 0; }
    if (argc >= 3 && strcmp(argv[1], "--synth") == 0) {
        uint16_t *px = synth(720, 1280);
        int rc = save_ppm(argv[2], px, 720, 1280);
        free(px);
        return rc;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: fxlab in.ppm out.ppm [--seed N] [--frame N] [--amount A] [--repeat N] fx[:k=v,..]...\n"
                        "       fxlab --synth out.ppm | fxlab --list\n");
        return 2;
    }

    uint32_t seed = 1, frame = 0;
    int repeat = 1, have_amount = 0;
    float amount = 0.5f;
    fx_chain_t chain;
    fx_chain_clear(&chain);

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)        seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--frame") && i + 1 < argc)  frame = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--amount") && i + 1 < argc) { amount = (float)atof(argv[++i]); have_amount = 1; }
        else if (parse_fx(&chain, argv[i], have_amount, amount) != 0) return 2;
    }

    int w, h;
    uint16_t *in = load_ppm(argv[1], &w, &h);
    if (!in) return 1;
    uint16_t *out = malloc((size_t)w * h * 2), *tmp = malloc((size_t)w * h * 2);

    fx_frame_t fin = { in, (uint16_t)w, (uint16_t)h, (uint32_t)w };
    fx_frame_t fout = { out, (uint16_t)w, (uint16_t)h, (uint32_t)w };
    fx_frame_t ftmp = { tmp, (uint16_t)w, (uint16_t)h, (uint32_t)w };
    fx_ctx_t ctx = { .seed = seed, .frame_no = frame, .prev = NULL, .scratch = NULL, .scratch_len = 0 };

    clock_t t0 = clock();
    for (int r = 0; r < repeat; r++) {
        ctx.frame_no = frame + (uint32_t)r;
        fx_chain_apply(&chain, &fin, &fout, &ftmp, &ctx);
    }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (repeat > 0 ? repeat : 1);
    printf("%dx%d, %d effect(s), %.2f ms/frame on this PC\n", w, h, chain.count, ms);

    int rc = save_ppm(argv[2], out, w, h);
    free(in); free(out); free(tmp);
    return rc;
}
