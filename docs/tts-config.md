# TTS Voice Configuration

The Move Anything TTS system supports voice customization via speed, pitch, and volume controls.

## Configuration File

**Location on device:** `/data/UserData/move-anything/config/tts.json`

Create this file to customize the TTS voice. If the file doesn't exist, default settings are used.

### Example Configuration

```json
{
  "speed": 1.0,
  "pitch": 110.0,
  "volume": 70
}
```

### Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `speed` | float | 0.5 - 2.0 | 1.0 | Speech rate (0.5 = half speed, 1.0 = normal, 2.0 = double speed) |
| `pitch` | float | 80.0 - 180.0 | 110.0 | Voice pitch in Hz (lower = deeper, higher = more high-pitched) |
| `volume` | int | 0 - 100 | 70 | TTS output volume percentage |

### When Changes Take Effect

- Configuration file is loaded during TTS initialization (lazy init on first use)
- To apply new settings: restart Move or reload the Move Anything host

## Programmatic Control

The TTS engine also exposes C API functions for runtime control:

```c
/* Set speech speed (0.5 to 2.0) */
void tts_set_speed(float speed);

/* Set voice pitch in Hz (80 to 180) */
void tts_set_pitch(float pitch_hz);

/* Set output volume (0 to 100) */
void tts_set_volume(int volume);
```

Changes via these functions take effect immediately for the next spoken phrase.

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
scp docs/tts-config.example.json root@move.local:/data/UserData/move-anything/config/tts.json

# Or create directly on Move via SSH
ssh root@move.local
mkdir -p /data/UserData/move-anything/config
cat > /data/UserData/move-anything/config/tts.json << 'EOF'
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
