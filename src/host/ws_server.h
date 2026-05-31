// SPDX-License-Identifier: MIT
//
// Minimal RFC 6455 WebSocket server for schwung-host (macOS sim).
//
// Listens on a TCP port in a background thread, handles the HTTP/1.1 upgrade
// handshake, then accepts binary or text frames from connected clients. No
// fragmentation, no compression, no TLS. Server-to-client frames are sent
// unmasked (per RFC); client-to-server frames must be masked and we unmask
// in place during parsing.
//
// Designed for a single use: the schwung-sim browser UI on :7682. Total
// concurrent client cap is small (a handful).

#ifndef SCHWUNG_HOST_WS_SERVER_H
#define SCHWUNG_HOST_WS_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_server ws_server_t;

// Callback signature for inbound frames. `data`/`len` is the unmasked payload.
// `is_binary` is 1 for binary frames (opcode 2) and 0 for text frames (opcode 1).
// Called from the WS listener thread — keep work brief, don't block.
typedef void (*ws_on_frame_fn)(void *ctx, const uint8_t *data, size_t len,
                               int is_binary);

// Start the server on `port`. Spawns one accept/poll thread. Returns NULL on
// failure (port bind error, OOM, etc.) with an error logged to stderr.
ws_server_t *ws_server_start(int port, ws_on_frame_fn on_frame, void *ctx);

// Stop the server: closes the listening socket, drops all clients, joins the
// thread. Safe to call multiple times. Frees the handle.
void ws_server_stop(ws_server_t *s);

// Broadcast a binary frame to all connected clients. Drops the frame for
// clients whose send buffer is full (no blocking). `data`/`len` payload only.
// Safe to call from any thread.
void ws_server_broadcast(ws_server_t *s, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SCHWUNG_HOST_WS_SERVER_H
