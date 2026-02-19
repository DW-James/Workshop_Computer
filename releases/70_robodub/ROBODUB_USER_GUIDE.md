# Robodub User Guide

**Stereo Sound Expander & Dub Delay**
For Music Thing Modular Workshop System Computer

---

## The short version:

Robodub is a fun, playable stereo audio delay effect designed for immediate, hands-on performance. 
You feed it small seeds of your audio signal and they grow, bloom, and get replayed in various ways to create hypnotic evolving soundscapes.
It's inspired by the Dub-Delay mixing technique, by the lovely feedback-rich Zen Delay pedal and by glitchy sample based effects like Qubit Data Bender or Hologram Microcosm.

**Stereo Tape-style delay**
 The big main knob is the feedback for the delay. Generally speaking, straight up 50% is unity where nothing is lost or gained, to the left the repeats will fade, to the right they will accumulate and bloom. Each channel has its own slow pitch wobble at a different rate, creating stereo drift like two slightly wonky tape machines. As the feedback increases, occasional random tape glitches (brief pitch hiccups) start to appear — rare at unity, more frequent as you push into bloom territory.

**12 bit crunchy Sample buffer**
When you send audio into the delay by pushing the toggle switch down, it is also saved as a short, crunchy 12 bit sample in the computer (2 sec mono, 1 sec stereo). Sending a signal into the Pulse 2 input then triggers repeatable playback of this sample into the delay loop, with chance and decoration randomisations dictated by the X knob position. Toggling the switch down at any point adds fresh dry signal to the delay and updates the saved sample.

**Ring Mod After-Effect**
The Y knob controls both the After Effect's Dry/Wet and Oscillator Frequency. It's a ring-mod style effect which adds tremolo, warble, or metallic overtones. It comes after the delay and is non-destructive to the delay loop. It is self contained, but can be additionally frequency modulated by external signals fed into CV1. CV2 input can be used to modulate the Y knob value. With a cable in CV2, the Y knob becomes the attenuator for the CV2 modulation.

**Multiband compressor**
Delays can get wild and overpowering. This delay has an 8-band compressor on the output which also references the input signal 4,800 times per second. This compressor accentuates the interesting tonal and spatial details in the delay feedback, controls wild spikes and dynamically sidechain-ducks those frequency bands most present in the Dry audio signal, allowing the final output of the effect to react to a mix and rhythmically EQ itself to adapt to fit in. So your main mix kicks and lead lines are still clear against the big wash of delay.

**Clock sync** 
Syncs to external clock signal into Pulse 1 input or manual tap tempo

**Two routing modes**: 
**Insert mode** **default** (wet/effect-only) for use as mixer send effect and for simultaneous connection to gear with audio IN and OUT where feedback loops would be problematic, eg: Synthstrom Deluge, Yamaha Reface CP, Roland S-1 
**End-of-chain mode** (dry+wet) Computer will output a full stereo mix. 

---


## Controls

### Knobs

| Knob | Function |
|------|----------|
| **Main** | **Feedback amount.** Below halfway: repeats fade. Halfway: sustain (unity). Above: bloom and growth up to 106%. Cross-feed adds ~3% stereo spread per repeat. |
| **X** | **Chaos** — Playback density and decoration probability. Decorations include ratchets, octave-up, reverse, two-octave sparkle. Each trigger rolls the dice. Effects stack. |
| **Y** | **Ring mod** — Dry wet mix and frequency sweep from ~1Hz tremolo to ~2kHz metallic. Some dry delay signal is always present underneath. |

### Switch

| Position | Behaviour |
|----------|-----------|
| **Down** (momentary) | **Capture + feed.** Hold down to feed audio into the delay. Release to lock the captured sample. |
| **Middle** | **Sample locked.** Pulse In 2 replays the captured sample - X knob position dictates chance and effects of playback. |
| **Up** | **Sample Mute switch.** Pulse In 2 is ignored. The delay tail rings out undisturbed. Also bypasses the multiband compressor for A/B comparison. |

### Inputs

| Input | Function |
|-------|----------|
| **Audio In 1** (top left) | Main audio input. See *Audio Routing* below. |
| **Audio In 2**  | Stereo right channel (optional). When a cable is detected, stereo input mode activates automatically. |
| **CV In 1** | Ring mod FM. Modulates the Ring mod effect carrier frequency. |
| **CV In 2** | Ring mod mix CV. Replaces the Y knob value when a cable is patched. |
| **Pulse In 1** | Clock input. Auto BPM detection. Delay time syncs to dotted-eighth note relative to the incoming clock. |
| **Pulse In 2** | Sample Trigger input. Behaviour depends on switch position (see above). |


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

Plug a single cable into **Audio In 1**. The signal is fed to both left and right delay lines, creating a stereo soundscape from a mono source.

**Getting the right level:** The audio input expects eurorack-level signals (roughly ±5V). If you're connecting an instrument, microphone, or line-level device directly, use the Workshop System's **Amplifier** module to boost the signal first. Watch the **clip indicator LED** (top left LED) and adjust the amplifier level to stay just below clipping.

### Stereo Input

Plug cables into both **Audio In 1** (left) and **Audio In 2** (right). Stereo mode activates automatically (top right LED lights up). Left and right channels feed their respective delay lines independently.

**Eurorack-level stereo** (e.g. another module's stereo output): Patch directly into Audio In 1 and Audio In 2. No boost needed.

**Line-level stereo** (e.g. phone, laptop, mixer output, drum machine): Use the Workshop System's **Stereo Input** module, which boosts the signal to eurorack level before it reaches Robodub. Again, watch the clip indicator and adjust your source volume so you're not hitting clipping, unless crackly noises are desirable.

### Insert Mode (default)

Output is **wet-only** (delay + effects). Designed for mixer send/return loops where the dry signal is already in the mix. This is the default mode.

### End-of-Chain Mode

Output is **dry + wet summed**. The dry signal bypasses the ring modulator and multiband compressor entirely, keeping it clean and uncoloured. For use as a standalone effect where Robodub is the only signal path and you want a full mix output. Configure this via the web interface.

---

## Tap Tempo

When no external clock is present on Pulse In 1, you can set the tempo manually:

1. **Enter tap tempo mode:** Flick the switch between **Up** and **Middle** three times within 2 seconds. 
You'll hear a short two-tone audio cue (high–low–low) play in a "Tap–Tem–po" rhythm to announce Tap Tempo mode.
2. **Tap the tempo:** Push the switch **Down** six times at the desired tempo (usually quarter notes). 
Each tap plays a click and lights an LED in the sequence.
3. **Confirmation:** After the sixth tap and all LEDs lit, you'll hear two automatic higher pitched confirmation clicks at the tapped tempo. 
The delay time is then set to a dotted-eighth note relative to your tapped quarter notes.
4. **Exit:** The pulse outputs (Pulse Out 1 and 2) begin firing at the tapped tempo and the delay is clocked.
5. **Override:** An external clock arriving on Pulse In 1 always overrides the tapped tempo.

The delay time is normalised to a usable BPM range (50-200 BPM). If your tapped tempo is outside this range, it's doubled or halved until it fits.

The audio cue/click volume for tap tempo sounds can be adjusted via the web configuration interface (1-10, default 7).

---

## Clock Sync

Patch a clock signal into **Pulse In 1**. Clock signals between 1ppqn and 24ppqn are automatically supported. The delay time locks to a dub-style dotted-eighth note (3/4 of a quarter note). The bar counter resets on the first beat, and Pulse Out 1/2 begin outputting bar and 16th-note pulses.

If the clock stops for more than 3 seconds, the last known tempo is maintained.

---

## Sample Buffer

The sample buffer is 12 bit 24khz, in the style of classic hardware samplers.
It starts to capture audio the moment the switch is held **Down** and replays it when triggered by Pulse In 2 (switch in **Middle** position).

- **Mono mode:** Up to 2 seconds at 24kHz (48,000 frames, 96KB)
- **Stereo mode:** Up to 1 second at 24kHz (24,000 frames, 96KB interleaved)
- **Chance and Decoration effects** (controlled by X knob): Chance of playback, ratchet (rapid repeats), octave-up, reverse playback, two-octave sparkle.

Generally speaking, the X knob minimum anticlockwise setting has minimum chance of firing and no decoration effects. As you turn it clockwise you progressively get higher chance and greater potential for decorations.
Switch Up position is a playable mute and the sampler will ignore trigger inputs.
---

## LEDs

| LED | Position | Meaning |
|-----|----------|---------|
| 0 | Top left | **Clip warning** — input signal is near clipping |
| 1 | Top right | **Stereo input mode indicator** — lights when Audio In 2 cable is detected |
| 2 | Middle left | **Pulse In 2** — mirrors the incoming pulse |
| 3 | Middle right | **Sample trigger** — flashes when sample playback is triggered |
| 4 | Bottom left | **Pulse In 1 / Clock** — mirrors the incoming clock pulse |
| 5 | Bottom right | **Output level** — brightness tracks the delay output envelope; flashes on bar pulse |

---

## Web Configuration

Robodub can be configured via a web browser using WebMIDI over USB.

### Requirements
- Chrome or Chromium-based browser (Edge, Brave, Opera, etc.)
- USB cable connected to the Workshop Computer front panel USB-C
- Works on Windows, Mac, Linux, and Android — **not iOS**

### Setup
1. Connect the Workshop Computer to your computer via USB
2. Open `robodub_config.html` in Chrome
3. The page will automatically detect the "Robodub" MIDI device
4. The connection indicator shows green when connected

### Settings

**Signal Routing:** Toggle between Insert mode (wet-only) and End-of-chain mode (dry+wet). See *Audio Routing* above for details on each mode.

**Tap Tempo Metronome (1-10, default 7):** Volume of the metronome tones played during tap tempo entry. Turn down if the clicks are too loud in the mix, or up if you can't hear them over the delay tail.

**Tape Warble (1-5, default 4):** Amount of pitch wobble on the delay repeats. Each channel has its own slow sine LFO at a different rate (L: 1.0Hz, R: 1.3Hz), creating independent stereo pitch drift like two slightly wonky tape machines. The wobble compounds through the feedback loop, so higher levels get increasingly woozy with each repeat.

| Level | Name | Depth | Pitch shift |
|-------|------|-------|-------------|
| 1 | Clean | ±0.33ms | ±6 cents |
| 2 | Subtle | ±1.0ms | ±13 cents |
| 3 | Moderate | ±1.67ms | ±18 cents |
| 4 | **Woozy** (default) | ±2.67ms | ±29 cents |
| 5 | Seasick | ±4.0ms | ±43 cents |

**Save to Flash:** Writes all current settings to flash memory so settings can persist across power cycles. There will be a brief audio glitch (~5ms) during the save.

### SysEx Protocol (for developers)

Manufacturer ID: `0x7D` (prototyping/private use)

| ID | Direction | Payload | Description |
|----|-----------|---------|-------------|
| `0x01` | web to fw | `[0x01, 0\|1]` | Set dry passthrough (0=insert, 1=end-of-chain) |
| `0x02` | web to fw | `[0x02]` | Save current config to flash |
| `0x03` | web to fw | `[0x03]` | Request current config state |
| `0x04` | fw to web | `[0x04, dry, metroVol, warble]` | Current config state |
| `0x05` | web to fw | `[0x05, 1-10]` | Set metronome volume |
| `0x06` | web to fw | `[0x06, 1-5]` | Set tape warble level |
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
| Wow modulation | L: 1.0Hz, R: 1.3Hz, configurable depth (5 levels) |
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
