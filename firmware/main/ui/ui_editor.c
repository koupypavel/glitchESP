#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lvgl.h"
#include "ui_editor.h"
#include "ui_lvgl.h"
#include "frame_pipeline.h"

#define ROW_H       52
#define HEAD_H      58
#define PAD         12
#define SLIDER_W    300
#define STEPS_MAX   500

static lv_obj_t *s_root;                        /* NULL when closed */
static const fx_desc_t *s_fx;                   /* effect being edited */
static lv_obj_t *s_ctl[FX_MAX_PARAMS];          /* slider or switch per parameter */
static lv_obj_t *s_val[FX_MAX_PARAMS];          /* value text */

/* ---- parameter <-> control ---- */

static bool is_switch(const fx_param_t *p)
{
    return p->step >= 1.0f && p->min == 0.0f && p->max == 1.0f;
}

/* Whole numbers are enough for anything with a wide range (pixels, degrees, counts). */
static bool is_integer(const fx_param_t *p)
{
    return p->step >= 1.0f || p->max - p->min >= 20.0f;
}

static int steps_of(const fx_param_t *p)
{
    float range = p->max - p->min;
    int n = is_integer(p) ? (int)(range / (p->step >= 1.0f ? p->step : 1.0f) + 0.5f) : 200;
    return n > STEPS_MAX ? STEPS_MAX : (n < 1 ? 1 : n);
}

static float value_of(const fx_param_t *p, int pos)
{
    float v = p->min + (p->max - p->min) * (float)pos / (float)steps_of(p);
    return is_integer(p) ? roundf(v) : v;
}

static int pos_of(const fx_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return (int)((v - p->min) * (float)steps_of(p) / (p->max - p->min) + 0.5f);
}

static void show_value(int k, float v)
{
    const fx_param_t *p = &s_fx->params[k];
    char text[16];
    if (is_switch(p))       snprintf(text, sizeof(text), "%s", v >= 0.5f ? "on" : "off");
    else if (is_integer(p)) snprintf(text, sizeof(text), "%d", (int)lroundf(v));
    else                    snprintf(text, sizeof(text), "%.2f", (double)v);
    lv_label_set_text(s_val[k], text);
}

static const fx_slot_t *slot_of(const fp_recipe_t *r, const fx_desc_t *fx)
{
    for (int i = 0; i < r->chain.count; i++) {
        if (r->chain.slots[i].fx == fx && r->chain.slots[i].enabled) return &r->chain.slots[i];
    }
    return NULL;
}

/* ---- events ---- */

static void slider_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    float v = value_of(&s_fx->params[k], (int)lv_slider_get_value(lv_event_get_target(e)));
    frame_pipeline_set_param(s_fx, k, v);
    show_value(k, v);
}

static void switch_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    float v = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1.0f : 0.0f;
    frame_pipeline_set_param(s_fx, k, v);
    show_value(k, v);
}

/* Back to what the amount knob would give this effect. */
static void reset_cb(lv_event_t *e)
{
    (void)e;
    fp_recipe_t r;
    float params[FX_MAX_PARAMS];
    frame_pipeline_get_recipe(&r);
    fx_params_defaults(s_fx, params);
    if (s_fx->from_amount) s_fx->from_amount(r.amount, params);
    for (int k = 0; k < s_fx->n_params; k++) frame_pipeline_set_param(s_fx, k, params[k]);
    ui_editor_sync();
}

static void close_cb(lv_event_t *e) { (void)e; ui_editor_close(); }

static void tab_cb(lv_event_t *e)
{
    const fx_desc_t *fx = lv_event_get_user_data(e);
    if (fx != s_fx) ui_editor_open(fx);
}

/* ---- building ---- */

static lv_obj_t *head_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 46);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

bool ui_editor_open(const fx_desc_t *fx)
{
    fp_recipe_t r;
    frame_pipeline_get_recipe(&r);
    if (!fx || !slot_of(&r, fx)) {                       /* default: the first active effect */
        fx = NULL;
        for (int i = 0; i < r.chain.count && !fx; i++) {
            if (r.chain.slots[i].fx && r.chain.slots[i].enabled) fx = r.chain.slots[i].fx;
        }
    }
    if (!fx) return false;
    ui_editor_close();
    s_fx = fx;

    /* a sheet over the control bar; the video above it stays visible */
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_root, FP_OUT_W, 2 * PAD + HEAD_H + fx->n_params * ROW_H);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_root, UI_COLOR_DIM, 0);        /* the video shows through, darkened */
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, PAD, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* header: one tab per active effect, then reset and close */
    lv_obj_t *head = lv_obj_create(s_root);
    lv_obj_set_size(head, LV_PCT(100), HEAD_H);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < r.chain.count; i++) {
        const fx_slot_t *s = &r.chain.slots[i];
        if (!s->fx || !s->enabled) continue;
        head_button(head, s->fx->name, s->fx == fx ? 0xE0007A : 0x303030, tab_cb, (void *)s->fx);
    }
    lv_obj_t *close = head_button(s_root, LV_SYMBOL_CLOSE, 0x505050, close_cb, NULL);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *reset = head_button(s_root, "Reset", 0x2060C0, reset_cb, NULL);
    lv_obj_align_to(reset, close, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    for (int k = 0; k < fx->n_params; k++) {
        const fx_param_t *p = &fx->params[k];
        int y = HEAD_H + k * ROW_H + ROW_H / 2;

        lv_obj_t *name = lv_label_create(s_root);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, FP_OUT_W - 2 * PAD - SLIDER_W - 100);
        lv_label_set_text(name, p->name);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, y - 10);

        s_val[k] = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_val[k], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_val[k], lv_color_hex(0xC0C0C0), 0);
        lv_obj_set_width(s_val[k], 64);
        lv_obj_set_style_text_align(s_val[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_val[k], LV_ALIGN_TOP_RIGHT, -(SLIDER_W + 24), y - 10);

        if (is_switch(p)) {
            s_ctl[k] = lv_switch_create(s_root);
            lv_obj_align(s_ctl[k], LV_ALIGN_TOP_RIGHT, -(SLIDER_W - 50), y - 14);
            lv_obj_add_event_cb(s_ctl[k], switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        } else {
            s_ctl[k] = lv_slider_create(s_root);
            lv_obj_set_size(s_ctl[k], SLIDER_W - 24, 18);
            lv_obj_align(s_ctl[k], LV_ALIGN_TOP_RIGHT, -12, y - 9);
            lv_slider_set_range(s_ctl[k], 0, steps_of(p));
            lv_obj_set_style_bg_color(s_ctl[k], lv_color_hex(0xE0007A), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_ctl[k], lv_color_hex(0xE0007A), LV_PART_KNOB);
            lv_obj_add_event_cb(s_ctl[k], slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        }
    }
    ui_editor_sync();
    return true;
}

void ui_editor_close(void)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    s_fx = NULL;
}

bool ui_editor_is_open(void)
{
    return s_root != NULL;
}

void ui_editor_sync(void)
{
    if (!s_root) return;
    fp_recipe_t r;
    frame_pipeline_get_recipe(&r);
    const fx_slot_t *slot = slot_of(&r, s_fx);
    if (!slot) {                                        /* its effect was switched off */
        if (!ui_editor_open(NULL)) ui_editor_close();
        return;
    }
    for (int k = 0; k < s_fx->n_params; k++) {
        const fx_param_t *p = &s_fx->params[k];
        float v = slot->params[k];
        if (is_switch(p)) {
            if (v >= 0.5f) lv_obj_add_state(s_ctl[k], LV_STATE_CHECKED);
            else           lv_obj_remove_state(s_ctl[k], LV_STATE_CHECKED);
        } else {
            lv_slider_set_value(s_ctl[k], pos_of(p, v), LV_ANIM_OFF);
        }
        show_value(k, v);
    }
}
