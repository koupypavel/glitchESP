/*
 * Small sounds through the board's ES8311 codec and speaker amplifier: a shutter click and
 * two beeps for recording. They are synthesized, so there are no audio files to ship.
 *
 * sound_play() only queues the request; a task does the work. The codec and amplifier are
 * switched on for a sound and off again shortly after, so the speaker does not hiss.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SOUND_SHUTTER, SOUND_REC_START, SOUND_REC_STOP } sound_id_t;

esp_err_t sound_init(void);
void sound_play(sound_id_t id);     /* does nothing when sounds are switched off in the settings */

#ifdef __cplusplus
}
#endif
