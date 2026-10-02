# TTS Voice Configuration

The Schwung TTS system supports voice customization via speed, pitch, and volume controls.

## Configuration File

**Location on device:** `/data/UserData/schwung/config/tts.json`

Create this file to customize the TTS voice. If the file doesn't exist, default settings are used.

### Example Configuration

```json
{
  "engine": "espeak",
  "speed": 1.0,
  "pitch": 110.0,
  "volume": 70,
  "evv_voice": 1,
  "evv_gender": 0,
  "evv_head": 50,
  "evv_pitch": 65,
  "evv_inflection": 30,
  "evv_rough": 0,
  "evv_breath": 0
}
```

Every writer goes through `tts_config_save()` (`src/host/tts_config.c`), which
loads the whole file, changes its own fields and writes the whole file back --
so a key one engine does not know (the `evv_*` voice, say) survives another
engine's save.

### Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `engine` | string | `espeak` \| `flite` \| `openevv` | `espeak` | TTS engine. eSpeak-NG is the default; Flite is bundled when the build includes its runtime; `openevv` is Eloquence (dlopened `lib/libeci.so.1`, falls back to eSpeak if it is missing). |
| `speed` | float | 0.5 – 6.0 | 1.0 | Speech rate (1.0 = normal). Higher = faster. |
| `pitch` | float | 80.0 – 180.0 | 110.0 | Voice pitch in Hz (lower = deeper). |
| `volume` | int | 0 – 100 | 70 | TTS output volume percentage. |
| `evv_voice` | int | 1 – 8 | 1 | Eloquence preset the six below were last loaded from (Adult Male 1, Adult Female 1, Child 1, Adult Male 2, Adult Male 3, Adult Female 2, Elderly Female 1, Elderly Male 1). |
| `evv_gender` | int | 0 – 1 | 0 | Eloquence: 0 male, 1 female. |
| `evv_head` | int | 0 – 100 | 50 | Eloquence head size (vocal tract length). |
| `evv_pitch` | int | 0 – 100 | 65 | Eloquence pitch baseline. Eloquence ignores `pitch` (Hz). |
| `evv_inflection` | int | 0 – 100 | 30 | Eloquence pitch fluctuation (how much the pitch moves). |
| `evv_rough` | int | 0 – 100 | 0 | Eloquence roughness. |
| `evv_breath` | int | 0 – 100 | 0 | Eloquence breathiness. |

Speak Delay is NOT in this file: it lives in `shadow_config.json` as
`tts_debounce_ms`.

### When Changes Take Effect

- The Shadow UI exposes these settings live under **Global Settings → Screen Reader** and writes them straight to `/data/UserData/schwung/config/tts.json` via the `tts_set_*` bindings — no restart needed.
- Editing the JSON file by hand is also supported; the engine re-reads it on next init. To force a re-read without rebooting, toggle the screen reader off and back on in Global Settings.

## Programmatic Control

The TTS engine exposes C API functions and matching JS bindings (in the shadow UI) for runtime control:

```c
/* Set speech speed (0.5 to 6.0) */
void tts_set_speed(float speed);

/* Set voice pitch in Hz (80 to 180) */
void tts_set_pitch(float pitch_hz);

/* Set output volume (0 to 100) */
void tts_set_volume(int volume);

/* Select engine: "espeak", "flite" or "openevv". openevv is dlopened; a
 * missing libeci.so.1 refuses the switch and the engine stays put. */
void tts_set_engine(const char *name);

/* The openevv (Eloquence) voice: preset + six ECI voice params. */
void tts_set_evv_voice(const tts_evv_voice_t *voice);

/* Tune debounce window in ms (0 to 1000) */
void tts_set_debounce(int ms);
```

The same functions are exposed to the Shadow UI as `tts_set_speed`,
`tts_set_pitch`, `tts_set_volume`, `tts_set_engine`, and
`tts_set_debounce`. Changes take effect immediately for the next
spoken phrase.

## Examples

### Faster, Higher Voice
```json
{
  "speed": 0.8,
  "pitch": 140.0,
  "volume": 70
}
```

### Slower, Deeper Voice
```json
{
  "speed": 1.3,
  "pitch": 90.0,
  "volume": 70
}
```

### Maximum Speed (for testing)
```json
{
  "speed": 2.0,
  "pitch": 110.0,
  "volume": 70
}
```

## Deployment

To deploy a custom config file to your Move:

```bash
# From your computer
scp docs/tts-config.example.json root@move.local:/data/UserData/schwung/config/tts.json

# Or create directly on Move via SSH
ssh root@move.local
mkdir -p /data/UserData/schwung/config
cat > /data/UserData/schwung/config/tts.json << 'EOF'
{
  "speed": 1.2,
  "pitch": 100.0,
  "volume": 80
}
EOF
```

## Implementation Details

- Config file is parsed using simple string matching (no JSON library dependency)
- Settings are validated and clamped to safe ranges
- Missing config file logs debug message but doesn't error
- Invalid values are ignored (defaults used instead)
- See `src/host/tts_engine_flite.c:tts_load_config()` for implementation

## Eloquence (openevv) Voice Parameters

openevv is a C rebuild of IBM ViaVoice / Eloquence behind IBM's ECI API
(`libs/openevv`, built to `lib/libeci.so.1`). The engine code is MIT; the
language data compiled into it is IBM's and is not licensed -- see
THIRD_PARTY_LICENSES.md.

- **speed** (shared row) → ECI `eciSpeed` = speed × 50, clamped 0–250, so
  1.0× is every preset's own rate.
- **volume** (shared row) → applied at read time, like the other engines; ECI's
  own volume is held at 100.
- **evv_voice** → `eciCopyVoice(preset, 0)`, then the six below are set on
  voice 0 before every utterance.
- **evv_gender / head / pitch / inflection / rough / breath** →
  `eciGender`, `eciHeadSize`, `eciPitchBaseline`, `eciPitchFluctuation`,
  `eciRoughness`, `eciBreathiness`.

Text is converted from UTF-8 to Windows-1252 (what `eciAddText` reads);
characters 1252 cannot say become spaces. Annotations stay off, so a backtick
in announced text is spoken, never interpreted.

**No ECI call runs on the SPI callback.** `eciNew` maps a 256 MB arena and
starts a thread, and a cancel waits ~27 ms for the current message to finish,
so a worker thread (SCHED_OTHER, cores 0–2) owns the instance and makes every
call; the RT side only publishes text and settings and reads a lock-free ring.

## Flite Voice Parameters

Under the hood, these settings map to Flite voice features:

- **speed** → `duration_stretch` (inverse relationship: higher = slower)
- **pitch** → `int_f0_target_mean` (fundamental frequency in Hz)

For more details on the TTS system architecture, see [tts-architecture.md](tts-architecture.md).
