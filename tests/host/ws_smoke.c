// SPDX-License-Identifier: MIT
//
// Phase 5a smoke test: start ws_server on :7682, log every inbound frame,
// echo a binary frame back to the client after each one. Run for 10 seconds
// then exit.
//
//   $ ./build/mac/ws_smoke &
//   $ python3 -c "
//   import asyncio, websockets
//   async def main():
//       async with websockets.connect('ws://localhost:7682/') as ws:
//           await ws.send(bytes([0x9F, 73, 100]))  # note-on ch16 note73 vel100
//           pong = await ws.recv()
//           print('got', pong)
//   asyncio.run(main())
//   "

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "host/ws_server.h"

static ws_server_t *g_srv = NULL;

static void on_frame(void *ctx, const uint8_t *data, size_t len, int is_binary) {
    (void)ctx;
    fprintf(stderr, "ws_smoke: rx %s len=%zu first=", is_binary ? "BIN" : "TEXT", len);
    for (size_t i = 0; i < len && i < 8; i++) fprintf(stderr, "%02x ", data[i]);
    fprintf(stderr, "\n");
    // Echo back
    if (g_srv) ws_server_broadcast(g_srv, data, len);
}

int main(void) {
    g_srv = ws_server_start(7682, on_frame, NULL);
    if (!g_srv) return 1;
    fprintf(stderr, "ws_smoke: listening for 10s\n");
    sleep(10);
    ws_server_stop(g_srv);
    return 0;
}
