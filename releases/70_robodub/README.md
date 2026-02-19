# Robodub

**Stereo Sound Expander & Dub Delay**
For Music Thing Modular Workshop System Computer

---

Robodub is a fun, playable stereo audio delay effect designed for immediate, hands-on performance. You feed it small seeds of your audio signal and they grow, bloom, and get replayed in various ways to create hypnotic evolving soundscapes. It's inspired by the Dub-Delay mixing technique, by the lovely feedback-rich Zen Delay pedal and by glitchy sample-based effects like Qubit Data Bender or Hologram Microcosm.

- **Stereo tape-style delay** with configurable pitch wobble, controllable feedback bloom, and random tape glitches
- **12 bit crunchy sample buffer** captures and mangles audio with ratchets, octave-up bursts, reverse playback, and two-octave sparkle
- **Ring mod after-effect** adds tremolo, warble, or metallic overtones — non-destructive to the delay loop
- **8-band sidechain compressor** dynamically ducks the delay against your dry input so the effect sits in a mix
- **Clock sync** to external pulse or manual tap tempo
- **Two routing modes**: insert (wet-only) for mixer sends, or end-of-chain (dry+wet) for standalone use
- **Web configuration** via USB for signal routing, metronome volume, and tape warble depth

## Controls

| Control | Function |
|---------|----------|
| **Main knob** | Feedback amount. Below halfway: repeats fade. Halfway: sustain (unity). Above: bloom and growth up to 106%. |
| **X knob** | Chaos — playback density and decoration probability. Ratchets, octave-up, reverse, two-octave sparkle. Effects stack. |
| **Y knob** | Ring mod — dry/wet mix and frequency sweep from ~1Hz tremolo to ~2kHz metallic. |
| **Switch Down** | Capture + feed. Hold to feed audio into the delay and save a sample. |
| **Switch Middle** | Sample locked. Pulse In 2 replays the captured sample with X knob decorations. |
| **Switch Up** | Sample mute. Pulse In 2 is ignored. The delay tail rings out undisturbed. |
| **Audio In 1** | Main audio input (mono eurorack level, or use Amplifier module for instruments/line level). |
| **Audio In 2** | Stereo right channel. Stereo mode auto-detects. Use Stereo Input module for line-level sources. |
| **CV In 1** | Ring mod FM — modulates the carrier frequency. |
| **CV In 2** | Ring mod CV — replaces the Y knob value when patched. Y knob becomes attenuator. |
| **Pulse In 1** | Clock input. Auto BPM detection. Delay syncs to dotted-eighth note. |
| **Pulse In 2** | Sample trigger. Behaviour depends on switch position. |
| **Audio Out 1+2** | Stereo output (delay + ring mod + compressor). In end-of-chain mode, dry signal is summed in clean. |
| **CV Out 1** | Envelope follower on delay output — rhythmic CV that breathes with the repeats. |
| **CV Out 2** | Tempo-synced triangle LFO (1 cycle per bar). |
| **Pulse Out 1** | Bar clock (1 pulse per 4 beats). |
| **Pulse Out 2** | 16th notes (4 pulses per beat). |

## LEDs

| LED | Position | Meaning |
|-----|----------|---------|
| 0 | Top left | Clip warning — input signal is near clipping |
| 1 | Top right | Stereo input mode indicator |
| 2 | Middle left | Pulse In 2 — mirrors incoming pulse |
| 3 | Middle right | Sample trigger flash |
| 4 | Bottom left | Pulse In 1 / Clock — mirrors incoming clock |
| 5 | Bottom right | Output level — brightness tracks delay output; flashes on bar pulse |

## Tap Tempo

No external clock? Flick the switch between Up and Middle three times to enter tap tempo mode, then tap Down six times at your desired tempo. The delay locks to a dotted-eighth note and the pulse outputs start firing. Metronome volume is adjustable via the web config.

## Web Configuration

Connect via USB and open `robodub_config.html` in Chrome. Configure signal routing (insert vs end-of-chain), tap tempo metronome volume (1-10), and tape warble depth (5 levels from Clean to Seasick). Settings save to flash and persist across power cycles.

## Build

Requires Pico SDK with TinyUSB. From the `source/` directory:

```
mkdir build && cd build
cmake -G Ninja ..
ninja
```

Flash the resulting `robodub.uf2` to a Workshop System Computer card.

See `ROBODUB_USER_GUIDE.md` for the full manual.
