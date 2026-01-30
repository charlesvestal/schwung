# Move Anything Logging Guide

## Quick Start

### Enable Logging

Create the flag file to enable logging:

```bash
ssh ableton@move.local "touch /data/UserData/move-anything/debug_log_on"
```

### Disable Logging

Remove the flag file:

```bash
ssh ableton@move.local "rm /data/UserData/move-anything/debug_log_on"
```

### View Logs

```bash
ssh ableton@move.local "tail -f /data/UserData/move-anything/debug.log"
```

## Log File Location

All logs go to: `/data/UserData/move-anything/debug.log`

## JavaScript Logging

### Option 1: Use console.log (Recommended)

In shadow UI modules, `console.log` automatically routes to the unified log:

```javascript
console.log('Something happened');  // Goes to debug.log
console.warn('Warning!');           // Goes to debug.log with WARN prefix
console.error('Error!');            // Goes to debug.log with ERROR prefix
```

### Option 2: Use Logger Module Directly

```javascript
import { log, debug, info, warn, error } from '/data/UserData/move-anything/shared/logger.mjs';

log('mymodule', 'Custom message');
debug('mymodule', 'Debug info');
warn('mymodule', 'Warning message');
error('mymodule', 'Error message');
```

### Option 3: Install Console Override in Your Module

```javascript
import { installConsoleOverride } from '/data/UserData/move-anything/shared/logger.mjs';

// Call once at module startup
installConsoleOverride('mymodule');

// Now console.log includes your module name
console.log('Hello');  // Logs as: [mymodule] Hello
```

## C/C++ Logging

### In DSP Plugins

Use the host log callback (this is the existing pattern and still works):

```c
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[myplugin] %s", msg);
        g_host->log(buf);
    }
}
```

### In Host/Shim Code

Include and use the unified log:

```c
#include "host/unified_log.h"

// Using macros
LOG_DEBUG("mycomponent", "Debug message: %d", value);
LOG_INFO("mycomponent", "Info message");
LOG_WARN("mycomponent", "Warning: %s", reason);
LOG_ERROR("mycomponent", "Error: %s", error);

// Using function directly
unified_log("mycomponent", LOG_LEVEL_DEBUG, "Message: %s", data);
```

### Log Levels

```c
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3
```

## Log Format

```
HH:MM:SS.mmm [LEVEL] [source] message
```

Example:
```
14:23:45.123 [DEBUG] [shadow] Slot 1 selected
14:23:45.456 [DEBUG] [chain] Loading synth: sf2
14:23:45.789 [WARN ] [shim] MIDI buffer overflow
```

## Log Levels

- `ERROR` - Critical errors that need attention
- `WARN` - Warnings that might indicate problems
- `INFO` - General information about operations
- `DEBUG` - Detailed debugging information

## Best Practices

1. **Use descriptive source names**: `shadow`, `chain`, `sf2`, `shim`, etc.
2. **Don't log in hot paths**: Avoid logging in render_block() or tight loops
3. **Log errors always**: Use LOG_ERROR for conditions that need investigation
4. **Disable in production**: Remove the flag file when not debugging
5. **Clear old logs**: The log file grows unbounded; periodically clear it

## Architecture

The unified logging system consists of:

- **C Core** (`src/host/unified_log.h/c`): Thread-safe logging with mutex, timestamps, log levels
- **JS Bindings** (`src/shadow/shadow_ui.c`): Exposes `unified_log()` and `unified_log_enabled()` to JavaScript
- **JS Module** (`src/shared/logger.mjs`): High-level API with console.log override

### How It Works

1. Logging is **disabled by default** for performance
2. Create `/data/UserData/move-anything/debug_log_on` to enable
3. The flag file is checked periodically (not every call) to minimize overhead
4. All log entries include millisecond timestamps and source tags
5. The log file is opened in append mode; entries are flushed immediately

## Troubleshooting

### Logs not appearing

1. Check flag file exists: `ssh ableton@move.local "ls /data/UserData/move-anything/debug_log_on"`
2. Check permissions: `ssh ableton@move.local "ls -la /data/UserData/move-anything/"`
3. Restart shadow UI after enabling logging (exit and re-enter via Shift+Vol+Track)

### Log file too large

Clear the log:
```bash
ssh ableton@move.local "> /data/UserData/move-anything/debug.log"
```

### Checking if logging is enabled from code

**JavaScript:**
```javascript
import { isLoggingEnabled } from '/data/UserData/move-anything/shared/logger.mjs';
if (isLoggingEnabled()) {
    // logging is on
}
```

**C:**
```c
#include "host/unified_log.h"
if (unified_log_enabled()) {
    // logging is on
}
```
