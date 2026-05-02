/* soundproducer.c -- Output sound stream generator (Final Adaptive Version)
 *
 * Copyright (C) 1990, 1991 Speech Research Laboratory, Minsk
 * Copyright (C) 2026 (Modified for adaptive crossfade)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "soundscript.h"
#include "voice.h"
#include "sink.h"

/* --- НАСТРОЙКИ АДАПТАЦИИ --- */
#define BASE_POWER 1.0f          /* Степень на нормальной скорости (100) */
#define SPEED_SENSITIVITY 0.008f /* Коэффициент агрессивности (чем выше, тем меньше ваты на скорости) */

static const int16_t synth_ctrl_data[][2] = {
    { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 },
    { 0, -1 }, { 0, -1 }, { 0x930C, 0 }, { 0x0CF74, 1 }, { 2, 0x8002 },
    { 0x930C, 1 }, { 2, 1 }, { 2, 0x8003 }, { 0x930C, 0 }, { 0x0CF74, 1 },
    { 0x308C, 1 }, { 0x0B8B, 2 }, { 0x502E, 1 }, { 0x66F0, 1 },
    { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 },
    { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }
};

static int16_t eval(icb_t *icb) {
    int16_t res = (icb->stretch >> 1);
    if (!(--icb->count)) {
        icb->stretch += icb->delta;
        icb->count = icb->period;
    }
    return res;
}

static uint16_t silence(sink_t *consumer, uint16_t length) {
    uint16_t i;
    for (i = 0; i < length; i++) sink_put(consumer, 0);
    return length;
}

static uint16_t fading(sink_t *consumer, const voice_t *voice, uint16_t sidx) {
    uint16_t i;
    int8_t sample = voice->samples[sidx - 1];
    for (i = 0; i < 3; i++) {
        sample >>= 1;
        sink_put(consumer, sample);
    }
    return 3;
}

static inline void sink_put_safe(sink_t *consumer, float sample) {
    if (sample > 127.0f) sample = 127.0f;
    else if (sample < -128.0f) sample = -128.0f;
    sink_put(consumer, (int8_t)sample);
}

void make_sound(soundscript_t *script, sink_t *consumer, int rate_factor) {
    int i;
    /* Вычисляем динамическую силу фейда */
    float adaptive_power = BASE_POWER + ((float)rate_factor * SPEED_SENSITIVITY);
    
    sink_put(consumer, 0);

    for (i = 0; (i < script->length) && !consumer->status; i++) {
        int16_t l = script->sounds[i].duration;
        uint16_t j = ((uint16_t)(script->sounds[i].id)) & 0xFF;
        uint16_t k;

        if (j >= 169) {
            /* Синтетические шумы и взрывные звуки */
            int16_t bx, cx;
            j -= 169; bx = synth_ctrl_data[j][0]; cx = synth_ctrl_data[j][1];
            if (cx != -1) {
                uint16_t ax = 205;
                int16_t sample_shift = (cx & 0xFF) + 8;
                float v1 = 0, v2 = 0, v3 = 0;
                for (k = 0; k <= l; k++) {
                    float si;
                    int16_t tmp = ax & 0x2D;
                    tmp ^= tmp >> 4;
                    tmp &= 0x0F;
                    if ((0x6996 >> tmp) & 0x01) ax |= 0x8000;
                    ax >>= 1;
                    int16_t backup_ax = ax;
                    ax >>= 2;
                    v3 *= 0.5f; v3 += v3 * 0.25f;
                    if (cx >= 0) v3 += v3 * 0.25f;
                    si = v3;
                    v3 = (v2 * 2.0f) - v1;
                    v1 = (float)ax;
                    float res = (v3 * ((float)bx / 32768.0f)) + v1 - si;
                    v3 = v2; v2 = res;
                    sink_put_safe(consumer, v2 / (float)(1 << sample_shift));
                    ax = backup_ax;
                }
            } else silence(consumer, l);
        } else if (l) {
            int16_t ax = 0;
            uint16_t sidx = script->voice->sound_offsets[j];
            uint16_t scnt = script->voice->sound_lengths[j];
            uint8_t stage = script->sounds[i].stage;

            if (scnt > VOICE_THRESHOLD) {
                do sink_put(consumer, script->voice->samples[sidx++]);
                while ((--scnt) && (--l));
            } else if (j >= 132) {
                while (l > ax) {
                    k = script->icb[stage].stretch;
                    do { sink_put(consumer, script->voice->samples[sidx++]); l--; } while ((--k) && (--scnt));
                    if (k) l -= silence(consumer, k);
                    else if (scnt > 1) l -= fading(consumer, script->voice, sidx);
                    ax = eval(&(script->icb[stage]));
                    sidx = script->voice->sound_offsets[j];
                    scnt = script->voice->sound_lengths[j];
                }
            } else {
                /* Смешивание с динамическим FADE_POWER */
                int16_t dx = 0;
                while (l >= ax) {
                    uint16_t next_pattern_offset;
                    k = script->icb[stage].stretch;
                    j = ((uint16_t)(script->sounds[i + 1].id)) & 0xFF;
                    next_pattern_offset = script->voice->sound_offsets[j + 1];
                    j = script->voice->sound_offsets[j];
                    
                    sink_put(consumer, 0);
                    ax = (int16_t)(script->voice->samples[j]);
                    
                    do {
                        float s_curr = (float)((int16_t)script->voice->samples[sidx]);
                        float s_next = (float)ax;
                        
                        /* Прогресс перехода */
                        float p = (l > 0) ? (float)dx / (float)(dx + l) : 1.0f;

                        /* mu определяет кривую смешивания */
                        float mu = powf(p, adaptive_power);
                        
                        float mixed = s_curr * (1.0f - mu) + s_next * mu;
                        
                        sink_put_safe(consumer, mixed);
                        
                        dx++; sidx++;
                        ax = ((++j) < next_pattern_offset) ? ((int16_t)(script->voice->samples[j])) : 0;
                    } while ((--k) && (--scnt));

                    if (k) dx += silence(consumer, k);
                    else if (scnt > 1) dx += fading(consumer, script->voice, sidx);

                    ax = dx + eval(&(script->icb[stage]));
                    j = ((uint16_t)(script->sounds[i].id)) & 0xFF;
                    sidx = script->voice->sound_offsets[j];
                    scnt = script->voice->sound_lengths[j];
                }
            }
        }
    }
    sink_flush(consumer);
}