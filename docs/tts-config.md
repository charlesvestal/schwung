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
  "debounce_ms": 300
}
```

### Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `engine` | string | `espeak` \| `flite` | `espeak` | TTS engine. eSpeak-NG is the default; Flite is bundled when the build includes its runtime. |
| `speed` | float | 0.5 – 6.0 | 1.0 | Speech rate (1.0 = normal). Higher = faster. |
| `pitch` | float | 80.0 – 180.0 | 110.0 | Voice pitch in Hz (lower = deeper). |
| `volume` | int | 0 – 100 | 70 | TTS output volume percentage. |
| `debounce_ms` | int | 0 – 1000 | 300 | How long to wait for further updates (e.g. while a knob is moving) before speaking. |

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

/* Select engine: "espeak" or "flite" */
void tts_set_engine(const char *name);

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

## Flite Voice Parameters

Under the hood, these settings map to Flite voice features:

- **speed** → `duration_stretch` (inverse relationship: higher = slower)
- **pitch** → `int_f0_target_mean` (fundamental frequency in Hz)

For more details on the TTS system architecture, see [tts-architecture.md](tts-architecture.md).
