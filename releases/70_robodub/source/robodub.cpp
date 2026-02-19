/*
    Robodub — Stereo Sound Expander & Dub Delay
    For Music Thing Modular Workshop System

    Feed in a small mono signal and get evolving stereo soundscapes out.
    Part King Tubby dub delay, part digital glitch machine, part ring
    modulator. Not just another tape delay simulator.

    The sampler captures and mangles audio with ratchets, octave-up bursts,
    and reverse playback (inspired by Qubit Data Bender / Hologram Microcosm).
    The tape delay smears it across stereo with pitch-drifting wow and
    controllable feedback bloom. The ring modulator adds tremolo, warble,
    or metallic overtones on top. Everything stays immediate and playable.

    No dry signal passthrough — designed as a send effect, safe with gear
    that lacks mixer sends (Deluge, Reface, grooveboxes, etc).

    Controls:
        Main knob:  Feedback amount. Below halfway: repeats fade.
                    Halfway: sustain (unity). Above: bloom and growth
                    up to 106%. Cross-feed adds ~3% stereo spread.
        X knob:     Chaos — glitch density and probability.
                    Ratchets, octave-up, reverse, two-octave sparkle.
                    Each trigger rolls the dice. Effects stack.
        Y knob:     Ring mod — frequency sweep (1Hz tremolo → 2kHz
                    metallic). Dry signal always present underneath.
                    CV In 1 modulates the carrier frequency.

        Switch Down (momentary): Capture audio AND burst into delay.
                                 Hold to continuously feed audio in.
        Switch Middle:           Sample locked. Pulse In 2 replays
                                 with glitches from X knob.
        Switch Up:               Pulse In 2 muted. Delay tail rings
                                 out undisturbed. Performance kill switch.

        Pulse In 1:  Clock input (quarter notes → dotted-eighth delay)
        Pulse In 2:  Trigger input (behaviour depends on switch)
        CV In 1:     Ring mod FM (modulates Y knob carrier frequency)
        CV In 2:     Ring mod (replaces Y knob when patched)
        CV Out 1:    Envelope follower on delay output
        CV Out 2:    Tempo-synced LFO (1 cycle per bar)
        Pulse Out 1: Bar clock (1 per 4 beats)
        Pulse Out 2: 16th notes (4× clock rate)

    Signal chain:
        Input → HPF (80Hz) → Gate/Sample buffer
        → Stereo delay (dotted-eighth, clock syncable)
        → Tape wow (L=1.0Hz, R=1.3Hz, ±128 samples = ±29-37 cents)
        → Feedback LPF (~12kHz) + HPF (120Hz) → cross-feed → delay write
        → Output → ring mod (Y knob, CV In 1 = FM)

    AI-generated (Claude) + James Robinson.
    Built on ComputerCard v0.2.8.
*/

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include <cmath>    // for sinf() — used at startup only, never in ISR


// ============================================================================
// Fixed-point helpers
// ============================================================================

static inline int32_t __not_in_flash_func(clamp)(int32_t x, int32_t lo, int32_t hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// Fast LCG pseudo-random number generator (same one used by the reverb card)
static uint32_t rng_state = 12345;
static inline int32_t __not_in_flash_func(fast_rand_signed)()
{
    rng_state = 1664525u * rng_state + 1013904223u;
    // Return signed 16-bit range: -32768..32767
    return (int32_t)(int16_t)(rng_state >> 16);
}

static inline uint32_t __not_in_flash_func(fast_rand)()
{
    rng_state = 1664525u * rng_state + 1013904223u;
    return rng_state;
}


// ============================================================================
// Circular delay buffer (power-of-2 masking, int16_t storage)
// ============================================================================
// Pattern from the reverb card and Vink: power-of-2 allocation with
// bitmask wrapping. No modulo, no branches.

struct DelayBuffer
{
    int16_t *data;
    uint32_t mask;
    uint32_t writePos;
};

static void delay_init(DelayBuffer *db, uint32_t maxDelay)
{
    uint32_t size = 1;
    while (size < maxDelay + 1) size <<= 1;
    db->data = new int16_t[size];
    // Explicit zero — value-init via () isn't reliable after warm reset
    // (e.g. UF2 re-flash with USB connected, watchdog reset)
    for (uint32_t i = 0; i < size; i++) db->data[i] = 0;
    db->mask = size - 1;
    db->writePos = 0;
}

static inline void __not_in_flash_func(delay_write)(DelayBuffer *db, int16_t sample)
{
    db->data[db->writePos & db->mask] = sample;
    db->writePos++;
}

static inline int16_t __not_in_flash_func(delay_read)(DelayBuffer *db, uint32_t delaySamples)
{
    return db->data[(db->writePos - delaySamples) & db->mask];
}

// Read with linear interpolation for fractional delay.
// 'delayFixed' is 16.16 fixed-point (integer samples . fractional part).
// Same interpolation approach as Goldfish and Vink.
static inline int32_t __not_in_flash_func(delay_read_interp)(DelayBuffer *db, uint32_t delayFixed)
{
    uint32_t intPart = delayFixed >> 16;
    uint32_t frac = (delayFixed >> 8) & 0xFF;  // 8-bit fraction (0..255)

    int32_t s0 = delay_read(db, intPart);
    int32_t s1 = delay_read(db, intPart + 1);

    return s0 + (((s1 - s0) * (int32_t)frac) >> 8);
}


// ============================================================================
// One-pole IIR filters (fixed-point, from reverb card pattern)
// ============================================================================
// Coefficient 'b' maps to cutoff:
//   f_c = -(f_s / 2π) * ln(1 - b/65536)

struct OnePole { int32_t state; };

static inline void filter_init(OnePole *f) { f->state = 0; }

static inline int32_t __not_in_flash_func(filter_lp)(OnePole *f, int32_t b, int32_t input)
{
    f->state += (((input - f->state) * b + 32768) >> 16);
    return f->state;
}

static inline int32_t __not_in_flash_func(filter_hp)(OnePole *f, int32_t b, int32_t input)
{
    f->state += (((input - f->state) * b + 32768) >> 16);
    return input - f->state;
}




// ============================================================================
// Soft saturation
// ============================================================================
// Two-segment piecewise: linear below knee, compressed above.
// No division, no 64-bit math. Musically, this gives gentle analog-style
// limiting in the feedback path. The knee at ±8192 means signals only
// compress when feedback has pushed them well above nominal level.

static inline int32_t __not_in_flash_func(soft_clip)(int32_t x)
{
    const int32_t knee = 8192;
    const int32_t limit = 14336;
    int32_t ax = x < 0 ? -x : x;

    if (ax <= knee) return x;

    int32_t excess = ax - knee;
    int32_t compressed = knee + (excess >> 1);
    if (compressed > limit) compressed = limit;
    return (x < 0) ? -compressed : compressed;
}

// Delay write clipper: keeps the delay buffer within 12-bit DAC range
// (±2047) so that the delay output never needs hard clipping on readback.
// Knee at ±1800 (~88% of DAC range) — leaves most of the signal untouched
// while catching peaks that would otherwise hard-clip on the DAC.
// Hard limit at ±2047 = DAC rail.
// Below the knee: signal passes through untouched (clean repeats).
// Above the knee: 2:1 compression (warm saturation, like tape).
// At the limit: hard cap (shouldn't be audible — soft region catches most).
static inline int32_t __not_in_flash_func(delay_soft_clip)(int32_t x)
{
    const int32_t knee = 1800;
    const int32_t limit = 2047;
    int32_t ax = x < 0 ? -x : x;

    if (ax <= knee) return x;

    int32_t excess = ax - knee;
    int32_t compressed = knee + (excess >> 1);  // 2:1 ratio above knee
    if (compressed > limit) compressed = limit;
    return (x < 0) ? -compressed : compressed;
}


// ============================================================================
// Sine LFO via 512-point lookup table
// ============================================================================
// For the WOW component of tape modulation. Built once at startup using
// float sinf(), then accessed with pure integer math in the ISR.

#define SINE_TABLE_SIZE 512
static int16_t sine_table[SINE_TABLE_SIZE];

static void build_sine_table()
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++)
    {
        float angle = (float)i / (float)SINE_TABLE_SIZE * 6.2831853f;
        sine_table[i] = (int16_t)(sinf(angle) * 4096.0f);
    }
}

// Read with 32-bit phase accumulator and 8-bit interpolation. Returns ±4096.
static inline int32_t __not_in_flash_func(sine_lookup)(uint32_t phase)
{
    uint32_t idx  = (phase >> 23) & (SINE_TABLE_SIZE - 1);
    uint32_t frac = (phase >> 15) & 0xFF;
    uint32_t next = (idx + 1) & (SINE_TABLE_SIZE - 1);

    int32_t s0 = sine_table[idx];
    int32_t s1 = sine_table[next];
    return s0 + (((s1 - s0) * (int32_t)frac) >> 8);
}


// ============================================================================
// Ring modulator (post-delay effect on Y knob)
// ============================================================================
// Ring modulation multiplies the audio by a carrier sine wave, creating
// sum and difference frequencies — the classic "Dalek voice" / metallic
// robot sound. Musically, it turns melodic content into inharmonic,
// bell-like textures. At low carrier frequencies it creates tremolo;
// at higher frequencies it gets increasingly alien and metallic.
//
// Two modes depending on whether CV In 1 has a cable:
//
//   NO CABLE: Internal sine carrier. Y knob sweeps the carrier
//     frequency from ~1Hz (slow tremolo) to ~2kHz (harsh metallic).
//     This gives a wide range of effects from a single knob.
//     The frequency mapping is exponential so you get good resolution
//     in both the low-frequency tremolo range and the mid-range
//     where the interesting tonal stuff happens.
//
//   CABLE IN CV1: External carrier signal from CV In 1. The audio
//     is multiplied directly by whatever's patched in — another
//     oscillator, an LFO, noise, whatever. Y knob becomes wet/dry
//     mix (0=dry, max=100% ring mod). This is the classic ring mod
//     patch: audio × carrier.
//
// Uses the existing 512-point sine lookup table (built at startup).
// Just a phase accumulator + one multiply per sample — negligible CPU.


// ============================================================================
// Tape wow/flutter modulation
// ============================================================================
// Real tape machines have two distinct speed variation components:
//
// WOW  (0.5-3 Hz):  Slow, quasi-periodic drift from reel eccentricity
//                    and capstan irregularities. Mostly sinusoidal but
//                    with slight cycle-to-cycle timing variation.
//                    Depth: ±1.5ms typical (±72 samples at 48kHz)
//
// FLUTTER (5-20 Hz): Faster, more random jitter from motor cogging,
//                     tape-to-head friction, and mechanical resonance.
//                     Not periodic — modeled as band-limited noise.
//                     Depth: ±0.3ms typical (±14 samples at 48kHz)
//
// Each stereo channel gets its own modulator with different rates so the
// left and right pitch drift independently, creating natural stereo width.
//
// Implementation:
//   WOW:     Sine LFO with slowly-drifting rate (rate is itself modulated
//            by a very slow random walk). This prevents the wow from being
//            perfectly periodic, which sounds digital.
//   FLUTTER: White noise → one-pole lowpass @ ~15Hz → scale.
//            The lowpass shapes the noise into the right frequency band
//            without needing a bandpass filter (we don't care about DC
//            since we're adding this to a much larger base delay time).

struct TapeWobble
{
    // Wow: slow sine with drifting rate
    uint32_t wow_phase;           // 32-bit phase accumulator
    uint32_t wow_rate;            // Phase increment per sample
    uint32_t wow_rate_base;       // Centre rate
    int32_t  wow_rate_drift;      // Current drift from centre rate (random walk)

    // Flutter: lowpass-filtered noise
    OnePole  flutter_filter;      // Smooths white noise into flutter band
    int32_t  flutter_coeff;       // Filter coefficient (sets flutter bandwidth)

    // Output scaling
    int32_t  wow_depth;           // Wow depth in 16.16 fixed-point samples
    int32_t  flutter_depth;       // Flutter depth in 16.16 fixed-point samples
};

// Initialise a tape wobble modulator.
//   wow_hz:     Centre frequency of wow (e.g. 0.8)
//   flutter_hz: Cutoff of flutter noise filter (e.g. 15.0)
//   wow_ms:     Wow depth in milliseconds (e.g. 1.5)
//   flutter_ms: Flutter depth in milliseconds (e.g. 0.3)
//
// Uses float at init time only — never in the ISR.
static void wobble_init(TapeWobble *w, float wow_hz, float flutter_hz,
                        float wow_ms, float flutter_ms)
{
    // Wow: phase increment = freq * 2^32 / 48000
    w->wow_rate_base = (uint32_t)(wow_hz * 89478.485f);
    w->wow_rate = w->wow_rate_base;
    w->wow_phase = 0;
    w->wow_rate_drift = 0;

    // Flutter: one-pole LPF coefficient
    // b = 1 - exp(-2π * fc / fs), scaled to 65536
    // For 15Hz at 48kHz: b ≈ 128
    w->flutter_coeff = (int32_t)(flutter_hz * 8.555f);  // ≈ 65536 * 2π / 48000
    filter_init(&w->flutter_filter);

    // Depth: milliseconds → 16.16 fixed-point samples
    // 1ms at 48kHz = 48 samples → in 16.16 = 48 << 16 = 3,145,728
    w->wow_depth = (int32_t)(wow_ms * 3145728.0f);
    w->flutter_depth = (int32_t)(flutter_ms * 3145728.0f);
}

// Process one sample of tape wobble. Returns modulation offset in 16.16
// fixed-point samples (positive = delay increases = pitch drops).
static inline int32_t __not_in_flash_func(wobble_process)(TapeWobble *w)
{
    // ---- Wow: sine LFO with drifting rate ----

    // Random walk on the wow rate: every 4096 samples (~85ms), nudge the
    // rate by a tiny amount. This makes the wow cycle length wander
    // slightly, preventing the mechanical regularity of a pure sine.
    // The drift is constrained to ±25% of the base rate.
    if ((fast_rand() & 0xFFF) == 0)
    {
        // Nudge: ±(base_rate / 64) per step
        int32_t nudge = (int32_t)(w->wow_rate_base >> 6);
        w->wow_rate_drift += (fast_rand() & 1) ? nudge : -nudge;

        // Constrain drift to ±25% of base rate
        int32_t max_drift = (int32_t)(w->wow_rate_base >> 2);
        if (w->wow_rate_drift > max_drift)  w->wow_rate_drift = max_drift;
        if (w->wow_rate_drift < -max_drift) w->wow_rate_drift = -max_drift;
    }

    w->wow_rate = w->wow_rate_base + w->wow_rate_drift;
    w->wow_phase += w->wow_rate;

    // Sine lookup returns ±4096
    int32_t wow_val = sine_lookup(w->wow_phase);

    // Scale to depth: ±4096 * depth / 4096 = ±depth
    int32_t wow_offset = (wow_val * w->wow_depth) >> 12;

    // ---- Flutter: lowpass-filtered noise ----

    // Generate white noise in ±4096 range
    int32_t noise = fast_rand_signed() >> 3;  // ±4096

    // Filter to flutter bandwidth (~15Hz)
    int32_t flutter_val = filter_lp(&w->flutter_filter, w->flutter_coeff, noise);

    // Scale to depth
    int32_t flutter_offset = (flutter_val * w->flutter_depth) >> 12;

    // Combine wow + flutter → total modulation offset (16.16 samples)
    return wow_offset + flutter_offset;
}


// ============================================================================
// Feedback amount curve (Main knob → feedback gain)
// ============================================================================
// Dead simple rule: halfway = unity. Below halfway the repeats
// fade out; above halfway they grow. This makes it easy to
// communicate: "past 12 o'clock = blooming."
//
//   Knob 0%:     0% feedback    (dead zone — silence)
//   Knob 5-25%:  40% → 80%     (short-lived repeats)
//   Knob 25-50%: 80% → 100%    (building toward sustain)
//   Knob 50%:    100% = UNITY   ← halfway point, repeats sustain
//   Knob 50-100%: 100% → 115%  (bloom zone — fine control over growth)
//
// Above unity the delay "blooms" — signal grows on each pass.
// The clamping in the feedback path prevents runaway, so it just
// gets thicker and more saturated the harder you push.
//
// Output is 10-bit fixed-point: 1024 = 100% = unity gain.

static const int32_t feedback_curve_lut[11] = {
    500,   // 0%   knob → 48.8% feedback  (repeats die fast)
    750,   // 10%  knob → 73.2%           (steep ramp up)
    900,   // 20%  knob → 87.9%           (short repeats)
    950,   // 30%  knob → 92.8%           (getting warmer)
    1024,  // 40%  knob → 100.0%          ← UNITY (plateau starts)
    1024,  // 50%  knob → 100.0%          ← UNITY (plateau — solid sustain zone)
    1036,  // 60%  knob → 101.2%          (gentle bloom)
    1049,  // 70%  knob → 102.4%          (warming up)
    1061,  // 80%  knob → 103.6%          (noticeable growth)
    1073,  // 90%  knob → 104.8%          (thick and swelling)
    1085   // 100% knob → 106.0%          (maximum bloom)
};

static inline int32_t __not_in_flash_func(get_feedback)(int32_t knob)
{
    // Tiny dead zone at the start: below ~1% knob = zero feedback.
    // Lets you fully kill the repeats. Knobs don't reach true zero
    // (typically ~14 minimum), so 50 gives a reliable "off" zone
    // without eating into the curve — the physical midpoint (2048)
    // must land on LUT index 5 (unity) for the "halfway = sustain"
    // rule to feel right.
    if (knob < 50) return 0;

    // Map knob range (50..4095) directly onto LUT (0..10).
    // scaled = knob_position * 10, then index = integer part,
    // frac = fractional part for interpolation.
    int32_t remapped = ((knob - 50) * 4095) / 4045;
    int32_t scaled = remapped * 10;
    int32_t index = scaled >> 12;
    int32_t frac = scaled & 0xFFF;
    if (index >= 10) return feedback_curve_lut[10];

    int32_t v0 = feedback_curve_lut[index];
    int32_t v1 = feedback_curve_lut[index + 1];
    return v0 + (((v1 - v0) * frac) >> 12);
}


// ============================================================================
// 8-band multiband compressor (output only, no sidechain)
// ============================================================================
// Splits the stereo delay output into 8 frequency bands and applies
// per-band compression. Tames peaks independently per band, bringing
// up quiet textural detail (shimmer, ring mod harmonics) while preventing
// bass buildup and feedback bloom from dominating.
//
// Architecture: Band splitting + gain application run per-sample on core 0.
// Gain computation runs decimated (every 10 samples ≈ 4.8kHz).
// Sidechain ducking is planned for core 1 in a future version.

#define NUM_BANDS       8
#define NUM_CROSSOVERS  7

// Crossover filter coefficients for the 7 split points.
// Computed from: b = 65536 * (1 - exp(-2*pi*fc/48000))
static const int32_t crossover_coeffs[NUM_CROSSOVERS] = {
    849,    // 100 Hz
    2109,   // 250 Hz
    4977,   // 600 Hz
    11914,  // 1500 Hz
    25471,  // 3500 Hz
    43534,  // 7000 Hz
    58927   // 12000 Hz
};

// Per-band compression parameters (fixed, tuned for delay output)
// Lower bands: higher threshold (bass has more energy), gentler ratio
// Upper bands: lower threshold (bring up shimmer detail), stronger ratio
struct BandCompParams {
    int32_t threshold;      // Absolute value, 12-bit scale (0-2047)
    int32_t ratio_recip;    // 1/ratio in Q10 (1024=1:1, 512=2:1, 341=3:1)
    int32_t makeup_gain;    // Makeup gain in Q10 (1024=unity)
    int32_t inv_threshold;  // Precomputed (1<<16)/threshold — avoids ISR division
};

static BandCompParams comp_params[NUM_BANDS] = {
    // threshold, ratio_recip, makeup_gain, inv_threshold (filled at init)
    { 800, 683, 1126, 0 },   // <100Hz:     1.5:1, +0.8dB makeup
    { 700, 683, 1178, 0 },   // 100-250Hz:  1.5:1, +1.2dB
    { 500, 512, 1229, 0 },   // 250-600Hz:  2:1,   +1.6dB
    { 400, 512, 1280, 0 },   // 600-1.5kHz: 2:1,   +2.0dB
    { 300, 410, 1382, 0 },   // 1.5-3.5kHz: 2.5:1, +2.6dB
    { 250, 410, 1434, 0 },   // 3.5-7kHz:   2.5:1, +3.0dB
    { 200, 341, 1536, 0 },   // 7-12kHz:    3:1,   +3.5dB
    { 150, 341, 1638, 0 },   // 12kHz+:     3:1,   +4.1dB
};

// Envelope follower coefficients (tuned for ~4.8kHz decimated update rate)
#define ENV_ATTACK_COEFF    13107  // ~1ms attack
#define ENV_RELEASE_COEFF   262    // ~50ms release

// Gain smoother coefficient (48kHz, ~1ms time constant)
#define GAIN_SMOOTH_COEFF   400

// Per-channel filterbank state (7 OnePole filters for serial subtraction)
struct BandFilters {
    OnePole crossover[NUM_CROSSOVERS];
};

// Complete multiband processor state (all on core 0)
struct MultibandState {
    // Band splitting filters (per-sample audio path)
    BandFilters wet_filters_L;
    BandFilters wet_filters_R;

    // Wet envelope filters (decimated — splits wet mono for envelope)
    BandFilters wet_env_filters;

    // Envelope followers (decimated)
    int32_t wet_env[NUM_BANDS];

    // Per-band gain (updated at decimated rate, applied per-sample)
    int32_t band_gain[NUM_BANDS];    // Q10: 1024 = unity

    // Gain smoothers (anti-zipper, per-sample)
    OnePole gain_smooth[NUM_BANDS];

    // Decimation counter — gain recomputed every N samples
    uint32_t decimCounter;
};

// Initialise all multiband state
static void multiband_init(MultibandState *mb)
{
    for (int i = 0; i < NUM_CROSSOVERS; i++)
    {
        filter_init(&mb->wet_filters_L.crossover[i]);
        filter_init(&mb->wet_filters_R.crossover[i]);
        filter_init(&mb->wet_env_filters.crossover[i]);
    }
    for (int i = 0; i < NUM_BANDS; i++)
    {
        mb->wet_env[i] = 0;
        mb->band_gain[i] = 1024;  // Start at unity
        filter_init(&mb->gain_smooth[i]);
        mb->gain_smooth[i].state = 1024;  // Pre-fill smoother at unity
    }
    mb->decimCounter = 0;

    // Precompute inverse thresholds to avoid division in the ISR
    for (int i = 0; i < NUM_BANDS; i++)
        comp_params[i].inv_threshold = (1 << 16) / comp_params[i].threshold;
}

// Decimated gain update — compression only, no sidechain.
// Called every 10 samples from core 0 ISR (≈ 4.8kHz).
#define MULTIBAND_DECIM 10

static inline void __not_in_flash_func(multiband_update_gains)(
    MultibandState *mb, int32_t wet_mono)
{
    // Split wet mono into 8 bands for envelope detection
    int32_t wet_bands[NUM_BANDS];
    int32_t wet_residual = wet_mono;
    for (int i = 0; i < NUM_CROSSOVERS; i++)
    {
        wet_bands[i] = filter_lp(&mb->wet_env_filters.crossover[i],
                                  crossover_coeffs[i], wet_residual);
        wet_residual -= wet_bands[i];
    }
    wet_bands[NUM_CROSSOVERS] = wet_residual;

    // Compute per-band compression gains
    for (int i = 0; i < NUM_BANDS; i++)
    {
        // Envelope follower
        int32_t wet_abs = wet_bands[i] < 0 ? -wet_bands[i] : wet_bands[i];
        if (wet_abs > mb->wet_env[i])
            mb->wet_env[i] += ((wet_abs - mb->wet_env[i])
                               * ENV_ATTACK_COEFF + 32768) >> 16;
        else
            mb->wet_env[i] += ((wet_abs - mb->wet_env[i])
                               * ENV_RELEASE_COEFF + 32768) >> 16;

        // Compression gain
        int32_t gain = 1024;
        int32_t env = mb->wet_env[i];
        int32_t thresh = comp_params[i].threshold;
        if (env > thresh)
        {
            int32_t excess = env - thresh;
            int32_t reduction_factor = 1024 - comp_params[i].ratio_recip;
            int32_t reduction = (reduction_factor * ((excess * comp_params[i].inv_threshold) >> 8)) >> 8;
            gain = 1024 - reduction;
            if (gain < 128) gain = 128;
        }

        // Apply makeup gain
        gain = (gain * comp_params[i].makeup_gain) >> 10;
        if (gain > 2048) gain = 2048;

        mb->band_gain[i] = gain;
    }
}


// ============================================================================
// Sidechain ducking (spectral, runs on core 1)
// ============================================================================
// Core 1 reads the dry input and wet output (via shared volatiles), splits
// the dry signal into 8 bands, and computes per-band ducking gains.
// Core 0 applies these gains alongside the compressor gains.
//
// Key design decisions to avoid previous bugs:
//  - Core 1 waits for a "ready" flag before processing (startup wait)
//  - Core 1 sleeps with __wfe() instead of hot spin loops (no bus contention)
//  - Core 0 wakes core 1 with __sev() after writing new samples
//  - Decimated: core 1 only processes every SC_DECIM ticks (~4.8kHz)

// Sidechain parameters
#define SC_DECIM            10      // Process every 10th sample (~4.8kHz)

// Per-band sidechain ducking parameters.
// Follows professional multiband sidechain norms (TC Finalizer, Trackspacer):
//  - Bass: higher threshold (more energy), deeper duck, slower attack/release
//  - Mids: moderate all round
//  - Highs: lower threshold (less energy but more sensitive), lighter duck, faster timing
//
// Attack/release coefficients are for ~4.8kHz update rate.
// Coefficient formula: coeff = 65536 * (1 - exp(-1000 / (time_ms * 4800)))
// inv_range = (1<<16) / range — precomputed to avoid division on M0+
struct BandDuckParams {
    int32_t threshold;      // Dry envelope must exceed this to start ducking
    int32_t depth;          // Max ducking in Q10 (614 ≈ -4dB, 820 ≈ -8dB)
    int32_t inv_range;      // (1<<16) / range — controls how fast ducking ramps up
    int32_t attack_coeff;   // Envelope attack (higher = faster)
    int32_t release_coeff;  // Envelope release (higher = faster)
};

static const BandDuckParams duck_params[NUM_BANDS] = {
    //  thresh  depth   inv_range   attack      release
    //                  (range)     (time)      (time)
    { 200,      820,    109,        910,        66  },  // <100Hz:     high thresh, -8dB deep, 75ms attack, 200ms release
    { 180,      768,    131,        1820,       87  },  // 100-250Hz:  -6.5dB, 40ms attack, 150ms release
    { 160,      716,    146,        3547,       109 },  // 250-600Hz:  -5.5dB, 20ms attack, 120ms release
    { 150,      665,    164,        6400,       131 },  // 600-1.5kHz: -5dB, 10ms attack, 100ms release
    { 100,      716,    197,        9600,       87  },  // 1.5-3.5kHz: -5.5dB, 7ms attack, 150ms release
    { 80,       716,    218,        13107,      87  },  // 3.5-7kHz:   -5.5dB, 5ms attack, 150ms release
    { 70,       665,    256,        19200,      87  },  // 7-12kHz:    -5dB, 3.5ms attack, 150ms release
    { 60,       614,    327,        26214,      87  },  // 12kHz+:     -4dB, 2.5ms attack, 150ms release
};

// Shared state between core 0 (ISR) and core 1 (sidechain loop).
// All fields are volatile for lock-free inter-core communication.
// On M0+ (Cortex-M0+), aligned 32-bit reads/writes are atomic.
struct SharedSidechain {
    volatile int32_t dry_mono;          // Written by core 0
    volatile int32_t duck_gain[NUM_BANDS]; // Written by core 1, read by core 0 (Q10)
    volatile uint32_t sample_tick;      // Incremented by core 0 each sample
    volatile bool ready;                // Set by core 0 after startup mute completes
};

// Core 1's private state (not shared — only core 1 touches this)
struct SidechainState {
    BandFilters dry_filters;            // 7 crossover filters for dry signal
    int32_t dry_env[NUM_BANDS];         // Per-band envelope followers
    uint32_t last_tick;                 // Last processed tick
};

// Forward declaration — the actual loop is defined after the class
static void core1_sidechain_entry();

// Global pointer so core 1's static entry point can reach the shared state.
// Set before multicore_launch_core1() is called.
static volatile SharedSidechain *g_sidechain = nullptr;


// ============================================================================
// Tap tempo — manual BPM entry via switch gestures
// ============================================================================
// Entry: 4 rapid Up<->Middle switch flicks within 3 seconds.
// Plays a "Tap Tempo" announcement, then collects 6 taps (Switch::Down).
// Averages 5 intervals, normalises BPM to 60-200 range, converts to
// dotted-eighth delay time. Switch::Up during tapping aborts/restarts.
// External clock arriving on Pulse In 1 overrides tap tempo.

enum TapTempoState : uint8_t {
    TT_OFF = 0,       // Normal operation. Flick detector runs passively.
    TT_ANNOUNCE,       // Playing "Tap Tempo" tone sequence.
    TT_WAIT_TAP,       // Waiting for next Down toggle.
    TT_PLAY_TONE,      // Playing metronome click after a tap.
    TT_CONFIRM,        // Playing confirmation tones + LED flash.
    TT_EXIT            // Storing result, returning to normal.
};

// A step in a tone sequence: frequency (phase increment) + duration in samples.
// phaseInc == 0 means silence.
struct ToneStep {
    uint32_t phaseInc;   // 32-bit phase increment per sample (0 = silence)
    uint32_t duration;   // Duration in samples at 48kHz
};

// Phase increments for 32-bit accumulator at 48kHz:
//   phaseInc = freq * (2^32 / 48000)
//   1760 Hz (A6): 1760 * 89478.4853 ≈ 157,481,727
//    880 Hz (A5):  880 * 89478.4853 ≈  78,740,863
static constexpr uint32_t TT_PHASE_HIGH = 157481727u;  // 1760 Hz (A6)
static constexpr uint32_t TT_PHASE_LOW  =  78740863u;  //  880 Hz (A5)

// Duration constants (samples at 48kHz)
static constexpr uint32_t TT_DUR_50MS  = 2400;
static constexpr uint32_t TT_DUR_80MS  = 3840;
static constexpr uint32_t TT_DUR_30MS  = 1440;
static constexpr uint32_t TT_DUR_15MS  =  720;
static constexpr uint32_t TT_DUR_100MS = 4800;

// "Tap Tempo" announcement: HIGH-silence-LOW-silence-LOW
// Durations are ~4× the metronome click for a clear, unhurried announcement.
static const ToneStep TT_SEQ_ANNOUNCE[] = {
    { TT_PHASE_HIGH, TT_DUR_50MS * 4  },  // "Tap"    (200ms)
    { 0,             TT_DUR_30MS * 4   },  // silence  (120ms)
    { TT_PHASE_LOW,  TT_DUR_50MS * 4   },  // "Tem-"  (200ms)
    { 0,             TT_DUR_15MS * 4   },  // silence  (60ms)
    { TT_PHASE_LOW,  TT_DUR_80MS * 4   },  // "-po"   (320ms)
};
static constexpr uint8_t TT_SEQ_ANNOUNCE_LEN = 5;

// Confirmation: short beep + silence + long beep
// Same 4× scaling as announcement for a clear, unhurried confirmation.
static const ToneStep TT_SEQ_CONFIRM[] = {
    { TT_PHASE_HIGH, TT_DUR_30MS * 4  },  // short beep (120ms)
    { 0,             TT_DUR_15MS * 4   },  // silence    (60ms)
    { TT_PHASE_HIGH, TT_DUR_100MS * 4  },  // long beep  (400ms)
};
static constexpr uint8_t TT_SEQ_CONFIRM_LEN = 3;

// Flick detection constants
static constexpr uint32_t TT_FLICK_DEBOUNCE  = 240;     // 5ms debounce lockout
static constexpr uint32_t TT_FLICK_WINDOW    = 144000;  // 3-second window
static constexpr uint8_t  TT_FLICKS_REQUIRED = 4;       // Up<->Middle flicks to enter

// Tap collection
static constexpr uint8_t  TT_TAPS_REQUIRED   = 6;
static constexpr uint32_t TT_INACTIVITY_MAX  = 480000;  // 10 seconds

// Metronome volume: amplitude = sine_lookup * (vol * 205) >> 12
// At vol=5: peak ≈ 1025 (quarter of DAC range)
static constexpr uint8_t  TT_DEFAULT_VOLUME  = 5;


// ============================================================================
// Clock / tempo detection
// ============================================================================
// Pulse In 1 = quarter-note clock.
// Delay time = 3/8 of clock period (dotted eighth note).

#define DEFAULT_DELAY_SAMPLES (48000 * 500 / 1000)  // 500ms = quarter note at 120 BPM
#define MIN_DELAY_SAMPLES     (48000 * 100 / 1000)   // 100ms minimum
#define MAX_DELAY_SAMPLES     (48000 * 680 / 1000)   // 680ms maximum (buffer holds 682ms)
#define DELAY_BUFFER_MAX      32700                    // Just under 32768 — delay_init rounds up to 32768 (128KB total)


// ============================================================================
// Sample capture buffer — 24kHz / 12-bit (Akai S900 character)
// ============================================================================
// Records at half the ProcessSample rate (24kHz) with 12-bit quantization.
// This gives the crunchy, slightly gritty texture of classic 80s samplers.
// 12kHz Nyquist is plenty of bandwidth for musical content.
//
// At 24kHz with int16_t storage:
//   36000 samples = 1.5 seconds = 70KB RAM
//   With 2× 32K delay buffers (128KB) + this + code ≈ 248KB of 264KB.
//   Leaves ~16KB for stack and state — tight but tested.

#define SAMPLE_BUF_SIZE       36000   // 1.5 seconds at 24kHz = 72KB
#define MIN_PLAYBACK_SAMPLES  24000   // 0.5s at 48kHz — minimum time a triggered sample plays
#define MAX_CAPTURE_SAMPLES   96000   // 2.0s at 48kHz — maximum gate-open time for capture in Up mode

struct SampleBuffer
{
    int16_t *data;             // Heap-allocated — 72KB too large for stack!
    uint32_t writePos;         // Write index (in 24kHz samples)
    uint32_t length;           // Length of captured sample (in 24kHz samples)
    uint32_t playPos;          // Read index (in 24kHz samples)
    bool     skipToggle;       // Alternates each 48kHz call for 2:1 downsample
    bool     playToggle;       // Alternates each 48kHz call for 1:2 upsample
    bool     capturing;
    bool     playing;
    bool     hasContent;
    int16_t  lastPlaySample;   // Previous output sample (for interpolation)
    int16_t  nextPlaySample;   // Next output sample (for interpolation)

    // --- Chaos / Data Bender-style glitch state ---
    // These get rolled on each trigger from Pulse In 2.
    int32_t  playSpeed;        // 1 = normal, 2 = octave-up (double speed)
    bool     reverse;          // True = play sample backwards
    int32_t  ratchetCountdown; // Samples until auto-re-trigger (0 = no ratchet)

    // Minimum playback duration: even a tiny captured blip gets looped until
    // at least this many 48kHz samples have elapsed. Prevents "blip" triggers.
    uint32_t playbackTimer;    // Counts up from 0 each time playback starts

    // Maximum capture duration: caps how long audio feeds into the delay in
    // Middle mode. Prevents long gates from dumping too much audio.
    uint32_t captureTimer;     // Counts up from 0 each time capture starts

    // Crossfade on retrigger: when a new playback starts while the old one
    // is still outputting, we fade out the old value over ~1.3ms (64 samples)
    // to avoid a hard discontinuity click at the splice point.
    // Power-of-2 length (64) so the fade uses shifts instead of division.
    int32_t  fadeOutValue;     // Value to fade from (captured at retrigger)
    int32_t  fadeOutCounter;   // Counts down from 64 → 0

    // Fade-in on playback start: ramps from 0 to full over 64 samples (~1.3ms).
    // Prevents clicks from non-zero first samples.
    int32_t  fadeInCounter;    // Counts up from 0 → 64 (64 = fully faded in)
};

static void samplebuf_init(SampleBuffer *sb)
{
    // Heap-allocate the sample buffer — 72KB is far too large for the stack.
    // The RP2040 default stack is only ~2-4KB, so any struct containing this
    // array as a direct member would overflow the stack and crash on boot.
    sb->data = new int16_t[SAMPLE_BUF_SIZE];
    for (uint32_t i = 0; i < SAMPLE_BUF_SIZE; i++) sb->data[i] = 0;
    sb->writePos = 0;
    sb->length = 0;
    sb->playPos = 0;
    sb->skipToggle = false;
    sb->playToggle = false;
    sb->capturing = false;
    sb->playing = false;
    sb->hasContent = false;
    sb->lastPlaySample = 0;
    sb->nextPlaySample = 0;
    sb->playSpeed = 1;
    sb->reverse = false;
    sb->ratchetCountdown = 0;
    sb->playbackTimer = 0;
    sb->captureTimer = 0;
    sb->fadeOutValue = 0;
    sb->fadeOutCounter = 0;
    sb->fadeInCounter = 64;  // Start fully faded in (no ramp on init)
}

// Called every 48kHz sample during capture.
// Only actually writes every other call (24kHz effective rate).
// The 24kHz rate + 12-bit ADC already provides plenty of lo-fi
// character without additional bit-crushing.
static inline void __not_in_flash_func(samplebuf_write)(SampleBuffer *sb, int16_t sample)
{
    sb->skipToggle = !sb->skipToggle;
    if (!sb->skipToggle) return;  // Skip odd samples → 24kHz

    if (sb->writePos < SAMPLE_BUF_SIZE)
    {
        sb->data[sb->writePos] = sample;
        sb->writePos++;
        sb->length = sb->writePos;
    }
}

// Called every 48kHz sample during playback.
// Reads a new 24kHz sample every other call, interpolates between them
// to produce a smooth-ish 48kHz output.
//
// Supports variable speed and reverse via the chaos system:
//   playSpeed=1: normal 24kHz playback (skip every other 48kHz call)
//   playSpeed=2: octave-up — read a new sample every 48kHz call (no skip)
//                This halves the playback time and raises pitch by one octave.
//   reverse=true: reads from the end of the buffer toward the start.
//
static inline int16_t __not_in_flash_func(samplebuf_read)(SampleBuffer *sb)
{
    // Crossfade: if we're fading out a previous playback, blend it in
    // even if the new playback hasn't started yet. This ensures clicks
    // from retrigger are always smoothed.
    int32_t fadeContrib = 0;
    if (sb->fadeOutCounter > 0)
    {
        // Linear fade: value × (counter/64) using shift instead of division
        fadeContrib = (sb->fadeOutValue * sb->fadeOutCounter) >> 6;
        sb->fadeOutCounter--;
    }

    if (!sb->playing || sb->length == 0) return (int16_t)fadeContrib;

    // Track how long we've been playing (at 48kHz)
    sb->playbackTimer++;

    // Check if we've reached the end of the sample data.
    // DON'T loop — just output silence for the rest of the minimum
    // playback window. Looping caused crackly clicks at each loop
    // splice and sounded unmusical.
    bool reachedEnd = false;
    if (!sb->reverse && sb->playPos >= sb->length)
        reachedEnd = true;
    if (sb->reverse && sb->playPos == 0 && sb->playbackTimer > 1)
        reachedEnd = true;

    if (reachedEnd)
    {
        if (sb->playbackTimer >= MIN_PLAYBACK_SAMPLES)
            sb->playing = false;
        // Either way, output silence (just the fade contribution if any)
        return (int16_t)fadeContrib;
    }

    // Playback speed:
    //   speed 1: normal (24kHz → 48kHz via interpolation toggle)
    //   speed 2: octave-up (read a new sample every 48kHz call)
    //   speed 4: two-octaves-up (skip a sample each call = 4× speed)
    int32_t rawSample;

    if (sb->playSpeed == 1)
    {
        sb->playToggle = !sb->playToggle;

        if (sb->playToggle)
        {
            // Odd samples: interpolate halfway between last and next
            rawSample = ((int32_t)sb->lastPlaySample + (int32_t)sb->nextPlaySample) >> 1;
            goto apply_envelopes;
        }
    }

    // Advance to next sample
    sb->lastPlaySample = sb->nextPlaySample;

    if (sb->reverse)
    {
        if (sb->playPos > 0)
        {
            sb->playPos--;
            sb->nextPlaySample = sb->data[sb->playPos];
        }
        else
        {
            sb->nextPlaySample = 0;
        }
    }
    else
    {
        if (sb->playPos < sb->length)
        {
            sb->nextPlaySample = sb->data[sb->playPos];
            sb->playPos++;
        }
        else
        {
            sb->nextPlaySample = 0;
        }
    }

    // Two-octaves-up: skip an extra sample to double the advance rate.
    // Combined with speed >= 2 (no toggle), this gives 4× playback speed
    // = two octaves above the original pitch.
    if (sb->playSpeed >= 4)
    {
        if (sb->reverse)
        {
            if (sb->playPos > 0) sb->playPos--;
        }
        else
        {
            if (sb->playPos < sb->length) sb->playPos++;
        }
    }

    rawSample = (int32_t)sb->lastPlaySample;

apply_envelopes:
    // Fade-in: ramp from silence over 64 samples (~1.3ms) at playback start
    if (sb->fadeInCounter < 64)
    {
        sb->fadeInCounter++;
        rawSample = (rawSample * sb->fadeInCounter) >> 6;
    }

    // Fade-out near end of sample: ramp to silence over last 64 samples.
    // Prevents clicks when the sample data ends at a non-zero value.
    // Uses distance-to-end in 24kHz sample positions.
    {
        uint32_t samplesLeft;
        if (sb->reverse)
            samplesLeft = sb->playPos;
        else
            samplesLeft = (sb->playPos < sb->length) ? (sb->length - sb->playPos) : 0;
        if (samplesLeft < 64)
            rawSample = (rawSample * (int32_t)samplesLeft) >> 6;
    }

    // Retrigger crossfade: blend old playback out, new playback in
    if (sb->fadeOutCounter > 0)
    {
        int32_t fadeIn = (rawSample * (64 - sb->fadeOutCounter)) >> 6;
        return (int16_t)(fadeIn + fadeContrib);
    }
    return (int16_t)rawSample;
}

static inline void samplebuf_start_capture(SampleBuffer *sb)
{
    sb->writePos = 0;
    sb->length = 0;
    sb->skipToggle = false;
    sb->capturing = true;
    sb->hasContent = false;
    sb->captureTimer = 0;
}

static inline void samplebuf_stop_capture(SampleBuffer *sb)
{
    sb->capturing = false;
    if (sb->length > 0) sb->hasContent = true;
}

// Trigger sample playback. Speed and direction are set by the chaos
// system before calling this — we just respect whatever's been decided.
//
// If the buffer is already playing, we capture the current output level
// and set up a ~1ms (48 sample) crossfade to avoid a hard click at the
// splice point where the old playback is cut and the new one starts.
static inline void samplebuf_trigger_play(SampleBuffer *sb)
{
    if (sb->hasContent && sb->length > 0)
    {
        // If already playing, capture the current output for crossfade.
        // The last interpolated value is a good approximation of what
        // was just being output.
        if (sb->playing)
        {
            sb->fadeOutValue = (int32_t)sb->lastPlaySample;
            sb->fadeOutCounter = 64;  // ~1.3ms at 48kHz (power-of-2 for fast math)
        }

        sb->playToggle = false;

        if (sb->reverse)
        {
            // Start from end, walk backward
            sb->playPos = sb->length - 1;
            sb->lastPlaySample = 0;
            sb->nextPlaySample = sb->data[sb->playPos];
        }
        else
        {
            // Normal: start from beginning
            sb->playPos = 0;
            sb->lastPlaySample = 0;
            sb->nextPlaySample = sb->data[0];
        }

        sb->playing = true;
        sb->playbackTimer = 0;
        // Fade-in from silence — but skip if retrigger crossfade is active,
        // since the crossfade already handles the smooth transition.
        if (sb->fadeOutCounter == 0)
            sb->fadeInCounter = 0;
        else
            sb->fadeInCounter = 64;  // Already faded in (crossfade handles it)
    }
}

// ============================================================================
// Trigger density — X knob also controls chance of firing
// ============================================================================
// At X=0 (minimum), only 40% of incoming pulses actually trigger.
// At X=max (4095), 100% of pulses fire. This makes the X knob a single
// "intensity" control: low = sparse + clean, high = dense + glitchy.
//
// Returns true if this trigger should fire.

static inline bool density_check(int32_t chaosKnob)
{
    // Map knob 0..4095 to threshold 1638..4095 (40%..100% of 4095)
    // threshold = 1638 + (chaosKnob * 2457 / 4095)
    // Simplify: threshold = 1638 + (chaosKnob * 3) / 5
    int32_t threshold = 1638 + ((chaosKnob * 3) / 5);
    uint32_t roll = fast_rand() >> 20;  // 0..4095
    return (int32_t)roll < threshold;
}


// ============================================================================
// Chaos engine — Data Bender-inspired glitch probability
// ============================================================================
// Reads the X knob and rolls the RNG to decide which glitch effects to
// apply to the next sample trigger. Called once per trigger event.
//
// The knob maps to a "chaos budget" — the higher the knob, the more
// likely you are to get weird effects:
//
//   0-512:    Clean zone — no glitches at all (safe default)
//   512+:     Ratchets start appearing (10-45% chance)
//   1024+:    Octave-up joins the party (5-35% chance)
//   2048+:    Reverse playback possible too (10-30% chance)
//
// When a ratchet is scheduled, the engine sets a countdown timer.
// After the countdown expires, the sample re-triggers automatically —
// cutting off the current playback and starting fresh. This creates
// that stuttery, rhythmic repetition effect.

static void chaos_roll(SampleBuffer *sb, int32_t chaosKnob)
{
    // Reset to clean defaults
    sb->playSpeed = 1;
    sb->reverse = false;
    sb->ratchetCountdown = 0;

    // Below the dead zone? No glitches.
    if (chaosKnob < 512) return;

    // Probability helper: roll 0..4095, compare against a threshold.
    // Higher threshold = more likely to trigger.
    uint32_t roll;

    // ---- Ratchet chance (available from ~12% knob onward) ----
    // Probability ramps from ~10% at the start to ~45% at full.
    // A ratchet re-triggers the sample partway through playback,
    // creating a stutter/repeat.
    if (chaosKnob >= 512)
    {
        // Scale: 410 at knob=512, 1843 at knob=4095
        int32_t ratchetChance = 410 + ((chaosKnob - 512) * 1433 / 3583);
        roll = fast_rand() >> 20;  // 0..4095 range
        if ((int32_t)roll < ratchetChance)
        {
            // Schedule re-trigger between 25-75% through the sample.
            // At 24kHz, each sample is 2 x 48kHz calls, so we count
            // in 48kHz samples for the countdown.
            uint32_t quarterLen = (sb->length > 4) ? (sb->length >> 2) : 1;
            uint32_t randOffset = fast_rand() % (quarterLen * 2 + 1);
            // Convert 24kHz sample count to 48kHz countdown
            sb->ratchetCountdown = (int32_t)((quarterLen + randOffset) * 2);
        }
    }

    // ---- Octave-up chance (available from ~25% knob onward) ----
    // Double-speed playback raises pitch by one octave. Spread across
    // a wide range of the knob for gradual introduction.
    // Probability: ~5% at 25% knob, ramping to ~35% at 100%.
    if (chaosKnob >= 1024)
    {
        int32_t octaveChance = 205 + ((chaosKnob - 1024) * 1229 / 3071);
        roll = fast_rand() >> 20;
        if ((int32_t)roll < octaveChance)
        {
            sb->playSpeed = 2;
        }
    }

    // ---- Two-octaves-up chance (available from ~75% knob onward) ----
    // Quad-speed playback raises pitch by two octaves — sparkly, chimey,
    // almost bell-like. Only appears at the extreme end of the knob and
    // stays rare even at maximum, keeping it as an occasional surprise.
    // Probability: ~5% at 75%, ramping to ~15% at 100%.
    // Overrides the single octave-up if both roll true (two octaves wins).
    if (chaosKnob >= 3072)
    {
        int32_t doubleOctaveChance = 205 + ((chaosKnob - 3072) * 410 / 1023);
        roll = fast_rand() >> 20;
        if ((int32_t)roll < doubleOctaveChance)
        {
            sb->playSpeed = 4;
        }
    }

    // ---- Reverse chance (available from ~50% knob onward) ----
    // Plays the sample backwards. Combined with octave-up, this can
    // produce some seriously weird textures.
    // Probability: ~10% at 50% knob, ramping to ~30% at 100%.
    if (chaosKnob >= 2048)
    {
        int32_t reverseChance = 410 + ((chaosKnob - 2048) * 819 / 2047);
        roll = fast_rand() >> 20;
        if ((int32_t)roll < reverseChance)
        {
            sb->reverse = true;
        }
    }
}


// ============================================================================
// Robodub — Main card class
// ============================================================================

class Robodub : public ComputerCard
{
    // --- Stereo delay lines ---
    DelayBuffer delayL, delayR;

    // --- Input HPF (removes sub-bass mud before delay) ---
    OnePole inputHPF;

    // --- Input compressor (tames hot signals, boosts quiet ones) ---
    int32_t compEnvelope;       // Envelope follower state (tracks peak level)

    // --- Feedback filters (per channel) ---
    OnePole feedbackLPF_L, feedbackLPF_R;   // HF damping (tape darkening)
    OnePole dcBlockL, dcBlockR;              // DC blocker (prevents runaway)
    OnePole dipHi_L, dipHi_R;               // ~1400Hz LP (upper edge of dip 1)
    OnePole dipLo_L, dipLo_R;               // ~1050Hz LP (lower edge of dip 1)
    OnePole dip2Hi_L, dip2Hi_R;             // ~2700Hz LP (upper edge of dip 2)
    OnePole dip2Lo_L, dip2Lo_R;             // ~2100Hz LP (lower edge of dip 2)

    // --- Tape wow (simple sine modulation of delay read position) ---
    // Each channel gets its own slow sine LFO at a different rate,
    // creating gentle independent pitch drift — like two slightly
    // different tape machines. Bipolar modulation centred on zero,
    // so the average delay time stays on-tempo.
    uint32_t wowPhaseL;        // 32-bit phase accumulator, left channel
    uint32_t wowPhaseR;        // 32-bit phase accumulator, right channel

    // --- Tape glitch (random transport irregularities) ---
    // Models occasional tape slips, sticky capstans, worn rollers —
    // brief, random pitch spikes that make the delay feel alive.
    // Each channel gets its own glitch state so they slip independently.
    // glitchCountdown: samples remaining in the current glitch (0 = idle)
    // glitchOffset: the 16.16 fixed-point offset applied during the glitch
    // glitchCooldown: minimum gap between glitches (prevents clustering)
    int32_t  glitchCountdownL, glitchCountdownR;
    int32_t  glitchOffsetL, glitchOffsetR;
    int32_t  glitchTotalL, glitchTotalR;       // Total duration of current glitch
    uint32_t glitchCooldownL, glitchCooldownR;

    // --- Ring modulator (post-delay effect) ---
    uint32_t ringmod_phase;     // 32-bit phase accumulator for internal carrier

    // --- CV output LFO ---
    uint32_t lfo_phase_CV;

    // --- Clock detection ---
    uint32_t clockPeriod;
    uint32_t clockCounter;
    uint32_t delayTimeSamples;
    bool     clockSynced;

    // --- Feedback state ---
    int32_t feedbackL, feedbackR;

    // --- Sample buffer ---
    SampleBuffer sampleBuf;
    bool lastSwitchDown;       // Track switch transitions
    int32_t gateEnvelope;      // Fade-in/out for Switch::Down input (0-64)
    bool lastPulse2;           // Track Pulse In 2 edges
    uint32_t pulse2Lockout;    // Debounce: counts down after a trigger, blocks re-triggers

    // --- Clock division / multiplication ---
    uint32_t beatCounter;       // Counts samples since last quarter note
    uint32_t barCounter;        // Counts quarter notes (0-3) for bar detection
    bool     barPulseState;     // Current state of bar pulse output
    uint32_t barPulseHold;      // Hold counter for bar pulse (visual + output)
    uint32_t sixteenthPhase;    // Phase accumulator for 16th note generation
    bool     sixteenthState;    // Current state of 16th note pulse output

    // --- CV outputs ---
    int32_t  envFollower;       // Peak envelope follower on delay output
    uint32_t syncedLfoPhase;    // Phase accumulator for tempo-synced LFO

    // --- LED + startup ---
    uint32_t ledCounter;
    int32_t  peakLevel;
    int32_t  startupCounter;
    int32_t  playbackEnvelope;  // Tracks sample playback level for LED 3
    uint32_t sampleTrigFlash;   // Counts down after sample trigger for LED 3
    uint32_t clipHold;          // Clipping indicator hold counter for LED 0

    // --- Multiband compressor (decimated on core 0) ---
    MultibandState mb;

    // --- Sidechain ducking (core 1) ---
    SharedSidechain sidechain;

    // --- Tap tempo ---
    TapTempoState ttState;

    // Gesture detection
    int32_t  ttLastSwitchForFlick;   // Previous switch position for edge detection
    uint32_t ttFlickTimestamps[4];   // Ring buffer of last 4 flick timestamps
    uint8_t  ttFlickCount;           // Number of valid flicks in window
    uint32_t ttFlickDebounce;        // Debounce countdown (samples)
    uint32_t ttSampleCounter;        // Global sample counter for timestamps

    // Tone generation
    uint32_t ttTonePhase;            // 32-bit phase accumulator
    uint32_t ttTonePhaseInc;         // Current phase increment (0 = silence)
    uint32_t ttToneTimer;            // Samples remaining in current step
    uint8_t  ttSequenceStep;         // Current step index in sequence
    uint8_t  ttSequenceLength;       // Total steps in current sequence
    const ToneStep *ttCurrentSeq;    // Pointer to announce/confirm/dynamic sequence

    // Dynamic confirmation sequence (built at confirm time from tapped tempo)
    ToneStep ttDynConfirm[5];        // wait-click-silence-click-silence at tapped BPM

    // Tap collection
    uint8_t  ttTapCount;             // Number of taps collected (0-6)
    uint32_t ttTapTimestamps[6];     // Sample counter at each tap
    bool     ttLastSwitchDown;       // Track Down transitions for tap detection

    // LED + timing
    uint32_t ttLedFlashTimer;        // Counter for confirmation LED flash
    uint32_t ttTappedInterval;       // Raw tapped quarter-note interval (before doubling)
    uint32_t ttInactivityTimer;      // Counts up since last tap / state entry
    uint8_t  ttMetronomeVolume;      // 1-10 (default 5)
    bool     tapTempoLocked;         // Prevents clock timeout from clearing sync

    // --- Tap tempo helper functions ---

    // Start playing a tone sequence (announcement or confirmation)
    inline void tt_start_sequence(const ToneStep *seq, uint8_t len)
    {
        ttCurrentSeq = seq;
        ttSequenceLength = len;
        ttSequenceStep = 0;
        ttTonePhaseInc = seq[0].phaseInc;
        ttToneTimer = seq[0].duration;
        ttTonePhase = 0;
    }

    // Advance the current tone sequence by one sample.
    // Returns true when the entire sequence has finished.
    inline bool tt_advance_sequence()
    {
        if (ttToneTimer > 0) {
            ttToneTimer--;
            return false;
        }
        // Current step finished — advance to next
        ttSequenceStep++;
        if (ttSequenceStep >= ttSequenceLength) return true;  // Sequence done
        ttTonePhaseInc = ttCurrentSeq[ttSequenceStep].phaseInc;
        ttToneTimer = ttCurrentSeq[ttSequenceStep].duration;
        ttTonePhase = 0;  // Reset phase for clean tone start
        return false;
    }

    // Generate one audio sample of the current tone.
    // Returns a signed value suitable for mixing into audio output.
    inline int32_t __not_in_flash_func(tt_generate_tone)()
    {
        if (ttTonePhaseInc == 0) return 0;  // Silence step
        ttTonePhase += ttTonePhaseInc;
        int32_t raw = sine_lookup(ttTonePhase);  // ±4096
        // Scale by volume: amplitude = raw * (vol * 205) >> 12
        // At vol=5: (5 * 205) = 1025, >> 12 gives peak ≈ 1025
        return (raw * (ttMetronomeVolume * 205)) >> 12;
    }

    // Detect 4 rapid Up<->Middle switch flicks to enter tap tempo mode.
    // Called every sample. Only transitions between Up and Middle count;
    // any involvement of Down resets the counter (isolates from normal play).
    inline void __not_in_flash_func(tt_detect_flick)(int32_t sw)
    {
        // Debounce: wait after last valid transition
        if (ttFlickDebounce > 0) { ttFlickDebounce--; return; }

        // Only care about transitions
        if (sw == ttLastSwitchForFlick) return;

        int32_t prev = ttLastSwitchForFlick;
        ttLastSwitchForFlick = sw;

        // Any involvement of Down resets — this is normal play behaviour
        if (sw == Switch::Down || prev == Switch::Down) {
            ttFlickCount = 0;
            return;
        }

        // Valid flick: only count Up->Middle (one direction only).
        // A physical flick is Up->Middle->Up, but we only count the
        // Up->Middle half. This means 4 physical round-trip flicks = 4 counts,
        // not 8. Middle->Up is the return stroke and is ignored.
        if (prev == Switch::Up && sw == Switch::Middle)
        {
            ttFlickDebounce = TT_FLICK_DEBOUNCE;

            // Store timestamp in ring buffer
            uint8_t idx = ttFlickCount < TT_FLICKS_REQUIRED
                        ? ttFlickCount
                        : (TT_FLICKS_REQUIRED - 1);
            // Shift buffer if full
            if (ttFlickCount >= TT_FLICKS_REQUIRED) {
                for (uint8_t i = 0; i < TT_FLICKS_REQUIRED - 1; i++)
                    ttFlickTimestamps[i] = ttFlickTimestamps[i + 1];
                idx = TT_FLICKS_REQUIRED - 1;
            }
            ttFlickTimestamps[idx] = ttSampleCounter;
            if (ttFlickCount < TT_FLICKS_REQUIRED) ttFlickCount++;

            // Check if we have 4 flicks within the time window
            if (ttFlickCount >= TT_FLICKS_REQUIRED) {
                uint32_t oldest = ttFlickTimestamps[0];
                uint32_t newest = ttFlickTimestamps[TT_FLICKS_REQUIRED - 1];
                if ((newest - oldest) <= TT_FLICK_WINDOW) {
                    // Enter tap tempo mode!
                    ttFlickCount = 0;
                    ttState = TT_ANNOUNCE;
                    ttTapCount = 0;
                    ttLastSwitchDown = false;
                    ttInactivityTimer = 0;
                    tt_start_sequence(TT_SEQ_ANNOUNCE, TT_SEQ_ANNOUNCE_LEN);
                }
            }
        }
    }

    // Calculate delay time from 6 taps.
    // Averages 5 intervals. The tapped interval IS the desired delay time —
    // no dotted-eighth conversion (that's only for external quarter-note clocks).
    // Zone normalisation clamps to the buffer's usable range.
    inline void tt_calculate_tempo()
    {
        if (ttTapCount < 2) return;

        // Average the intervals between consecutive taps
        uint32_t sum = 0;
        uint8_t count = ttTapCount - 1;
        for (uint8_t i = 0; i < count; i++)
            sum += ttTapTimestamps[i + 1] - ttTapTimestamps[i];
        uint32_t avgInterval = sum / count;

        // Tapped interval = quarter note. Delay = dotted eighth = 3/8 of quarter.
        // Find the longest quarter-note multiple whose dotted-eighth fits in buffer.
        // e.g. 106 BPM: quarter=566ms, dotted-8th=212ms, 2×quarter→424ms fits → use 424ms.
        //      80 BPM: quarter=750ms, dotted-8th=281ms, 2×quarter→562ms fits → use 562ms.

        // First halve if the raw interval's dotted-eighth is already too long
        while (((avgInterval * 3) >> 3) > MAX_DELAY_SAMPLES) avgInterval >>= 1;

        // Save the raw tapped interval for confirmation playback
        ttTappedInterval = avgInterval;

        // Then double the quarter note as long as the dotted-eighth still fits
        uint32_t quarter = avgInterval;
        while ((((quarter << 1) * 3) >> 3) <= MAX_DELAY_SAMPLES) quarter <<= 1;

        // Dotted eighth = 3/8 of the (possibly doubled) quarter note
        uint32_t newDelay = (quarter * 3) >> 3;

        // Clamp to valid range (safety)
        if (newDelay < MIN_DELAY_SAMPLES) newDelay = MIN_DELAY_SAMPLES;
        if (newDelay > MAX_DELAY_SAMPLES) newDelay = MAX_DELAY_SAMPLES;

        // Store — the existing smooth transition handles the glide
        clockPeriod = quarter;
        delayTimeSamples = newDelay;
        clockSynced = true;
        tapTempoLocked = true;
    }

    // Update LEDs during tap tempo mode (bypasses normal LED control).
    // Called at 100Hz (inside the existing ledCounter block).
    inline void tt_update_leds()
    {
        switch (ttState) {
        case TT_ANNOUNCE:
            // All LEDs off during announcement
            for (int i = 0; i < 6; i++) LedOn(i, false);
            break;

        case TT_WAIT_TAP:
        case TT_PLAY_TONE:
            // Light LEDs 0..(tapCount-1) to show progress
            for (int i = 0; i < 6; i++)
                LedOn(i, i < (int)ttTapCount);
            break;

        case TT_CONFIRM:
            // Flash all 6 LEDs at ~5Hz during confirmation
            {
                bool on = (ttLedFlashTimer / 4800) & 1;  // Toggle every 100ms
                for (int i = 0; i < 6; i++) LedOn(i, on);
            }
            break;

        default:
            break;
        }
    }

    // Main tap tempo state machine. Called every sample when ttState != TT_OFF.
    inline void __not_in_flash_func(tt_update)(int32_t sw)
    {
        ttInactivityTimer++;

        switch (ttState) {
        case TT_ANNOUNCE:
            // Play announcement sequence
            if (tt_advance_sequence()) {
                // Announcement done — wait for first tap
                ttState = TT_WAIT_TAP;
                ttInactivityTimer = 0;
                ttTonePhaseInc = 0;
            }
            break;

        case TT_WAIT_TAP:
            // Check for abort: Switch::Up restarts
            if (sw == Switch::Up) {
                ttState = TT_ANNOUNCE;
                ttTapCount = 0;
                ttInactivityTimer = 0;
                ttLastSwitchDown = false;
                tt_start_sequence(TT_SEQ_ANNOUNCE, TT_SEQ_ANNOUNCE_LEN);
                break;
            }

            // Check for inactivity timeout
            if (ttInactivityTimer > TT_INACTIVITY_MAX) {
                ttState = TT_OFF;
                break;
            }

            // Detect Down press (rising edge)
            {
                bool isDown = (sw == Switch::Down);
                if (isDown && !ttLastSwitchDown) {
                    // Record this tap
                    ttTapTimestamps[ttTapCount] = ttSampleCounter;
                    ttTapCount++;
                    ttInactivityTimer = 0;

                    // Start metronome click: HIGH for tap 1, LOW for the rest
                    ToneStep click;
                    click.phaseInc = (ttTapCount == 1) ? TT_PHASE_HIGH : TT_PHASE_LOW;
                    click.duration = TT_DUR_30MS;

                    // We can't point to a local, so set tone state directly
                    ttTonePhaseInc = click.phaseInc;
                    ttToneTimer = click.duration;
                    ttTonePhase = 0;
                    ttCurrentSeq = nullptr;  // Not using a sequence for single clicks
                    ttSequenceStep = 0;
                    ttSequenceLength = 0;

                    ttState = TT_PLAY_TONE;
                }
                ttLastSwitchDown = isDown;
            }
            break;

        case TT_PLAY_TONE:
            // Check for abort: Switch::Up restarts
            if (sw == Switch::Up) {
                ttState = TT_ANNOUNCE;
                ttTapCount = 0;
                ttInactivityTimer = 0;
                ttLastSwitchDown = false;
                tt_start_sequence(TT_SEQ_ANNOUNCE, TT_SEQ_ANNOUNCE_LEN);
                break;
            }

            // Play the click tone (single step, no sequence)
            if (ttToneTimer > 0) {
                ttToneTimer--;
            } else {
                // Click done — check if we have enough taps
                ttTonePhaseInc = 0;
                if (ttTapCount >= TT_TAPS_REQUIRED) {
                    // All taps collected — calculate and confirm
                    tt_calculate_tempo();

                    // Build confirmation: wait one beat, then 2 clicks at tapped tempo.
                    // Uses the raw tapped interval (not the doubled clockPeriod).
                    uint32_t clickDur = TT_DUR_30MS;
                    uint32_t beatRest = (ttTappedInterval > clickDur) ? (ttTappedInterval - clickDur) : clickDur;
                    ttDynConfirm[0] = { 0, beatRest };              // wait after 6th tap
                    ttDynConfirm[1] = { TT_PHASE_HIGH, clickDur };  // confirm click 1
                    ttDynConfirm[2] = { 0, beatRest };              // beat gap
                    ttDynConfirm[3] = { TT_PHASE_HIGH, clickDur };  // confirm click 2
                    ttDynConfirm[4] = { 0, beatRest };              // tail silence

                    ttState = TT_CONFIRM;
                    ttLedFlashTimer = 0;
                    ttInactivityTimer = 0;
                    tt_start_sequence(ttDynConfirm, 5);
                } else {
                    // Wait for next tap
                    ttState = TT_WAIT_TAP;
                    ttInactivityTimer = 0;
                    // Track Down state — user might still be holding it
                    ttLastSwitchDown = (sw == Switch::Down);
                }
            }
            break;

        case TT_CONFIRM:
            ttLedFlashTimer++;
            if (tt_advance_sequence()) {
                // Confirmation done — exit
                ttState = TT_EXIT;
            }
            break;

        case TT_EXIT:
            // clockPeriod and delayTimeSamples already set by tt_calculate_tempo().
            // tapTempoLocked already true. Just return to normal.
            ttState = TT_OFF;
            ttTonePhaseInc = 0;
            break;

        default:
            break;
        }
    }

public:
    Robodub()
    {
        build_sine_table();

        delay_init(&delayL, DELAY_BUFFER_MAX);
        delay_init(&delayR, DELAY_BUFFER_MAX);

        filter_init(&inputHPF);
        compEnvelope = 0;
        filter_init(&feedbackLPF_L);
        filter_init(&feedbackLPF_R);
        filter_init(&dcBlockL);
        filter_init(&dcBlockR);
        filter_init(&dipHi_L);
        filter_init(&dipHi_R);
        filter_init(&dipLo_L);
        filter_init(&dipLo_R);
        filter_init(&dip2Hi_L);
        filter_init(&dip2Hi_R);
        filter_init(&dip2Lo_L);
        filter_init(&dip2Lo_R);
        ringmod_phase = 0;

        // Tape wow: two slow sine LFOs at different rates modulate
        // the delay read position per channel. Different starting phases
        // so L and R drift independently from the start.
        wowPhaseL = 0;
        wowPhaseR = 0x60000000;  // ~135° offset for stereo decorrelation

        glitchCountdownL = 0; glitchCountdownR = 0;
        glitchOffsetL = 0;    glitchOffsetR = 0;
        glitchTotalL = 0;     glitchTotalR = 0;
        glitchCooldownL = 0;  glitchCooldownR = 0;

        lfo_phase_CV = 0;

        clockPeriod = 0;
        clockCounter = 0;
        delayTimeSamples = DEFAULT_DELAY_SAMPLES;
        clockSynced = false;

        feedbackL = 0;
        feedbackR = 0;

        samplebuf_init(&sampleBuf);
        lastSwitchDown = false;
        gateEnvelope = 0;
        lastPulse2 = false;
        pulse2Lockout = 0;

        beatCounter = 0;
        barCounter = 0;
        barPulseState = false;
        barPulseHold = 0;
        sixteenthPhase = 0;
        sixteenthState = false;
        envFollower = 0;
        syncedLfoPhase = 0;

        ledCounter = 0;
        peakLevel = 0;
        startupCounter = 0;
        playbackEnvelope = 0;
        sampleTrigFlash = 0;
        clipHold = 0;

        // Multiband compressor (decimated on core 0)
        multiband_init(&mb);

        // Sidechain ducking — init shared state, launch core 1
        sidechain.dry_mono = 0;
        sidechain.sample_tick = 0;
        sidechain.ready = false;  // Core 1 waits until startup mute completes
        for (int i = 0; i < NUM_BANDS; i++)
            sidechain.duck_gain[i] = 1024;  // Unity (no ducking) until core 1 starts

        g_sidechain = &sidechain;
        multicore_launch_core1(core1_sidechain_entry);

        // Tap tempo — all off / zeroed
        ttState = TT_OFF;
        ttLastSwitchForFlick = -1;  // Invalid — forces first edge
        ttFlickCount = 0;
        ttFlickDebounce = 0;
        ttSampleCounter = 0;
        for (int i = 0; i < 4; i++) ttFlickTimestamps[i] = 0;
        ttTonePhase = 0;
        ttTonePhaseInc = 0;
        ttToneTimer = 0;
        ttSequenceStep = 0;
        ttSequenceLength = 0;
        ttCurrentSeq = nullptr;
        ttTapCount = 0;
        for (int i = 0; i < 6; i++) ttTapTimestamps[i] = 0;
        ttLastSwitchDown = false;
        ttLedFlashTimer = 0;
        ttTappedInterval = 0;
        ttInactivityTimer = 0;
        ttMetronomeVolume = TT_DEFAULT_VOLUME;
        tapTempoLocked = false;

        EnableNormalisationProbe();
    }

    // ====================================================================
    // ProcessSample — called at 48kHz in interrupt context
    // ====================================================================

    virtual void ProcessSample()
    {
        // ---- Startup mute (2 seconds) ----
        // Prevents thump from DC settling and filter initialisation.
        // Also gates core 1 sidechain — it waits for 'ready' before processing.
        if (startupCounter < 96000) { startupCounter++; AudioOut1(0); AudioOut2(0); return; }
        if (!sidechain.ready) sidechain.ready = true;  // Signal core 1: audio is up

        // ---- Tap tempo: sample counter + flick detection (always runs) ----
        ttSampleCounter++;

        // ---- Read controls ----
        int32_t mainKnob  = KnobVal(Knob::Main);   // Feedback amount
        int32_t chaosKnobRaw = KnobVal(Knob::X);     // Chaos / density
        int32_t reverbKnob = KnobVal(Knob::Y);      // Ring mod

        // X knob controls chaos directly.
        int32_t chaosKnob = chaosKnobRaw;
        int32_t switchPos  = SwitchVal();

        // Flick detection runs in all modes (lightweight, ~10 cycles)
        if (ttState == TT_OFF) tt_detect_flick(switchPos);

        // Run tap tempo state machine if active
        if (ttState != TT_OFF) tt_update(switchPos);

        // ---- Clock detection (Pulse In 1 = quarter notes) ----
        // Measures time between rising edges. Delay = 3/8 of period
        // (dotted eighth note). Smoothed to avoid glitchy jumps.
        clockCounter++;
        if (PulseIn1RisingEdge())
        {
            // Real external clock overrides tap tempo
            tapTempoLocked = false;

            // Accept clock periods between ~75ms and ~2s.
            // At 1ppqn: 30-640 BPM range. At 2ppqn: 15-320 BPM range.
            // Wide enough for fast clock divisions (2ppqn, 4ppqn) and slow tempos.
            if (clockCounter > 3600 && clockCounter < 96000)
            {
                clockPeriod = clockCounter;
                clockSynced = true;
            }
            clockCounter = 0;

            // Bar counter: count quarter notes 0-3, fire bar pulse on beat 1
            barCounter++;
            if (barCounter >= 4)
            {
                barCounter = 0;
                barPulseState = true;
                barPulseHold = 2400;  // ~50ms flash for LED + pulse output
            }

            // Reset 16th note phase on each quarter note for tight sync
            sixteenthPhase = 0;
            sixteenthState = true;  // Fire on the downbeat
        }
        // If no clock for 2 seconds, fall back to default tempo.
        // Don't clear sync if tap tempo set it — there's no external clock to timeout.
        if (clockCounter > 96000 && !tapTempoLocked) clockSynced = false;

        if (clockSynced)
        {
            // Find the longest multiple of the clock period whose dotted-eighth
            // fits in the delay buffer. This handles any ppqn (1, 2, 4, etc.)
            // automatically — fast clock divisions get doubled up until the
            // delay time fills the buffer as much as possible.
            uint32_t period = clockPeriod;
            while ((((period << 1) * 3) >> 3) <= MAX_DELAY_SAMPLES) period <<= 1;
            int32_t newDelay = (int32_t)((period * 3) >> 3);
            newDelay = clamp(newDelay, MIN_DELAY_SAMPLES, MAX_DELAY_SAMPLES);
            // Smooth towards target (1/256 per sample ≈ 5ms convergence)
            delayTimeSamples += (newDelay - (int32_t)delayTimeSamples + 128) >> 8;
        }
        else
        {
            // Slowly drift back to default 500ms (120 BPM)
            int32_t diff = DEFAULT_DELAY_SAMPLES - (int32_t)delayTimeSamples;
            if (diff > 0) delayTimeSamples++;
            else if (diff < 0) delayTimeSamples--;
        }

        // ---- Read audio input, sum to mono ----
        int32_t inL = AudioIn1();
        int32_t inR = AudioIn2();
        int32_t monoIn = (inL + inR) >> 1;

        // Write dry input to shared state for core 1 sidechain, then wake it
        sidechain.dry_mono = monoIn;
        sidechain.sample_tick++;
        __sev();  // Wake core 1 from __wfe() sleep — zero cost if already awake

        // ---- Input high-pass filter @ ~80Hz ----
        // Removes sub-bass rumble before the delay to prevent muddy
        // feedback buildup. Coefficient 684 ≈ 80Hz at 48kHz.
        int32_t filtered = filter_hp(&inputHPF, 684, monoIn);

        // ---- No compressor ----
        // The input signal passes through with only the HPF applied.
        // Previous compressor versions caused distortion — likely due
        // to the per-sample division on the M0+ (no hardware divider)
        // causing ISR timing overruns. The delay sounds clean without
        // compression; the feedback path's own clamping handles levels.
        (void)compEnvelope;
        int32_t compressed = filtered;

        // ---- Gate + sample buffer ----
        // Three modes based on switch position:
        //   DOWN (momentary): capture audio live AND feed it to the delay
        //   MIDDLE (default): sample is locked; Pulse In 2 replays it
        //   UP (hold):        Pulse In 2 captures fresh + feeds delay
        //
        // During tap tempo: skip all gate/sample/pulse2 processing.
        // The switch is used exclusively for tap detection, and Pulse In 2
        // must be ignored to prevent sample triggers interfering with tones.
        bool currentPulse2 = PulseIn2();
        if (pulse2Lockout > 0) pulse2Lockout--;
        bool pulse2Rising = currentPulse2 && !lastPulse2 && (pulse2Lockout == 0);
        if (pulse2Rising) pulse2Lockout = 2400;  // 50ms debounce
        lastPulse2 = currentPulse2;

        int32_t delayInput = 0;

        if (ttState != TT_OFF)
        {
            // Tap tempo active — no gate, no sample buffer, no Pulse In 2.
            // delayInput stays 0, delay tail rings out naturally.
        }
        else if (switchPos == Switch::Down)
        {
            // Momentary capture: start recording on switch press,
            // feed audio directly to the delay while held
            if (!lastSwitchDown)
            {
                samplebuf_start_capture(&sampleBuf);
                sampleTrigFlash = 4800;  // Flash LED 3 on capture start
            }
            if (sampleBuf.capturing) samplebuf_write(&sampleBuf, (int16_t)compressed);
            // Fade-in: ramp gate envelope up over 64 samples (~1.3ms)
            if (gateEnvelope < 64) gateEnvelope++;
            delayInput = (compressed * gateEnvelope) >> 6;
        }
        else if (switchPos == Switch::Middle)
        {
            // Locked sample mode: stop any active capture, then
            // replay the stored sample on each Pulse In 2 trigger
            if (sampleBuf.capturing) samplebuf_stop_capture(&sampleBuf);
            if (pulse2Rising && density_check(chaosKnob))
            {
                chaos_roll(&sampleBuf, chaosKnob);
                samplebuf_trigger_play(&sampleBuf);
                sampleTrigFlash = 4800;  // ~100ms flash for LED 3
            }
            // Handle ratchet re-triggers (auto-stutter from chaos engine)
            if (sampleBuf.ratchetCountdown > 0)
            {
                sampleBuf.ratchetCountdown--;
                if (sampleBuf.ratchetCountdown == 0 && sampleBuf.playing)
                {
                    chaos_roll(&sampleBuf, chaosKnob);
                    samplebuf_trigger_play(&sampleBuf);
                    sampleTrigFlash = 4800;  // ~100ms flash for LED 3
                }
            }
            delayInput = samplebuf_read(&sampleBuf);
        }
        else // Switch::Up
        {
            // Pulse 2 MUTED. Flipping to Up disables all Pulse In 2
            // triggering — the delay just runs with its current feedback
            // content. Your captured sample is preserved, so flipping
            // back to Middle resumes glitching exactly where you left off.
            //
            // This is a performance tool: flip up to silence the chaos
            // and let the delay tail ring out naturally, then flip back
            // down to Middle when you want the glitches to kick in again.
            //
            // No delayInput is set here, so the delay only recirculates
            // its existing content (feedback path still active).
        }
        lastSwitchDown = (switchPos == Switch::Down);

        // Gate envelope fade-out: when switch leaves Down, ramp the input
        // to silence over 64 samples (~1.3ms) to avoid a click at the cut.
        if (switchPos != Switch::Down && gateEnvelope > 0)
        {
            gateEnvelope--;
            // Continue feeding fading audio during the ramp-down
            delayInput = (compressed * gateEnvelope) >> 6;
        }

        // ---- Feedback amount (Main knob, custom curve) ----
        int32_t fbAmount = get_feedback(mainKnob);

        // ---- Read from delay lines (with tape wow) ----
        // Each channel's read position is modulated by a slow sine LFO
        // at a different rate, creating gentle independent pitch drift
        // between L and R — like two slightly wonky tape machines.
        //
        // The modulation is bipolar (centred on zero), so the average
        // delay time stays exactly on tempo. The pitch wobbles up and
        // down equally, keeping things musical.
        //
        // Rates: L = 1.0Hz, R = 1.3Hz (different enough to never sync)
        // Fast enough to hear as obvious pitch wobble, slow enough
        // to feel like a worn tape transport rather than a chorus.
        //
        // Depth: ±128 samples = ±2.67ms at 48kHz.
        // Peak pitch shift = depth × rate × 2π / sampleRate:
        //   L: 128 × 1.0 × 6.28 / 48000 ≈ 1.7% ≈ ±29 cents
        //   R: 128 × 1.3 × 6.28 / 48000 ≈ 2.2% ≈ ±37 cents
        // Audible on the first repeat, nicely woozy after a few
        // feedback passes. Compounds through the loop.
        //
        // Phase increment = freq * 2^32 / 48000
        //   1.0Hz → 89478      1.3Hz → 116322
        wowPhaseL += 89478;
        wowPhaseR += 116322;

        uint32_t baseDelay16 = (uint32_t)delayTimeSamples << 16;

        // sine_lookup returns ±4096. We want ±128 samples in 16.16 format:
        //   ±128 samples = ±8388608 in 16.16 (128 << 16 = 8388608)
        //   Scale: ±4096 * 2048 = ±8388608. So multiply by 2048.
        int32_t wowOffsetL = sine_lookup(wowPhaseL) * 2048;  // ±8388608 (±128 samples in 16.16)
        int32_t wowOffsetR = sine_lookup(wowPhaseR) * 2048;

        // ---- Tape glitches: random transport irregularities ----
        // Models sticky capstans, worn rollers, momentary tape slips.
        // Each glitch is a brief (~2-8ms) sudden shift in the delay
        // read position — you hear it as a quick pitch "whoop" or
        // "hiccup". The feedback knob controls how often they happen:
        // below unity they're very rare (once every ~5s); as you push
        // into bloom territory the tape machine "struggles" more and
        // glitches become frequent (every ~0.5s at max).
        //
        // Each channel glitches independently for organic stereo feel.
        // A cooldown prevents glitches clustering unnaturally.

        // Tick down cooldowns
        if (glitchCooldownL > 0) glitchCooldownL--;
        if (glitchCooldownR > 0) glitchCooldownR--;

        // Glitch probability scales with feedback amount.
        // fbAmount is 0-1085 (10-bit). We check every 4096 samples
        // (~85ms) and roll against a small threshold:
        //   below ~50% feedback: no glitches at all
        //   at unity (1024):     very rare → ~1 in 350 checks → ~30s avg
        //   at max bloom (1085): most frequent → ~1 in 58 checks → ~5s avg
        {
            uint32_t glitchChance;
            if (fbAmount < 512)
                glitchChance = 0;  // No glitches below ~50% feedback
            else if (fbAmount < 1024)
                glitchChance = (uint32_t)(fbAmount - 512) / 72;  // 0→7
            else
                glitchChance = 7 + (uint32_t)(fbAmount - 1024) * 37 / 61;  // 7→44
            // glitchChance is 0..44. Compare against rand >> 19 (0..8191).
            // At max: 44/8192 ≈ 1/186 per check. Check every ~85ms →
            // average gap = 186 × 85ms ≈ 16s... but per channel, so
            // you'll hear a glitch on either side roughly every ~5-8s.
            // At unity: 7/8192 ≈ 1/1170 → ~100s per channel → very rare.

            // Check every 4096 samples (~85ms)
            if ((clockCounter & 0xFFF) == 0)
            {
                // Left channel
                if (glitchCountdownL == 0 && glitchCooldownL == 0)
                {
                    if ((fast_rand() >> 19) < glitchChance)
                    {
                        // Start a glitch: duration 2400-9600 samples (50-200ms).
                        // Long enough for the pitch drift to be heard as a
                        // "whoop" or "waver" rather than a click.
                        glitchTotalL = 2400 + (int32_t)(fast_rand() % 7200);
                        glitchCountdownL = glitchTotalL;
                        // Peak offset: ±16 to ±64 samples in 16.16 format.
                        // Gentler than the wow depth — these are brief
                        // speed irregularities, not wild lurches.
                        int32_t depth = 16 + (int32_t)(fast_rand() % 48);
                        glitchOffsetL = (fast_rand() & 1) ? (depth << 16) : -(depth << 16);
                        // Cooldown: at least 14400 samples (300ms)
                        glitchCooldownL = 14400;
                    }
                }
                // Right channel (independent roll)
                if (glitchCountdownR == 0 && glitchCooldownR == 0)
                {
                    if ((fast_rand() >> 19) < glitchChance)
                    {
                        glitchTotalR = 2400 + (int32_t)(fast_rand() % 7200);
                        glitchCountdownR = glitchTotalR;
                        int32_t depth = 16 + (int32_t)(fast_rand() % 48);
                        glitchOffsetR = (fast_rand() & 1) ? (depth << 16) : -(depth << 16);
                        glitchCooldownR = 14400;
                    }
                }
            }
        }

        // Apply active glitches as a temporary tape speed change.
        // The glitch is a smooth speed deviation that drifts the read
        // position away then brings it back — like a sine half-wave
        // of rate change. The READ POSITION offset is the running
        // integral of this rate curve, so it smoothly rises, peaks
        // at the halfway point, then returns to zero by the end.
        //
        // This creates audible pitch shift for the duration of the
        // glitch (pitch up in first half, pitch down in second half,
        // or vice versa) with no clicks at start or end because the
        // offset starts and ends at zero.
        //
        // We use a sine approximation: offset = peak × sin(π × t/total)
        // where t goes from 0 to total. The DERIVATIVE of this is the
        // pitch shift (cosine-shaped), giving smooth pitch variation.
        //
        // Approximated as a parabola: offset = peak × 4 × t/T × (1 - t/T)
        // which peaks at t=T/2 and is zero at t=0 and t=T.
        int32_t glitchL = 0, glitchR = 0;
        if (glitchCountdownL > 0)
        {
            // Parabolic envelope: peaks at midpoint, zero at both ends.
            // progress: 0→128→0 over the glitch duration (triangle-ish).
            //   First half: rises 0→128. Second half: falls 128→0.
            // Then: glitch = peakOffset × progress / 128
            //
            // This creates a smooth hump in the read position, whose
            // derivative (= pitch shift) is positive in the first half
            // and negative in the second — a smooth "whoop up then down"
            // (or vice versa depending on offset sign).
            int32_t elapsed = glitchTotalL - glitchCountdownL;
            int32_t half = glitchTotalL >> 1;
            int32_t progress;
            if (elapsed < half)
                progress = (elapsed * 128) / half;         // 0→128
            else
                progress = ((glitchTotalL - elapsed) * 128) / half;  // 128→0
            glitchL = (glitchOffsetL >> 7) * progress;     // peak offset at progress=128
            glitchCountdownL--;
        }
        if (glitchCountdownR > 0)
        {
            int32_t elapsed = glitchTotalR - glitchCountdownR;
            int32_t half = glitchTotalR >> 1;
            int32_t progress;
            if (elapsed < half)
                progress = (elapsed * 128) / half;
            else
                progress = ((glitchTotalR - elapsed) * 128) / half;
            glitchR = (glitchOffsetR >> 7) * progress;
            glitchCountdownR--;
        }

        // Apply wow offset to the delay read position.
        // Clamp to safe range: never read less than 256 samples back
        // (avoids reading near the write head) or more than the buffer.
        //
        // IMPORTANT: use int64 for the addition because at max delay
        // (32640 << 16 = 2.14B) plus wow offset (±8.4M), the sum can
        // overflow int32 (max 2.15B). The clamp brings it back to a
        // safe int32 range before we use it.
        int32_t minDelay16 = 256 << 16;  // Safety floor
        int32_t maxDelay16 = (int32_t)((MAX_DELAY_SAMPLES - 256) << 16);
        int64_t sumL = (int64_t)(int32_t)baseDelay16 + (int64_t)wowOffsetL;
        int64_t sumR = (int64_t)(int32_t)baseDelay16 + (int64_t)wowOffsetR;
        if (sumL < minDelay16) sumL = minDelay16;
        if (sumL > maxDelay16) sumL = maxDelay16;
        if (sumR < minDelay16) sumR = minDelay16;
        if (sumR > maxDelay16) sumR = maxDelay16;
        uint32_t readDelayL = (uint32_t)(int32_t)sumL;
        uint32_t readDelayR = (uint32_t)(int32_t)sumR;

        int32_t delOutL = delay_read_interp(&delayL, readDelayL);
        int32_t delOutR = delay_read_interp(&delayR, readDelayR);

        // ---- Feedback path ----
        // Apply feedback gain from the Main knob curve
        feedbackL = (delOutL * fbAmount) >> 10;
        feedbackR = (delOutR * fbAmount) >> 10;

        // Lowpass filter in the feedback loop: darkens each repeat,
        // simulating tape head high-frequency loss. Coefficient 62800
        // gives a cutoff around ~12kHz. This is set higher than a
        // typical tape (~8kHz) to compensate for the additional HF
        // loss introduced by the wow modulation — the interpolating
        // delay read acts as a mild lowpass when the read position
        // is moving, so the combined effect still sounds like natural
        // tape darkening without getting muddy.
        feedbackL = filter_lp(&feedbackLPF_L, 62800, feedbackL);
        feedbackR = filter_lp(&feedbackLPF_R, 62800, feedbackR);

        // Highpass filter in the feedback loop prevents low-frequency
        // rumble from accumulating. The wow modulation shifts energy
        // downward on each pass, and without this filter the bass
        // builds into a muddy pulsing throb. Coefficient 1022 gives
        // a cutoff around ~120Hz — clears out the sub-bass rumble
        // while leaving kick drums and bass notes intact.
        feedbackL = filter_hp(&dcBlockL, 1022, feedbackL);
        feedbackR = filter_hp(&dcBlockR, 1022, feedbackR);

        // Two narrow dips to tame resonant peaks from cumulative
        // LP+HP phase shift in the feedback loop.
        // Dip 1: fundamental at ~1178Hz (bandpass 1100-1260Hz, 20%)
        // Dip 2: 2nd harmonic at ~2360Hz (bandpass 2100-2700Hz, 15%)
        // Coefficients: 10155≈1260Hz, 8886≈1100Hz, 20525≈2700Hz, 16277≈2100Hz
        {
            int32_t hiL = filter_lp(&dipHi_L, 10155, feedbackL);
            int32_t loL = filter_lp(&dipLo_L, 8886, feedbackL);
            feedbackL -= ((hiL - loL) * 205) >> 10;  // 205/1024 ≈ 20%

            int32_t hiR = filter_lp(&dipHi_R, 10155, feedbackR);
            int32_t loR = filter_lp(&dipLo_R, 8886, feedbackR);
            feedbackR -= ((hiR - loR) * 205) >> 10;
        }
        {
            int32_t hiL = filter_lp(&dip2Hi_L, 20525, feedbackL);
            int32_t loL = filter_lp(&dip2Lo_L, 16277, feedbackL);
            feedbackL -= ((hiL - loL) * 154) >> 10;

            int32_t hiR = filter_lp(&dip2Hi_R, 20525, feedbackR);
            int32_t loR = filter_lp(&dip2Lo_R, 16277, feedbackR);
            feedbackR -= ((hiR - loR) * 154) >> 10;
        }

        // ---- Write to delay ----
        // Stereo cross-feed: full same-channel + 3% bleed from opposite.
        // Just enough to gently widen the stereo image over many repeats
        // without smearing the field or adding loop gain. The wow LFOs
        // already create stereo decorrelation (different rates per channel),
        // so we only need a hint of cross-feed on top of that.
        // Total loop gain contribution: ~103% — close enough to unity
        // that the feedback LUT doesn't need to compensate.
        //
        // Soft saturation at the write point: catches the sum of input +
        // feedback + crossfeed. Knee at ±1800, 2:1 above, hard limit at
        // ±2047. Tames peaks that would hard-clip on the DAC, and naturally
        // limits whichever frequency is "winning" in the feedback loop.
        int32_t writeL = delay_soft_clip(delayInput + feedbackL + (feedbackR >> 5));
        int32_t writeR = delay_soft_clip(delayInput + feedbackR + (feedbackL >> 5));
        delay_write(&delayL, (int16_t)writeL);
        delay_write(&delayR, (int16_t)writeR);

        // ---- Output mix ----
        // Output is delay-only. No dry signal passthrough.
        // This card is designed to be wired in a feedback loop (mixer
        // send/return or similar), so passing the input audio straight
        // to the output would cause howling. The audio enters the delay
        // buffer, and you hear it after the delay time has elapsed.
        // A web config option could enable dry passthrough for users
        // who want to use this as an end-of-chain effect.
        // Compensation for energy lost in the feedback EQ dips (~3%).
        // Applied at the output, NOT inside the feedback loop — putting
        // makeup gain inside the loop compounds every round trip and
        // destabilises the system. At the output it's a one-time boost.
        int32_t outL = clamp((delOutL * 1055) >> 10, -2047, 2047);
        int32_t outR = clamp((delOutR * 1055) >> 10, -2047, 2047);

        // ---- Ring modulator (Y knob / CV In 2) ----
        // Y knob controls both the carrier frequency AND the ring mod
        // amount in a single sweep. At zero: pure delay. As you turn
        // up, the carrier starts at ~1Hz (tremolo) and sweeps to
        // ~2kHz (metallic), while the ring mod signal is layered on
        // top of the dry delay — the delay tone is always present.
        //
        // CV In 2 = direct replacement for Y knob.
        //   No cable: Y knob works directly.
        //   Cable: CV In 2 replaces the Y knob value (unipolar,
        //   0V = Y at zero, +5V = Y at max). Same behaviour as
        //   turning the knob — controls both frequency and wet/dry.
        //   Ideal for the Workshop 4-button keyboard to flip between
        //   ring mod sweet spots.
        //
        // CV In 1 = frequency modulation of the carrier.
        //   Adds ±50% of the current frequency for evolving textures.
        {
            int32_t yKnob;
            if (!Disconnected(Input::CV2))
            {
                int32_t cv2 = CVIn2();  // ±2047
                if (cv2 < 0) cv2 = 0;
                int32_t cvScaled = (cv2 * 4095) / 2047;  // 0→4095
                yKnob = (cvScaled * reverbKnob) >> 12;    // Y knob attenuates
            }
            else
            {
                yKnob = reverbKnob;
            }

            if (yKnob > 100)
            {
                // Ring mod mix from knob position.
                // Ring mod mix: Y knob controls dry/wet balance.
                // The dry signal fades gently while ring mod comes in,
                // keeping the delay tone always present underneath.
                //
                // At low carrier frequencies (tremolo range), the ring mod
                // wet signal averages near zero because the sine carrier
                // crosses through zero regularly. So dry must stay high
                // in the first half to avoid a volume dip in the middle.
                //
                // Dry/wet curves:
                //   Y   0%: dry 100%, wet  0%  = 100% total
                //   Y  25%: dry  95%, wet 30%  = 125% total (tremolo zone)
                //   Y  50%: dry  80%, wet 45%  = 125% total (sweet spot)
                //   Y 100%: dry  40%, wet 60%  = 100% total
                //
                // Dry: piecewise — stays high in first half, ducks in second.
                // Wet: piecewise — fast ramp to 45% at midpoint, then
                // gentle climb to 60% at full.
                int32_t yNorm = ((yKnob - 100) * 4096) / 3995;  // 0–4096
                if (yNorm > 4096) yNorm = 4096;

                // Dry: piecewise — gentle duck first half, steeper second.
                //   First half:  4096 → 3277 (100% → 80%)
                //   Second half: 3277 → 1638 (80% → 40%)
                int32_t dryMix;
                if (yNorm < 2048)
                    dryMix = 4096 - ((yNorm * 819) >> 11);   // 100%→80%
                else
                    dryMix = 3277 - (((yNorm - 2048) * 1639) >> 11);  // 80%→40%

                // Wet: piecewise — fast ramp to 45% at midpoint, then
                // gentle climb to 60% at full Y.
                int32_t ringAmount;
                if (yNorm < 2048)
                    // First half: 0→1843 (0%→45%) — steep ramp
                    ringAmount = (yNorm * 1843) >> 11;
                else
                    // Second half: 1843→2458 (45%→60%) — gentle climb
                    ringAmount = 1843 + (((yNorm - 2048) * 615) >> 11);

                // Base carrier frequency from Y knob position.
                // Three zones for good resolution across the range:
                //   100-1365:  1-16Hz    (tremolo / slow pulse)
                //   1365-2730: 16-256Hz  (warble / low ring mod)
                //   2730-4095: 256-2048Hz (metallic ring mod)
                //
                // We work in freq×16 to keep integer precision without
                // overflowing 32 bits (max: 32768 × 89478 = 2.9B < 4.2B).
                int32_t freq_x16;

                if (yKnob < 1365)
                {
                    // 1–16Hz: tremolo range
                    freq_x16 = 16 + ((yKnob - 100) * 240 / 1265);
                }
                else if (yKnob < 2730)
                {
                    // 16–256Hz: warble range
                    freq_x16 = 256 + ((yKnob - 1365) * 3840 / 1365);
                }
                else
                {
                    // 256–2048Hz: metallic range
                    freq_x16 = 4096 + ((yKnob - 2730) * 28672 / 1365);
                }

                // CV In 1 modulates the carrier frequency.
                // Adds ±50% of the current frequency, so low Y + CV
                // gives gentle tremolo sweeps, high Y + CV gives wild
                // metallic warping. Clamp to keep things positive.
                if (!Disconnected(Input::CV1))
                {
                    int32_t cv = CVIn1();  // ±2047
                    // ±50% modulation: cv/2047 * freq/2 = cv * freq / 4094
                    int32_t mod = (cv * freq_x16) / 4094;
                    freq_x16 += mod;
                    if (freq_x16 < 1) freq_x16 = 1;  // Don't go negative/zero
                    if (freq_x16 > 32768) freq_x16 = 32768;  // Cap at 2kHz
                }

                // Phase increment: freq_x16 * 89478 >> 4
                uint32_t phaseInc = (uint32_t)(freq_x16 * 89478u) >> 4;
                ringmod_phase += phaseInc;

                // Carrier: sine lookup returns ±4096
                int32_t carrier = sine_lookup(ringmod_phase);

                // Ring mod at unity gain: signal × carrier >> 12.
                // carrier peak is ±4096, so 2047 × 4096 >> 12 = 2047.
                int32_t wetL = (outL * carrier) >> 12;
                int32_t wetR = (outR * carrier) >> 12;

                // Blend: dry fades gently (100%→50%) as ring mod
                // comes in (0%→100%). The delay tone always stays
                // audible — ring mod adds texture on top, not instead.
                outL = ((outL * dryMix) >> 12) + ((wetL * ringAmount) >> 12);
                outR = ((outR * dryMix) >> 12) + ((wetR * ringAmount) >> 12);
            }
        }

        // ---- 8-band multiband compressor (output only) ----
        // Switch Up = bypass compressor (for A/B testing).
        // Gain computation runs decimated (every 10 samples ≈ 4.8kHz).
        // Band splitting + gain application runs every sample.
        if (switchPos == Switch::Up)
        {
            // Bypass: force all bands to unity gain
            for (int i = 0; i < NUM_BANDS; i++)
                mb.band_gain[i] = 1024;
        }
        else
        {
            mb.decimCounter++;
            if (mb.decimCounter >= MULTIBAND_DECIM)
            {
                mb.decimCounter = 0;
                int32_t wetMono = (clamp(outL, -2047, 2047) + clamp(outR, -2047, 2047)) >> 1;
                multiband_update_gains(&mb, wetMono);
            }
        }

        // Split wet stereo into 8 bands, apply per-band gain, recombine.
        // Serial subtraction: band[i] = LP(residual), residual -= band[i].
        // All bands sum back to exactly the original signal at unity gain.
        // Combined gain = compressor gain × sidechain duck gain (both Q10).
        {
            int32_t finalL = 0, finalR = 0;
            int32_t residualL = outL;
            int32_t residualR = outR;

            for (int i = 0; i < NUM_CROSSOVERS; i++)
            {
                int32_t bandL = filter_lp(&mb.wet_filters_L.crossover[i],
                                           crossover_coeffs[i], residualL);
                int32_t bandR = filter_lp(&mb.wet_filters_R.crossover[i],
                                           crossover_coeffs[i], residualR);
                residualL -= bandL;
                residualR -= bandR;

                // Combine compressor + sidechain gains (both Q10, multiply → Q20, shift back)
                int32_t comp_g = mb.band_gain[i];
                int32_t duck_g = sidechain.duck_gain[i];
                int32_t combined = (comp_g * duck_g) >> 10;

                // Smooth the combined gain to prevent zipper noise (~1ms)
                int32_t g = filter_lp(&mb.gain_smooth[i], GAIN_SMOOTH_COEFF,
                                       combined);
                finalL += (bandL * g) >> 10;
                finalR += (bandR * g) >> 10;
            }
            // Last band = residual (everything above 12kHz)
            {
                int32_t comp_g = mb.band_gain[NUM_CROSSOVERS];
                int32_t duck_g = sidechain.duck_gain[NUM_CROSSOVERS];
                int32_t combined = (comp_g * duck_g) >> 10;
                int32_t g = filter_lp(&mb.gain_smooth[NUM_CROSSOVERS],
                                       GAIN_SMOOTH_COEFF,
                                       combined);
                finalL += (residualL * g) >> 10;
                finalR += (residualR * g) >> 10;
            }

            outL = finalL;
            outR = finalR;
        }

        // Mix metronome tone during tap tempo (mono, both channels)
        if (ttState != TT_OFF) {
            int32_t tone = tt_generate_tone();
            outL += tone;
            outR += tone;
        }

        AudioOut1((int16_t)clamp(outL, -2047, 2047));
        AudioOut2((int16_t)clamp(outR, -2047, 2047));

        // ---- CV outputs ----

        // CV Out 1: Envelope follower on the delay output.
        // Tracks the peak level of the stereo delay output with a
        // fast attack / slow release. Since the delay is tempo-synced,
        // this naturally pulses in time with the music — patch it to
        // a filter cutoff and it breathes with the delay repeats.
        {
            int32_t absOL = outL < 0 ? -outL : outL;
            int32_t absOR = outR < 0 ? -outR : outR;
            int32_t peak = absOL > absOR ? absOL : absOR;
            if (peak > envFollower)
                envFollower = peak;  // Instant attack
            else
                envFollower -= (envFollower >> 11);  // Slow release (~43ms to half)
            CVOut1((int16_t)envFollower);  // 0 to +2047
        }

        // CV Out 2: Tempo-synced LFO (1 cycle per bar = 4 beats).
        // Sine wave locked to the clock. When no clock is present,
        // free-runs at ~0.5Hz (roughly 1 bar at 120 BPM).
        // Patch to filter, VCA, or other modulation destinations
        // for movement that's always in time with the delay.
        {
            uint32_t lfoInc;
            if (clockSynced)
                // 1 cycle per 4 beats: increment = 2^32 / (clockPeriod * 4)
                lfoInc = (uint32_t)(1073741824u / clockPeriod);  // 2^30 / period
            else
                lfoInc = 44739;  // ~0.5Hz free-running (2^32 / 48000 / 2)
            syncedLfoPhase += lfoInc;
            CVOut2((int16_t)(sine_lookup(syncedLfoPhase) >> 1));  // ±2048
        }

        // ---- Pulse outputs ----

        // Pulse Out 1: 1 pulse per bar (every 4 quarter notes).
        // Useful for resetting sequencers or triggering events on
        // bar boundaries. Pulse width = 25% of one beat.
        if (barPulseHold > 0)
        {
            barPulseHold--;
            if (barPulseHold == 0) barPulseState = false;
        }
        PulseOut1(barPulseState);

        // Pulse Out 2: 16th notes (4× the clock rate).
        // Generated by dividing each quarter note period into 4.
        // Useful for clocking fast sequencers, hi-hat triggers, etc.
        if (clockSynced)
        {
            // Phase accumulator: wraps 4 times per quarter note
            // sixteenthPeriod = clockPeriod / 4
            uint32_t sixteenthPeriod = clockPeriod >> 2;
            beatCounter++;
            if (beatCounter >= sixteenthPeriod)
            {
                beatCounter = 0;
                sixteenthState = true;
            }
            // Pulse width: 50% duty cycle
            PulseOut2(beatCounter < (sixteenthPeriod >> 1));
        }
        else
        {
            // No clock: output 16ths at ~120 BPM = 8Hz
            // Period = 48000/8 = 6000 samples
            beatCounter++;
            if (beatCounter >= 6000) beatCounter = 0;
            PulseOut2(beatCounter < 3000);
        }

        // ---- LEDs ----
        // LED layout (as viewed from front):
        //   | 0 1 |   0 = output clipping (near DAC rails)
        //   | 2 3 |   1 = output level meter (brightness)
        //   | 4 5 |   2 = Pulse In 2 activity
        //                  3 = sample trigger flash
        //                  4 = Pulse In 1 (clock) activity
        //                  5 = bar pulse (1 per 4 beats)

        // Flash LED 3 on sample trigger (not continuous envelope)
        if (sampleTrigFlash > 0) sampleTrigFlash--;

        // Input clip indicator: LED 0 lights when the raw audio input
        // is near the ADC rails (±1950 of ±2047). This helps with gain
        // staging — if it's lighting up, turn down your source signal.
        // Holds for ~100ms so you can see it flash on transients.
        int32_t absInL = inL < 0 ? -inL : inL;
        int32_t absInR = inR < 0 ? -inR : inR;
        int32_t absInPeak = absInL > absInR ? absInL : absInR;
        if (absInPeak > 1950) clipHold = 4800;  // ~100ms hold
        if (clipHold > 0) clipHold--;

        ledCounter++;
        if (ledCounter >= 480)   // Update LEDs at 100Hz
        {
            ledCounter = 0;

            if (ttState != TT_OFF) {
                // Tap tempo controls LEDs — bypass normal display
                tt_update_leds();
            } else {
                // Peak-hold envelope for output level metering
                int32_t absOL2 = outL < 0 ? -outL : outL;
                int32_t absOR2 = outR < 0 ? -outR : outR;
                int32_t peak2 = absOL2 > absOR2 ? absOL2 : absOR2;
                peakLevel = (peakLevel * 240) >> 8;  // Slow decay
                if (peak2 > peakLevel) peakLevel = peak2;

                // LED 0: clip warning
                LedOn(0, clipHold > 0);

                // LED 1: output level brightness
                int32_t levelBright = peakLevel << 1;
                if (levelBright > 4095) levelBright = 4095;
                LedBrightness(1, levelBright);

                // LED 2: Pulse In 2 activity
                LedOn(2, currentPulse2);

                // LED 3: sample trigger flash (bright for ~100ms on trigger)
                LedOn(3, sampleTrigFlash > 0);

                // LED 4: Pulse In 1 — brief flash on each rising edge.
                // Shows clock activity whether synced or not.
                // clockCounter resets to 0 on each rising edge, so
                // clockCounter < 2400 gives a ~50ms flash per beat.
                LedOn(4, clockCounter < 2400);

                // LED 5: bar pulse — brief flash on beat 1 of each bar.
                // barPulseHold counts down from a short value.
                // Using 2400 samples = ~50ms flash.
                LedOn(5, barPulseHold > 0);
            }
        }
    }
};


// ============================================================================
// Core 1: Sidechain ducking loop
// ============================================================================
// Runs on the second RP2040 core. Reads dry input from shared volatiles,
// splits into 8 frequency bands, computes per-band envelope followers,
// and writes duck_gain values back to shared state for core 0 to apply.
//
// Startup safety: waits for core 0's 2-second startup mute to complete
// before processing any audio. This prevents noise from uninitialised
// filters/buffers being interpreted as envelope data.
//
// Sleep strategy: uses __wfe() (ARM wait-for-event) to sleep between
// processing cycles. Core 0 sends __sev() after each new sample.
// This means core 1 generates ZERO bus traffic while sleeping, unlike
// a spin-wait loop on a volatile that constantly reads SRAM.

static void core1_sidechain_entry()
{
    volatile SharedSidechain *sc = g_sidechain;
    SidechainState state;

    // Init core 1's private filter/envelope state
    for (int i = 0; i < NUM_CROSSOVERS; i++)
        filter_init(&state.dry_filters.crossover[i]);
    for (int i = 0; i < NUM_BANDS; i++)
        state.dry_env[i] = 0;
    state.last_tick = 0;

    // ---- STARTUP WAIT ----
    // Sleep until core 0 signals that audio is fully initialised.
    // This is the fix for the startup noise — core 1 does nothing
    // until the 2-second mute is over and real audio is flowing.
    while (!sc->ready)
        __wfe();

    // Drain any pending SEV events accumulated during startup mute
    __sev();  // Set event flag so next __wfe() doesn't block
    __wfe();  // Consume it immediately

    state.last_tick = sc->sample_tick;

    // ---- MAIN PROCESSING LOOP ----
    for (;;)
    {
        // Sleep until core 0 wakes us with __sev()
        __wfe();

        // Check if enough samples have elapsed for decimated processing
        uint32_t tick = sc->sample_tick;
        uint32_t elapsed = tick - state.last_tick;
        if (elapsed < SC_DECIM) continue;
        state.last_tick = tick;

        // Read dry mono from shared state
        int32_t dry_mono = sc->dry_mono;

        // Split dry signal into 8 frequency bands
        int32_t dry_bands[NUM_BANDS];
        int32_t residual = dry_mono;
        for (int i = 0; i < NUM_CROSSOVERS; i++)
        {
            dry_bands[i] = filter_lp(&state.dry_filters.crossover[i],
                                      crossover_coeffs[i], residual);
            residual -= dry_bands[i];
        }
        dry_bands[NUM_CROSSOVERS] = residual;

        // Per-band envelope follower + ducking gain computation.
        // Each band has its own threshold, depth, and attack/release —
        // bass ducks deep and slow, highs duck light and fast.
        for (int i = 0; i < NUM_BANDS; i++)
        {
            const BandDuckParams *p = &duck_params[i];

            // Envelope follower on dry signal (per-band attack/release)
            int32_t dry_abs = dry_bands[i] < 0 ? -dry_bands[i] : dry_bands[i];
            if (dry_abs > state.dry_env[i])
                state.dry_env[i] += ((dry_abs - state.dry_env[i])
                                     * p->attack_coeff + 32768) >> 16;
            else
                state.dry_env[i] += ((dry_abs - state.dry_env[i])
                                     * p->release_coeff + 32768) >> 16;

            // Ducking: if dry envelope > threshold, reduce gain proportionally
            int32_t env = state.dry_env[i];
            int32_t duck = 1024;  // Unity (no ducking)
            if (env > p->threshold)
            {
                int32_t excess = env - p->threshold;
                int32_t reduction = (excess * p->inv_range) >> 16;
                if (reduction > p->depth) reduction = p->depth;
                duck = 1024 - reduction;
                if (duck < (1024 - p->depth)) duck = 1024 - p->depth;
            }
            sc->duck_gain[i] = duck;
        }
    }
}


// ============================================================================
// Entry point
// ============================================================================

int main()
{
    // 144MHz reduces ADC tonal artifacts and gives more CPU headroom.
    set_sys_clock_khz(144000, true);

    // Static allocation — keeps large object off the small RP2040 stack.
    static Robodub robodub;
    robodub.Run();
}
