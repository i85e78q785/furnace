#include "soundchip.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct Channel {
    int square_enable;
    int noise_enable;
    int wavetable_enable;

    uint16_t square_period; /* 1..2047 */
    uint16_t wavetable_period; /* 1..2047 */

    uint8_t volume; /* 0..15 */
    int wavetable_bank; /* 0 or 1 */
    int wavetable_volbit; /* 0 or 1 */

    /* runtime state */
    double square_phase; /* fractional accumulator */
    double wavetable_phase; /* fractional index position */
};

struct SoundChip {
    unsigned sample_rate;
    uint8_t sram[SC_SRAM_SIZE];

    struct Channel ch[SC_CHANNELS];

    /* wavetable samples-per-table (16 or 32) */
    unsigned samples_per_table;

    /* Noise LFSR */
    uint16_t lfsr; /* 16-bit LFSR state */
    uint8_t tap_a; /* 0..15 */
    int tap_b_enable;
    uint8_t noise_freq_index; /* 0..15 */
    double noise_timer; /* accum */

    /* chip clock/divider */
    uint32_t chip_clock_hz;
    uint32_t clock_divider;

    /* analog filter state */
    double filter_cutoff_hz;
    double filter_alpha; /* 0..1 */
    double filter_l_prev, filter_r_prev;

    /* precomputed volume table (0..15 -> scale 0.0..1.0 quadratic) */
    double vol_table[16];
};

/* noise frequency table: samples per LFSR step (approx). We'll map indices to
 * periods so index 0 is very low freq (long period), 15 is very fast.
 */
static const double noise_period_table[16] = {
    4096, 2048, 1536, 1024, 768, 512, 384, 256,
    192, 128, 96, 64, 48, 32, 24, 16
};

SoundChip *sc_create(unsigned sample_rate) {
    SoundChip *sc = (SoundChip*)calloc(1, sizeof(SoundChip));
    if (!sc) return NULL;
    sc->sample_rate = sample_rate;
    for (int i = 0; i < 16; ++i) {
        double v = (double)i / 15.0;
        sc->vol_table[i] = v * v; /* quadratic */
    }
    sc_reset(sc);
    /* default: use 16-sample tables (user can set 16 or 32) */
    sc->wt_samples_per_table[0] = 16;
    sc->wt_samples_per_table[1] = 16;
    return sc;
}

void sc_destroy(SoundChip *sc) {
    free(sc);
}

void sc_reset(SoundChip *sc) {
    memset(sc->sram, 0, SC_SRAM_SIZE);
    for (int i = 0; i < SC_CHANNELS; ++i) {
        sc->ch[i].square_enable = 0;
        sc->ch[i].noise_enable = 0;
        sc->ch[i].wavetable_enable = 0;
        sc->ch[i].square_period = 256;
        sc->ch[i].wavetable_period = 256;
        sc->ch[i].volume = 15;
        sc->ch[i].wavetable_table_index = 0;
        sc->ch[i].wavetable_volbit = 1;
        sc->ch[i].square_phase = 0.0;
        sc->ch[i].wavetable_phase = 0.0;
    }
    sc->lfsr = 0xACE1u; /* non-zero seed */
    sc->tap_a = 0;
    sc->tap_b_enable = 0;
    sc->noise_freq_index = 4;
    sc->noise_timer = 0.0;
    sc->chip_clock_hz = 2000000u;
    sc->clock_divider = 4u;
    sc->samples_per_table = 16;
    sc->filter_cutoff_hz = 125000.0;
    sc->filter_alpha = 0.0;
    sc->filter_l_prev = sc->filter_r_prev = 0.0;
}

void sc_load_sram(SoundChip *sc, const uint8_t data[SC_SRAM_SIZE]) {
    memcpy(sc->sram, data, SC_SRAM_SIZE);
}

static int ch_valid(int ch) { return ch >= 0 && ch < SC_CHANNELS; }

void sc_set_square_enable(SoundChip *sc, int ch, int enable) {
    if (!ch_valid(ch)) return; sc->ch[ch].square_enable = !!enable;
}
void sc_set_noise_enable(SoundChip *sc, int ch, int enable) {
    if (!ch_valid(ch)) return; sc->ch[ch].noise_enable = !!enable;
}
void sc_set_wavetable_enable(SoundChip *sc, int ch, int enable) {
    if (!ch_valid(ch)) return; sc->ch[ch].wavetable_enable = !!enable;
}

void sc_set_square_period(SoundChip *sc, int ch, uint16_t period11) {
    if (!ch_valid(ch)) return; uint16_t p = period11 & 0x07FF; if (p==0) p=1; sc->ch[ch].square_period = p;
}
void sc_set_wavetable_period(SoundChip *sc, int ch, uint16_t period11) {
    if (!ch_valid(ch)) return; uint16_t p = period11 & 0x07FF; if (p==0) p=1; sc->ch[ch].wavetable_period = p;
}

void sc_set_volume(SoundChip *sc, int ch, uint8_t vol4) {
    if (!ch_valid(ch)) return; sc->ch[ch].volume = vol4 & 0x0F;
}

void sc_set_wavetable_bank(SoundChip *sc, int ch, int bank) {
    if (!ch_valid(ch)) return; sc->ch[ch].wavetable_bank = bank ? 1 : 0;
}
void sc_set_wavetable_mode(SoundChip *sc, unsigned samples_per_table) {
    if (!sc) return;
    if (samples_per_table != 16 && samples_per_table != 32) return;
    sc->samples_per_table = samples_per_table;
}

void sc_set_wavetable_index(SoundChip *sc, int ch, unsigned table_index) {
    if (!ch_valid(ch)) return;
    sc->ch[ch].wavetable_table_index = table_index;
}

void sc_set_wavetable_volbit(SoundChip *sc, int ch, int bit) {
    if (!ch_valid(ch)) return; sc->ch[ch].wavetable_volbit = !!bit;
}

void sc_set_chip_clock(SoundChip *sc, uint32_t chip_clock_hz, uint32_t divider) {
    if (!sc) return;
    if (divider == 0) divider = 1;
    sc->chip_clock_hz = chip_clock_hz;
    sc->clock_divider = divider;
}

void sc_set_filter_cutoff(SoundChip *sc, double cutoff_hz) {
    if (!sc) return;
    sc->filter_cutoff_hz = cutoff_hz;
    /* compute alpha for simple one-pole lowpass: alpha = exp(-2*pi*fc/fs) */
    double fc = cutoff_hz;
    double fs = (double)sc->sample_rate;
    if (fc <= 0.0) sc->filter_alpha = 1.0; else sc->filter_alpha = exp(-2.0 * M_PI * fc / fs);
}
void sc_set_wavetable_volbit(SoundChip *sc, int ch, int bit) {
    if (!ch_valid(ch)) return; sc->ch[ch].wavetable_volbit = !!bit;
}

void sc_set_noise_tap_a(SoundChip *sc, uint8_t tap_a4) { sc->tap_a = tap_a4 & 0x0F; }
void sc_set_noise_tap_b_enable(SoundChip *sc, int enable) { sc->tap_b_enable = !!enable; }
void sc_set_noise_freq_index(SoundChip *sc, uint8_t idx0_15) { sc->noise_freq_index = idx0_15 & 0x0F; }

void sc_phase_reset(SoundChip *sc) {
    for (int i = 0; i < SC_CHANNELS; ++i) {
        sc->ch[i].square_phase = 0.0;
        sc->ch[i].wavetable_phase = 0.0;
    }
    sc->lfsr = 0xACE1u;
    sc->noise_timer = 0.0;
}

/* helper: fetch wavetable sample (0..255) */
/* Fetch sample from wavetable: we treat each bank as having tables starting at
 * its base. For simplicity the active table is the first table in the bank and
 * its size is sc->wt_samples_per_table[bank] (16 or 32). Future work could
 * allow selecting different tables within the bank.
 */
static uint8_t wt_sample(const SoundChip *sc, unsigned table_index, size_t idx) {
    unsigned tblsz = sc->samples_per_table;
    if (tblsz == 0) tblsz = 16;
    unsigned tables_in_sram = SC_SRAM_SIZE / tblsz;
    if (tables_in_sram == 0) return 0;
    unsigned ti = table_index % tables_in_sram;
    size_t base = (size_t)ti * tblsz;
    idx %= tblsz;
    return sc->sram[base + idx];
}

/* Render implementation
 * Mixing rule: for each channel, form an 8-bit value from wavetable (0..255),
 * square (0 or 255), noise (0 or 255), then OR them: out8 = wt | sq | nz.
 * Then convert out8 (0..255) to signed float -1..1, scale by volume quadratic
 * and pan to stereo (even->left, odd->right). Channels not enabled produce 0.
 */
void sc_render(SoundChip *sc, int16_t *buffer, size_t frames) {
    if (!sc || !buffer) return;

    /* chip ticks per audio sample */
    double chip_ticks_per_sample = (double)sc->chip_clock_hz / (double)sc->clock_divider / (double)sc->sample_rate;

    /* precompute table size and tables count */
    unsigned tblsz = sc->samples_per_table ? sc->samples_per_table : 16;
    unsigned tables_in_sram = SC_SRAM_SIZE / tblsz;

    for (size_t f = 0; f < frames; ++f) {
        double out_l = 0.0, out_r = 0.0;

        /* per-channel processing */
        for (int ch = 0; ch < SC_CHANNELS; ++ch) {
            struct Channel *c = &sc->ch[ch];

            uint8_t wt_val = 0;
            if (c->wavetable_enable && c->wavetable_volbit && tables_in_sram > 0) {
                unsigned table_index = c->wavetable_table_index % tables_in_sram;
                size_t idx = (size_t)floor(c->wavetable_phase) % tblsz;
                wt_val = wt_sample(sc, table_index, idx);
            }

            uint8_t sq_val = 0;
            if (c->square_enable) {
                /* square toggles every square_period chip ticks (half-cycle) */
                /* use square_phase as chip-tick accumulator */
                int bit = (c->square_phase < (double)c->square_period) ? 1 : 0;
                sq_val = bit ? 255 : 0;
            }

            uint8_t nz_val = 0;
            if (c->noise_enable) {
                uint8_t bit = (sc->lfsr & 1) ? 255 : 0;
                nz_val = bit;
            }

            uint8_t mixed = wt_val | sq_val | nz_val;

            /* convert mixed (0..255) to -1..1 float */
            double s = ((double)mixed / 127.5) - 1.0;

            /* apply per-channel volume quadratic */
            double vol_scale = sc->vol_table[c->volume];
            s *= vol_scale;

            /* pan: even -> left, odd -> right */
            if ((ch & 1) == 0) out_l += s; else out_r += s;

            /* advance wavetable phase: wavetable_period is chip ticks per table step */
            if (c->wavetable_enable) {
                double steps_per_sample = chip_ticks_per_sample / (double)c->wavetable_period;
                c->wavetable_phase += steps_per_sample;
                if (c->wavetable_phase >= (double)tblsz) c->wavetable_phase = fmod(c->wavetable_phase, (double)tblsz);
            }

            /* advance square phase in chip ticks */
            if (c->square_enable) {
                c->square_phase += chip_ticks_per_sample;
                double half = (double)c->square_period;
                if (c->square_phase >= half * 2.0) c->square_phase = fmod(c->square_phase, half * 2.0);
            }
        }

        /* advance noise LFSR according to noise frequency (in chip ticks) */
        double noise_chip_ticks = noise_period_table[sc->noise_freq_index];
        sc->noise_timer += chip_ticks_per_sample;
        while (sc->noise_timer >= noise_chip_ticks) {
            sc->noise_timer -= noise_chip_ticks;
            unsigned bit0 = sc->lfsr & 1u;
            unsigned tap_bit = (sc->lfsr >> sc->tap_a) & 1u;
            unsigned fb = bit0 ^ tap_bit;
            if (sc->tap_b_enable) {
                unsigned tapb = (sc->lfsr >> 1) & 1u;
                fb ^= tapb;
            }
            sc->lfsr = (sc->lfsr >> 1) | ((fb & 1u) << 15);
        }

        /* apply analog one-pole lowpass filter */
        double a = sc->filter_alpha;
        double l_filtered = a * sc->filter_l_prev + (1.0 - a) * out_l;
        double r_filtered = a * sc->filter_r_prev + (1.0 - a) * out_r;
        sc->filter_l_prev = l_filtered;
        sc->filter_r_prev = r_filtered;

        /* clamp and write */
        if (l_filtered > 1.0) l_filtered = 1.0; if (l_filtered < -1.0) l_filtered = -1.0;
        if (r_filtered > 1.0) r_filtered = 1.0; if (r_filtered < -1.0) r_filtered = -1.0;

        buffer[f*2 + 0] = (int16_t)lrint(l_filtered * 32767.0);
        buffer[f*2 + 1] = (int16_t)lrint(r_filtered * 32767.0);
    }
}
