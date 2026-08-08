# MIDI / voice / looper event trace (G1.2)

A debugging lens for "notes pile up / get stuck" bugs (looper accumulation #138, poly
voice-steal drop #141): it records, in time order, every note reaching the engine, every
voice **alloc / steal / stop**, and every looper **record / playback / wrap** — so the exact
event that starts an extra voice is *named*, not guessed.

## Turning it on

Off by default (zero cost). Set the environment variable before launching:

```bash
VASYNTH_MIDI_TRACE=1 ./build/VASynth_artefacts/Release/Standalone/synth
# optional: choose the file (default ~/vasynth-miditrace.log)
VASYNTH_MIDI_TRACE=1 VASYNTH_MIDI_TRACE_FILE=/tmp/trace.log ./…/synth
```

In a DAW, set the variable in the environment that launches the host. The audio thread only
pushes small PODs into a lock-free ring; a message-thread timer drains them to the file every
~200 ms, with a final flush at teardown. When disabled, every emit is a single atomic-bool load
— RT-safe, no file, no thread.

## Reading it

One line per event: `block frame kind a b c d`

| kind    | meaning                    | a       | b       | c        | d          |
|---------|----------------------------|---------|---------|----------|------------|
| `BLK`   | a new audio block          | samples |         |          |            |
| `NLIV`  | live/played voice on       | note    | vel127  | part     | voiceIdx   |
| `NGEN`  | generator (arp/seq/looper) | note    | vel127  | part     | voiceIdx   |
| `NRTG`  | same note re-hit in place  | note    | vel127  | part     | voiceIdx   |
| `NOFF`  | voice released             | note    | part    | voiceIdx |            |
| `STEAL` | pool full → victim stolen  | note    | part    | stealIdx | victimNote |
| `CC`    | control change             | cc#     | value   | part     |            |
| `PANIC` | all-notes-off requested    |         |         |          |            |
| `ANOFF` | engine flushed every voice |         |         |          |            |
| `LREC`  | event recorded into a lane | part    | note    | vel127   | on?1:0     |
| `LEMIT` | armed event played back    | part    | note    | on?1:0   | t (sample) |
| `LWRAP` | lane wrapped → events armed| part    | count   |          |            |

The trailing `# end (dropped=N)` line reports events lost to ring overflow (0 in normal use).

## What to look for

- **Looper accumulation (#138):** on the third pass, a note whose `LEMIT … on=1` has **no
  matching `on=0`** within the cycle, or a lane emitting **more `LEMIT on=1` than it recorded
  `LREC on=1`** — that names an unpaired/duplicated trigger.
- **Poly steal drop (#141):** a `STEAL` whose `victimNote` is one of the held chord tones while
  the pool is full of leaked voices — cross-reference `NLIV`/`NGEN` counts vs. the F12 LIVE/GEN
  breakdown to see whether stuck voices filled the pool.
