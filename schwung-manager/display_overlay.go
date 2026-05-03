// Display overlay: render a one-shot status message ("Rebooting Move...")
// to the live display SHM so the device user sees what's happening
// during the install's reboot wait. Without this the screen looks
// frozen for ~30s and users power-cycle, which we just spent a week
// fixing the consequences of.
//
// Frame format (matches display_server.c + the in-browser SSE viewer):
// 128x64 1-bit OLED. 8 pages of 128 bytes; byte at (page*128 + col)
// holds 8 vertical pixels at column `col`, with bit 0 = topmost pixel
// in that page slice and bit 7 = bottommost.

package main

import (
	"net/http"
	"os"
)

const (
	dispW   = 128
	dispH   = 64
	dispSHM = "/dev/shm/schwung-display-live"
)

// 5x7 bitmap font for the characters we need. Each rune maps to 5 bytes
// (one per column). bit 0 = topmost pixel in the column. Hand-rolled so
// we don't drag in a font dep just for one screen.
var overlayFont = map[rune][5]byte{
	' ': {0x00, 0x00, 0x00, 0x00, 0x00},
	'.': {0x60, 0x60, 0x00, 0x00, 0x00},
	'M': {0x7F, 0x02, 0x0C, 0x02, 0x7F},
	'R': {0x7F, 0x09, 0x19, 0x29, 0x46},
	'b': {0x7F, 0x44, 0x44, 0x44, 0x38},
	'e': {0x38, 0x54, 0x54, 0x54, 0x18},
	'g': {0x0C, 0x52, 0x52, 0x52, 0x3E},
	'i': {0x00, 0x44, 0x7D, 0x40, 0x00},
	'n': {0x7C, 0x08, 0x04, 0x04, 0x78},
	'o': {0x38, 0x44, 0x44, 0x44, 0x38},
	't': {0x04, 0x3F, 0x44, 0x40, 0x20},
	'v': {0x1C, 0x20, 0x40, 0x20, 0x1C},
}

// renderText stamps 5x7 glyphs into a 128x64 1-bit frame at pixel (x, y).
// Returns the next x after the rendered text so callers can chain.
func renderText(frame []byte, x, y int, text string) int {
	for _, r := range text {
		glyph, ok := overlayFont[r]
		if !ok {
			x += 6
			continue
		}
		for col := 0; col < 5; col++ {
			px := x + col
			if px < 0 || px >= dispW {
				continue
			}
			bits := glyph[col]
			for row := 0; row < 7; row++ {
				if bits&(1<<row) == 0 {
					continue
				}
				py := y + row
				if py < 0 || py >= dispH {
					continue
				}
				page := py / 8
				bit := py % 8
				frame[page*128+px] |= 1 << bit
			}
		}
		x += 6 // 5-col glyph + 1-col gap
	}
	return x
}

// handleShowRebooting writes a "Rebooting Move..." frame to the shared
// memory display buffer. Used by install.sh just before triggering the
// reboot so users see explicit on-screen feedback during the ~30s wait
// instead of a blank/frozen-looking display.
func (app *App) handleShowRebooting(w http.ResponseWriter, r *http.Request) {
	var frame [1024]byte
	msg := "Rebooting Move..."
	// 17 glyphs * 6 px = 102 px wide. Center horizontally.
	startX := (dispW - len(msg)*6) / 2
	if startX < 0 {
		startX = 0
	}
	renderText(frame[:], startX, 28, msg)

	if err := os.WriteFile(dispSHM, frame[:], 0644); err != nil {
		app.logger.Error("show-rebooting: write SHM failed", "err", err, "path", dispSHM)
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	app.logger.Info("show-rebooting: frame written")
	w.WriteHeader(http.StatusOK)
}
