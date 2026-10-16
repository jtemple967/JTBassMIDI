# JTBassMIDI (LV2)

A monophonic bass-to-MIDI converter, the companion to JTBassSynth. It tracks the pitch
of a bass guitar and sends MIDI notes with velocity and optional pitch bend. The bass
also passes through to the audio output, so the plugin can sit inline on a pedalboard
(for example on a pi-Stomp) and drive a synth plugin or external gear.

## Signal flow

```
bass in ─┬───────────────────────────────────────── Thru ──> audio out
         │
         ├─ HPF ─ envelopes ─ gate / pluck / damp detector ──┐
         │                                                   ├──> note-on / note-off
         └─ HPF ─ LPF ─ ↓6 kHz ─ YIN pitch detector ─────────┴──> pitch bend   ──> MIDI out
```

* **Tracking:** the same YIN tracker and note-start rules as JTBassSynth. It runs on a copy
  of the input downsampled to about 6 kHz, analyses only audio since the latest pluck, and
  resists the octave errors that strong 2nd and 3rd harmonics cause. Its reference window
  is the newest audio, so the attack transient drops out of the analysis within 15 ms.
* **Note-on:** sent once three consecutive clear readings agree: within 0.12 semitone, or
  within 0.3 semitone and close to a semitone. A stray or flat early reading therefore
  never reaches the synth as a wrong first note. Velocity comes from the attack level.
* **Re-plucks:** a new pluck while the string still rings is detected as a sudden rise in
  level. It only counts once the previous note has started to decay, so a light pluck
  that swells in slowly isn't mistaken for two plucks. Right after a damp (see Note-off),
  a 12 dB rise from the bottom of the dip counts as the next pluck. That catches
  fingerstyle playing, where the next finger damps the string for 15–25 ms just before
  plucking. The old note keeps sounding until the new pitch is confirmed, then it is
  swapped, so legato synth patches don't gap.
* **Hammer-ons, pull-offs, slides and soft plucks:** a pitch change with no detected pluck
  becomes a new note once six clear readings in a row (about 16 ms) agree within 0.3
  semitone and sit near a semitone. Readings taken while the pitch is still moving, or
  while the next finger touches the string before a damp, keep drifting and never pass,
  so no stray note appears. Changes are measured against the sounding note, so a single
  odd reading followed by a return to the sounding note doesn't restart it.
* **Bends and vibrato:** with Bend Range set, gradual pitch changes are sent as pitch bend
  (median of the last three readings; changes under about 2 cents are not sent, so DIN
  MIDI isn't flooded). Each note starts with the bend centred, and bends only follow once
  the first readings have settled (about 10 ms). Each note also learns how clean its
  readings normally are, and readings much less clean are ignored. Those come about 30 ms
  before the next pluck, when the next finger touches the string. Bends are held back by
  about 11 ms and dropped if a damp is detected in that time, so the sharp drift in the
  last moments of a note never reaches the synth. Bending past the range starts a new
  note. With Bend Range Off, output is quantised to the nearest note.
* **Note-off:** when the string is damped, for example by lifting a finger off the fret,
  the plugin sees the level fall far faster than a ringing string decays. It stops
  following the pitch at once and sends the note-off 40 ms later, unless another pluck
  comes first. Then the old note runs straight into the new one, as on a re-pluck. A real
  stop ends the note about 65–75 ms after the finger lifts. Fret noise and the open
  string ringing underneath never turn into stray notes or bends. A note that just rings
  out ends when the level falls below the gate (half the Gate Threshold level). Neither
  applies while Hold is on.
* **Hold and Release:** with Hold on, the note keeps sounding after the string stops.
  The Latch switch sets what ends it:
  * **Latch off:** a different note replaces it (or Release ends it). Plucking the note
    that is already sounding doesn't restart it; it keeps sustaining.
  * **Latch on:** only Release ends it. Anything you play meanwhile is ignored: no new
    notes and no pitch bend. After Release, the next note you play latches.

  Release ends the note at once, even if the string is still ringing; the ringing string
  is then ignored until your next pluck. Release works with Hold off too, as a quick way
  to cut a note short.
* **Bypass:** the plugin uses the `lv2:enabled` port, so MOD's bypass footswitch releases
  any sounding note and resets pitch bend instead of leaving a hung note on the synth.
* **Startup:** the first run sends All Notes Off on the selected channel.

## Controls

| Control        | Range           | Notes |
|----------------|-----------------|-------|
| Gate Threshold | −80 to −10 dB   | Lower values make the gate more sensitive |
| Dynamics       | 0–1             | 0 sends every note at velocity 100; 1 follows picking strength fully |
| Bend Range     | Off, 1–24 semis | Match the receiving synth's pitch-bend range (2 is the usual default) |
| Thru           | 0–1             | Level of the bass passed to the audio output |
| Channel        | 1–16            | MIDI output channel |
| Transpose      | ±24 semitones   | +12 puts the bass an octave up, which suits many synth patches |
| Hold           | on / off        | Keeps the note sounding after the string stops, until a different note or Release |
| Release        | momentary       | Ends the sounding note |
| Latch          | on / off        | With Hold on: only Release ends the held note; other notes are ignored |

Hold and Release are buttons on the pedal face, with the Latch toggle between them.
On a pi-Stomp you can also assign them to footswitches: Hold and Latch as latching
switches, Release as a momentary one.

The plugin also has an output port, **Note**, that shows the MIDI note currently sounding
(−1 when none). It's useful for checking the tracking.

## Latency

Note-on time after the pluck, measured with `make test` (synthetic plucks, every fret
position, three tones):

| String | Average | Range |
|--------|---------|-------|
| B (5-string) | ~53 ms | 38–70 ms |
| E      | ~43 ms | 35–54 ms |
| A      | ~36 ms | 32–43 ms |
| D      | ~32 ms | 30–35 ms |
| G      | ~28 ms | 24–32 ms |

Add about 3–6 ms for the audio buffer on the pi-Stomp. The slowest notes are the open
and low-fret notes on each string.

## Build & install

You need a C compiler and the LV2 headers (`lv2-dev` on Debian/Ubuntu/Raspberry Pi OS).

```bash
git clone https://github.com/jtemple967/JTBassMIDI.git
cd JTBassMIDI
make
make install        # → ~/.lv2 (Linux), ~/Library/Audio/Plug-Ins/LV2 (macOS), %APPDATA%/LV2 (MinGW)
make test           # offline tracking test with synthetic plucks (see below)
```

URI: `https://github.com/jtemple967/JTBassMIDI`. It appears in MOD as *JT → Bass MIDI*.

### On a pi-Stomp (Raspberry Pi 5)

Build on the Pi itself, so the `.so` is native aarch64:

```bash
sudo apt install build-essential lv2-dev pkg-config
make
```

Copy the whole `JTBassMIDI.lv2` folder to the directory your pi-Stomp loads plugins from
(the one that already contains the other `.lv2` bundles, commonly `~/.lv2`). Then restart
the MOD services or reboot.

On the pedalboard:

1. Connect the pi-Stomp's input to JTBassMIDI's audio input, and its audio output onward
   (or turn Thru down if you only want the synth).
2. Connect **MIDI Out** to a synth plugin's MIDI input, or to the pi-Stomp's MIDI output
   for external gear (DIN module or USB).
3. Set Bend Range to match the synth's pitch-bend range, or Off if the synth ignores bend.
4. Try Transpose +12 if the synth patch sounds muddy at the bass's real pitch.

For the tightest feel, run the pi-Stomp at 64 or 128 frames and check for audio dropouts.

## Offline test

`tools/test_tracking.c` builds the plugin source directly, with no host needed. It
synthesises plucked notes (stiff-string partials with a pick click) in three tones: round,
bright, and a weak-fundamental tone that is the classic octave trap. It checks:

* every fret position on the B, E, A, D and G strings: right note, exactly one note-on,
  note-off after damping, and min/avg/max note-on latency
* re-plucks while the previous note rings
* a hammer-on with no new attack (no stray note in between)
* a light pluck that swells in over 40 ms, then a finger lifted off the fret with fret
  click and the open string ringing underneath: one note-on, note-off within 80 ms of the
  lift, no open-string note and no pitch bend from the tail (with bends on and off)
* a fingerstyle G major scale up and down, each note damped ~20 ms before the next pluck
  (modelled on a real recording): exactly the 15 notes in order, each replacing the last
  with no gap, no pitch bend over 15 cents
* a slow bend with Bend Range 2 (pitch bend, no retrigger) and with Bend Range Off
  (quantised new note)
* velocity rising with picking strength
* Hold and Transpose
* Hold: re-plucking the sounding note sends no new note-on; a different note does
* Latch: other notes and bends are ignored until Release, then the next note plays
* Release: ends a held note on the press, ends a still-ringing note without it restarting,
  and the next pluck still plays

It exits non-zero if anything fails. All checks pass. So far it has been compiled against
minimal stand-in LV2 headers; `make test` on the Pi builds it against the real ones.

## MOD interface

| File | Purpose |
|------|---------|
| `modgui.ttl` | Points MOD at the template, stylesheet, screenshot and thumbnail |
| `modgui/icon-jtbassmidi.html` | Pedal template (knobs, Hold/Release buttons, Latch toggle, footswitch, LED, audio and MIDI jacks) |
| `modgui/stylesheet-jtbassmidi.css` | Control positions and sprites |
| `modgui/*.png` | Panel artwork, knob film strip, Hold/Release button and Latch toggle sprites, screenshot, thumbnail |
| `tools/make_modgui.py` | Regenerates every PNG (`make modgui-images`, needs Pillow) |

The style matches JTBassSynth, with a navy enclosure and amber legends so the two are easy
to tell apart. If you change the layout in `tools/make_modgui.py`, update the matching
`left`/`top` values in the stylesheet.

## License

Copyright (c) 2026 Jonas Temple. Released under the GNU General Public License v3.0 or later; see `LICENSE`.
