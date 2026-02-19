# Robodub User Guide

**Stereo Sound Expander & Dub Delay**
For Music Thing Modular Workshop System Computer

---

## Overview

Robodub turns a small mono or stereo input into evolving stereo soundscapes. It combines a tape-style stereo delay with a sample mangler and ring modulator, all designed for immediate, hands-on performance.

- **Tape delay** with pitch-drifting wow, controllable feedback bloom, and stereo spread
- **Sample buffer** captures and mangles audio with ratchets, octave-up bursts, and reverse playback
- **Ring modulator** adds tremolo, warble, or metallic overtones
- **Clock sync** to external quarter-note pulse or manual tap tempo
- **Two routing modes**: insert (wet-only) for mixer sends, or end-of-chain (dry+wet) for standalone use

---

## Controls

### Knobs

| Knob | Function |
|------|----------|
| **Main** | **Feedback amount.** Below halfway: repeats fade. Halfway: sustain (unity). Above: bloom and growth up to 106%. Cross-feed adds ~3% stereo spread. |
| **X** | **Chaos** — glitch density and probability. Ratchets, octave-up, reverse, two-octave sparkle. Each trigger rolls the dice. Effects stack. |
| **Y** | **Ring mod** — frequency sweep from ~1Hz tremolo to ~2kHz metallic. The dry delay signal is always present underneath. |

### Switch

| Position | Behaviour |
|----------|-----------|
| **Down** (momentary) | **Capture + feed.** Hold to continuously feed audio into the delay. Release to lock the captured sample. |
| **Middle** | **Sample locked.** Pulse In 2 replays the captured sample with glitch effects from the X knob. |
| **Up** | **Kill switch.** Pulse In 2 is muted. The delay tail rings out undisturbed. |

### Inputs

| Input | Function |
|-------|----------|
| **Audio In 1** (top left) | Main audio input. Mono eurorack level or line level. |
| **Audio In 2** (bottom left) | Stereo right channel. When a cable is detected, stereo mode activates automatically with 8x gain boost for line level. |
| **Pulse In 1** | Clock input (quarter notes). Delay time syncs to dotted-eighth note relative to the incoming clock. |
| **Pulse In 2** | Trigger input. Behaviour depends on switch position (see above). |
| **CV In 1** | Ring mod FM. Modulates the Y knob carrier frequency. |
| **CV In 2** | Ring mod CV. Replaces the Y knob value when a cable is patched. |

### Outputs

| Output | Function |
|--------|----------|
| **Audio Out 1** | Left output (delay + ring mod + compressor). |
| **Audio Out 2** | Right output (delay + ring mod + compressor). |
| **CV Out 1** | Envelope follower tracking the delay output level. |
| **CV Out 2** | Tempo-synced triangle LFO (1 cycle per bar). |
| **Pulse Out 1** | Bar clock (1 pulse per 4 beats). |
| **Pulse Out 2** | 16th notes (4 pulses per beat). |

---

## Audio Routing

### Mono Input (default)
Plug a single cable into **Audio In 1**. The signal is fed to both left and right delay lines. Works at eurorack level directly.

### Stereo Input
Plug cables into both **Audio In 1** and **Audio In 2**. Stereo mode activates automatically (LED 1 lights up). An 8x gain boost is applied to bring line level signals up to eurorack range. Left and right channels feed their respective delay lines independently.

### Insert Mode (default)
Output is **wet-only** (delay + effects). Designed for mixer send/return loops where the dry signal is already in the mix. This is the default mode.

### End-of-Chain Mode
Output is **dry + wet summed**. For use as a standalone effect where Robodub is the only signal path (e.g., guitar pedal style). Configure this via the web interface.

---

## Tap Tempo

When no external clock is present on Pulse In 1, you can set the tempo manually:

1. **Enter tap tempo mode:** Flick the switch rapidly between **Up** and **Middle** three times within 2 seconds. You'll hear a short ascending tone.
2. **Tap the tempo:** Push the switch **Down** six times at the desired tempo. Each tap plays a click.
3. **Confirmation:** After the sixth tap, you'll hear two confirmation clicks at the tapped tempo. The delay time is set to a dotted-eighth note relative to your tapped quarter note.
4. **Exit:** The pulse outputs (Pulse Out 1 and 2) begin firing at the tapped tempo.
5. **Override:** An external clock arriving on Pulse In 1 always overrides the tapped tempo.

The delay time is normalised to a usable BPM range (50-200 BPM). If your tapped tempo is outside this range, it's doubled or halved to fit.

---

## Clock Sync

Patch a quarter-note clock into **Pulse In 1**. The delay time automatically locks to a dotted-eighth note (3/4 of a quarter note). The bar counter resets on the first beat, and Pulse Out 1/2 begin outputting bar and 16th-note pulses.

If the clock stops for more than 3 seconds, the last known tempo is maintained.

---

## Sample Buffer

The sample buffer captures audio when the switch is held **Down** and replays it when triggered by Pulse In 2 (switch in **Middle** position).

- **Mono mode:** Up to 2 seconds at 24kHz (48,000 frames, 96KB)
- **Stereo mode:** Up to 1 second at 24kHz (24,000 frames, 96KB interleaved)
- **Glitch effects** (controlled by X knob): ratchet (rapid repeats), octave-up, reverse playback, two-octave sparkle
- Playback includes crossfade for click-free looping

---

## LEDs

| LED | Position | Meaning |
|-----|----------|---------|
| 0 | Top left | **Clip warning** — input signal is near clipping (compensated for stereo 8x boost) |
| 1 | Top right | **Stereo indicator** — lights when Audio In 2 cable is detected |
| 2 | Middle left | **Pulse In 2** — mirrors the incoming pulse |
| 3 | Middle right | **Sample trigger** — flashes when sample playback is triggered |
| 4 | Bottom left | **Pulse In 1 / Clock** — mirrors the incoming clock pulse |
| 5 | Bottom right | **Output level** — brightness tracks the delay output envelope; flashes on bar pulse |

---

## Web Configuration

Robodub can be configured via a web browser using WebMIDI over USB.

### Requirements
- Chrome or Chromium-based browser (Edge, Brave, Opera, etc.)
- USB cable connected to the Workshop Computer
- Works on Windows, Mac, Linux, and Android — **not iOS**

### Setup
1. Connect the Workshop Computer to your computer via USB
2. Open `robodub_config.html` in Chrome
3. The page will automatically detect the "Robodub" MIDI device
4. The connection indicator shows green when connected

### Settings

**Signal Routing:** Toggle between Insert mode (wet-only) and End-of-chain mode (dry+wet).

**Save to Flash:** Writes current settings to flash memory. Settings persist across power cycles. There will be a brief audio glitch (~5ms) during the save.

### SysEx Protocol (for developers)

Manufacturer ID: `0x7D` (prototyping/private use)

| ID | Direction | Payload | Description |
|----|-----------|---------|-------------|
| `0x01` | web to fw | `[0x01, 0\|1]` | Set dry passthrough (0=insert, 1=end-of-chain) |
| `0x02` | web to fw | `[0x02]` | Save current config to flash |
| `0x03` | web to fw | `[0x03]` | Request current config state |
| `0x04` | fw to web | `[0x04, dryPassthrough]` | Current config state |
| `0x10` | fw to web | `[0x10, major, minor, patch]` | Firmware version |

---

## Technical Specs

| Parameter | Value |
|-----------|-------|
| Sample rate | 48 kHz |
| CPU clock | 192 MHz (RP2040, dual Cortex-M0+) |
| Bit depth | 12-bit ADC, 12-bit DAC |
| Delay buffer | 2 x 64KB (65536 samples per channel, ~1.37s) |
| Sample buffer | 96KB (2s mono / 1s stereo at 24kHz) |
| Input HPF | ~80 Hz (removes sub-bass mud before delay) |
| Feedback LPF | ~12 kHz (tape darkening) |
| Feedback HPF | ~120 Hz (DC blocker) |
| Wow modulation | L: 1.0Hz, R: 1.3Hz, depth ±128 samples |
| Ring mod range | ~1 Hz (tremolo) to ~2 kHz (metallic) |
| Startup mute | 2 seconds (allows filters/buffers to initialise) |
| USB | MIDI device (TinyUSB), appears as "Robodub" |
| Config storage | Last 4KB flash sector, 256-byte page |

### Architecture

- **Core 0:** Audio ISR (ProcessSample at 48kHz) + multiband compressor
- **Core 1:** Sidechain ducking (8-band, decimated to ~4.8kHz) + USB MIDI polling
- Core 1 uses `__wfe()`/`__sev()` for zero-cost sleep between processing cycles
- Flash writes use Pico SDK `flash_safe_execute` for multicore safety

---

*AI-generated (Claude) + James Robinson. Built on ComputerCard v0.2.8.*
