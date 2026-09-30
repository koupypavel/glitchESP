#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "presets.h"
#include "fx.h"

static const char *TAG = "presets";

#define NS              "glitch"
#define BLOB_VERSION    1
#define ID_LEN          12

typedef struct {
    char id[ID_LEN];                /* effect id, zero padded */
    uint8_t n_params;
    uint8_t enabled;
    uint8_t pad[2];
    float params[FX_MAX_PARAMS];
} preset_fx_t;

typedef struct {
    uint8_t version;
    uint8_t count;                  /* effects in the chain */
    uint8_t pad[2];
    float amount;
    uint32_t seed;
    preset_fx_t fx[FX_CHAIN_MAX];
} preset_blob_t;

static void key_for(int slot, char key[12])
{
    snprintf(key, 12, "preset%d", slot);
}

static esp_err_t read_blob(int slot, preset_blob_t *b)
{
    if (slot < 0 || slot >= PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    char key[12];
    key_for(slot, key);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READONLY, &h);
    if (ret != ESP_OK) return ESP_ERR_NOT_FOUND;        /* namespace not created yet */
    size_t len = sizeof(*b);
    ret = nvs_get_blob(h, key, b, &len);
    nvs_close(h);
    if (ret != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (len != sizeof(*b) || b->version != BLOB_VERSION || b->count > FX_CHAIN_MAX) return ESP_ERR_INVALID_VERSION;
    return ESP_OK;
}

bool presets_exists(int slot)
{
    preset_blob_t b;
    return read_blob(slot, &b) == ESP_OK;
}

esp_err_t presets_save(int slot, const fp_recipe_t *r)
{
    if (slot < 0 || slot >= PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    preset_blob_t b = { .version = BLOB_VERSION, .amount = r->amount, .seed = r->seed };
    for (int i = 0; i < r->chain.count && b.count < FX_CHAIN_MAX; i++) {
        const fx_slot_t *s = &r->chain.slots[i];
        if (!s->fx) continue;
        preset_fx_t *p = &b.fx[b.count++];
        strncpy(p->id, s->fx->id, ID_LEN - 1);
        p->n_params = s->fx->n_params;
        p->enabled = s->enabled;
        memcpy(p->params, s->params, sizeof(p->params));
    }
    char key[12];
    key_for(slot, key);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_blob(h, key, &b, sizeof(b));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "slot %d saved: %d effect(s), amount %.2f: %s", slot + 1, b.count, (double)b.amount, esp_err_to_name(ret));
    return ret;
}

esp_err_t presets_load(int slot, fp_recipe_t *out)
{
    preset_blob_t b;
    esp_err_t ret = read_blob(slot, &b);
    if (ret != ESP_OK) return ret;
    fx_chain_clear(&out->chain);
    for (int i = 0; i < b.count; i++) {
        const preset_fx_t *p = &b.fx[i];
        char id[ID_LEN + 1] = { 0 };
        memcpy(id, p->id, ID_LEN);
        const fx_desc_t *fx = fx_registry_find(id);
        if (!fx) {
            ESP_LOGW(TAG, "slot %d: effect '%s' no longer exists, skipped", slot + 1, id);
            continue;
        }
        int idx = fx_chain_add(&out->chain, fx);        /* starts from the effect's defaults */
        if (idx < 0) break;
        fx_slot_t *s = &out->chain.slots[idx];
        s->enabled = p->enabled;
        if (p->n_params == fx->n_params) {
            memcpy(s->params, p->params, sizeof(s->params));
        } else if (fx->from_amount) {
            fx->from_amount(b.amount, s->params);       /* parameter list changed: fall back to the knob */
        }
    }
    out->amount = b.amount;
    out->seed = b.seed;
    return ESP_OK;
}

esp_err_t presets_clear(int slot)
{
    if (slot < 0 || slot >= PRESET_SLOTS) return ESP_ERR_INVALID_ARG;
    char key[12];
    key_for(slot, key);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_erase_key(h, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

void presets_describe(int slot, char *buf, size_t len)
{
    preset_blob_t b;
    if (read_blob(slot, &b) != ESP_OK) {
        snprintf(buf, len, "empty");
        return;
    }
    buf[0] = 0;
    int shown = 0;
    for (int i = 0; i < b.count; i++) {
        char id[ID_LEN + 1] = { 0 };
        memcpy(id, b.fx[i].id, ID_LEN);
        const fx_desc_t *fx = fx_registry_find(id);
        if (!b.fx[i].enabled) continue;
        if (shown++) strlcat(buf, " + ", len);
        strlcat(buf, fx ? fx->name : id, len);
    }
    if (!shown) strlcat(buf, "no effects", len);
    char pct[12];
    snprintf(pct, sizeof(pct), "  %d%%", (int)(b.amount * 100.0f + 0.5f));
    strlcat(buf, pct, len);
}
