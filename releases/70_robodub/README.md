# Robodub

A stereo sound expander, ambient backdrop generator, and playable dub instrument.

Feed in a small mono signal — a synth line, a drum hit, a voice — and Robodub turns it into an evolving stereo soundscape. Part King Tubby dub delay, part digital glitch machine, part ring modulator. Not just another tape delay simulator.

The **sampler** captures and mangles your audio with ratchets, octave-up bursts, and reverse playback. The **tape delay** smears it across stereo with pitch-drifting wow and controllable feedback bloom. The **ring modulator** adds tremolo, warble, or metallic overtones on top. Everything stays immediate and playable — three knobs and a switch.

**No dry signal passthrough** — designed as a send effect, safe with gear that lacks mixer sends (Deluge, Reface, grooveboxes, etc).

## Controls

| Control | Function |
|---------|----------|
| **Main knob** | Feedback amount. Below halfway: repeats fade. Halfway: sustain. Above: bloom and growth (up to 106%) |
| **X knob** | Chaos — glitch density and probability (ratchets, octave-up, reverse, two-octave sparkle) |
| **Y knob** | Ring mod — frequency sweep (1Hz tremolo → 2kHz metallic) with dry signal always present underneath |
| **Switch Down** | Capture audio AND burst into delay (hold to feed) |
| **Switch Middle** | Sample locked. Pulse In 2 replays with glitches from X knob |
| **Switch Up** | Pulse In 2 muted — delay tail rings out undisturbed. Performance kill switch |
| **Pulse In 1** | Clock input (quarter notes → dotted-eighth delay) |
| **Pulse In 2** | Trigger input (behaviour depends on switch position) |
| **CV In 1** | Ring mod FM — modulates the Y knob carrier frequency |
| **CV In 2** | External chaos control — X knob attenuates incoming CV. Patch a slow LFO for generative glitch swells |
| **CV Out 1** | Envelope follower on delay output — rhythmic CV that breathes with the delay repeats |
| **CV Out 2** | Tempo-synced LFO (1 cycle per bar). Free-runs at ~0.5Hz when no clock |
| **Pulse Out 1** | Bar clock (1 pulse per 4 beats) |
| **Pulse Out 2** | 16th notes (4× clock rate) |
| **Audio Out 1+2** | Stereo delay output (wet only) |

## Chaos Engine (X knob)

Inspired by Qubit Data Bender and Hologram Microcosm. Turn clockwise for more chaos:

| Range | Effect |
|-------|--------|
| 0-12% | Clean — no glitches |
| 12-25% | Occasional ratchets (stutter re-triggers) |
| 25-50% | Add octave-up bursts (double-speed playback) |
| 50-75% | Add reverse playback |
| 75-100% | Two-octave-up sparkle. Full chaos — effects stack randomly |

Each trigger rolls the dice. A reversed octave-up ratchet is entirely possible.

## Tape Delay

Clock-syncable stereo delay (dotted-eighth note timing). Each channel has independent tape wow at different rates, creating stereo pitch drift that compounds through the feedback loop — the longer the tail, the woozier it gets. Feedback LPF darkens each repeat like a worn tape head. High-pass filter at 120Hz prevents bass rumble accumulating.

## Ring Modulator (Y knob)

Post-delay character effect. The dry delay signal is always present — the ring mod adds texture on top, never replaces it. Three frequency zones as Y sweeps up: slow tremolo, mid-range warble, metallic ring mod. Patch an LFO or oscillator into CV In 1 for FM modulation of the carrier.

## Sample Buffer

24kHz / 12-bit capture (Akai S900 character). 1.5 seconds maximum. The lo-fi quantisation adds cumulative grit through the delay feedback path — each pass through the sampler degrades the signal further.

## LED Display

| LED | Function |
|-----|----------|
| 0 (top left) | Clipping warning (input near ADC rails) |
| 1 (top right) | Output level (brightness) |
| 2 (mid left) | Pulse In 2 activity |
| 3 (mid right) | Sample trigger flash |
| 4 (bot left) | Pulse In 1 (clock) activity |
| 5 (bot right) | Bar pulse (beat 1 of each bar) |

## Status

**Current:** Stereo delay with feedback bloom, tape wow, clock sync, sample capture/replay, chaos engine, ring modulator with CV FM.

**Planned:** Web editor for configuration.

## Build

Requires Pico SDK. Copy `pico_sdk_import.cmake` into the `source/` directory, then:

```
cd source
mkdir build && cd build
cmake ..
make
```

Flash the resulting `robodub.uf2` to a program card.
