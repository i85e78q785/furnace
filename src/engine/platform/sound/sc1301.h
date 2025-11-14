#ifndef SOUNDCHIP_H
#define SOUNDCHIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSG-style soundchip emulator (C)
 * - 4 channels
 * - Square, Noise, Wavetable (shared SRAM)
 * - 1KB SRAM for wavetables
 * - Noise: 16 freq indices, 16-bit LFSR with configurable tap A (4-bit) and optional tap B
 * - Square and wavetable have independent 11-bit period registers
 * - Volumes are 4-bit per-channel; quadratic mapping applied at render
 * - Wavetable vol is a 1-bit gate; when 0, wavetable contributes 0
 * - Mixer: bitwise OR mixing of square/noise/wavetable (implemented via 8-bit bands)
 */

#define SC_CHANNELS 4
#define SC_SRAM_SIZE 1024

typedef struct SoundChip SoundChip;

/* Create/destroy */
SoundChip *sc_create(unsigned sample_rate);
void sc_destroy(SoundChip *sc);

/* Reset phases/LFSR */
void sc_reset(SoundChip *sc);

/* SRAM load (exactly SC_SRAM_SIZE bytes) */
void sc_load_sram(SoundChip *sc, const uint8_t data[SC_SRAM_SIZE]);

/* Channel control (channel: 0..3) */
void sc_set_square_enable(SoundChip *sc, int ch, int enable);
void sc_set_noise_enable(SoundChip *sc, int ch, int enable);
void sc_set_wavetable_enable(SoundChip *sc, int ch, int enable);

/* Square and wavetable periods are 11-bit (1..2047). We interpret them as
 * "samples per step" for wavetable and "samples per half-cycle" for square.
 */
void sc_set_square_period(SoundChip *sc, int ch, uint16_t period11);
void sc_set_wavetable_period(SoundChip *sc, int ch, uint16_t period11);

/* Per-channel 4-bit volume (0..15). Quadratic mapping is applied in render. */
void sc_set_volume(SoundChip *sc, int ch, uint8_t vol4);

/* Wavetable specifics */
/* bank: 0 => channels 0/1, 1 => channels 2/3 */
/* wavetable length per bank (1..512) */
/* per-channel 1-bit wavetable volume gate */
/* Wavetable specifics
 * - The SRAM is viewed as a list of consecutive tables. Each table is either
 *   16 or 32 samples (bytes). Use `sc_set_wavetable_mode` to choose table size.
 * - Channels select which table index to use with `sc_set_wavetable_index`.
 * - Tables can be shared by multiple channels (hardware supports up to two
 *   channels using the same table simultaneously).
 */
/* set global wavetable sample size: 16 or 32 */
void sc_set_wavetable_mode(SoundChip *sc, unsigned samples_per_table);
/* select which table index in SRAM this channel uses (0-based) */
void sc_set_wavetable_index(SoundChip *sc, int ch, unsigned table_index);
/* per-channel 1-bit wavetable volume gate */
void sc_set_wavetable_volbit(SoundChip *sc, int ch, int bit);
/* Chip clock and analog filter */
/* Set chip clock (Hz) and divider (integer). Effective chip tick rate is
 * chip_clock_hz / divider. Default: 2_000_000 Hz, divider 4.
 */
void sc_set_chip_clock(SoundChip *sc, uint32_t chip_clock_hz, uint32_t divider);
/* Set analog filter cutoff (Hz). Default ~125000 Hz. If cutoff >= sample_rate/2
 * the filter has effectively no attenuation (limited by sample rate).
 */
void sc_set_filter_cutoff(SoundChip *sc, double cutoff_hz);

/* Noise control */
/* tap_a: 0..15 selects which LFSR bit is used as tap A */
void sc_set_noise_tap_a(SoundChip *sc, uint8_t tap_a4);
/* tap_b_enable: 0/1 */
void sc_set_noise_tap_b_enable(SoundChip *sc, int enable);
/* noise freq index 0..15 */
void sc_set_noise_freq_index(SoundChip *sc, uint8_t idx0_15);

/* Phase reset */
void sc_phase_reset(SoundChip *sc);

/* Render frames: stereo interleaved signed 16-bit samples (frames*2 samples)
 * buffer must be preallocated
 */
void sc_render(SoundChip *sc, int16_t *buffer, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
