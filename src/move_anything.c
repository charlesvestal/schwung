#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <dlfcn.h>

#include "quickjs.h"
#include "quickjs-libc.h"
#include "host/plugin_api_v1.h"

#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"

#include "host/module_manager.h"
#include "host/settings.h"

int global_fd = -1;
int global_exit_flag = 0;

/* Host-level input state for system shortcuts */
int host_shift_held = 0;
int host_transpose = 0;  /* Semitone transpose for internal MIDI (-48 to +48) */

/* Move MIDI CC constants for system shortcuts */
#define CC_SHIFT 49
#define CC_JOG_CLICK 3
#define CC_BACK 51
#define CC_MASTER_KNOB 79
#define CC_UP 55
#define CC_DOWN 54

/* Module manager instance */
module_manager_t g_module_manager;
int g_module_manager_initialized = 0;

/* Host settings instance */
host_settings_t g_settings;

/* MIDI clock state */
#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
float g_clock_accumulator = 0.0f;
int g_clock_started = 0;

/* Flag to refresh JS function references after module UI load */
int g_js_functions_need_refresh = 0;
int g_reload_menu_ui = 0;
char g_menu_script_path[256] = "";
int g_silence_blocks = 0;

/* Default modules directory */
#define DEFAULT_MODULES_DIR "/data/UserData/move-anything/modules"

/* Base directory for path validation */
#define BASE_DIR "/data/UserData/move-anything"

/* Bundled curl binary path */
#define CURL_PATH "/data/UserData/move-anything/bin/curl"

typedef struct FontChar {
  unsigned char* data;
  int width;
  int height;
} FontChar;

typedef struct Font {
  int charSpacing;
  FontChar charData[128];
  /* TTF font data */
  int is_ttf;
  stbtt_fontinfo ttf_info;
  unsigned char* ttf_buffer;
  float ttf_scale;
  int ttf_ascent;
  int ttf_height;
} Font;

Font* font = NULL;

unsigned char screen_buffer[128*64];
int screen_dirty = 0;
int frame = 0;

/* Display refresh rate limiting */
int display_pending = 0;           /* Display has changes waiting to be flushed */
int display_countdown = 0;         /* Countdown to next allowed refresh */
int display_refresh_interval = 30; /* Ticks between refreshes (~11Hz at 344Hz loop) */
int display_force_flush = 0;       /* Force immediate flush */

/* Forward declarations */
void push_screen(int sync);

struct SPI_Memory
{
    unsigned char outgoing_midi[256];
    unsigned char outgoing_random[512];
    unsigned char outgoing_unknown[1280];
    unsigned char incoming_midi[256];
    unsigned char incoming_random[512];
    unsigned char incoming_unknown[1280];
};

unsigned char *mapped_memory;

int outgoing_midi_counter = 0;

#define LED_MAX_UPDATES_PER_TICK 16
#define LED_QUEUE_SAFE_BYTES 76
static int pending_note_color[128];
static uint8_t pending_note_status[128];
static uint8_t pending_note_cin[128];
static int pending_cc_color[128];
static uint8_t pending_cc_status[128];
static uint8_t pending_cc_cin[128];

struct USB_MIDI_Packet
{
    unsigned char cable;
    unsigned char code_index_number : 4;
    unsigned char midi_0;
    unsigned char midi_1;
    unsigned char midi_2;
};



void set_int16(int byte, int16_t value) {
  if(byte >= 0 && byte < 4095) {
    mapped_memory[byte] = value & 0xFF;
    mapped_memory[byte+1] = (value >> 8) & 0xFF;
  }
}

int16_t get_int16(int byte) {
  if(byte >= 0 && byte < 4095) {
    int16_t ret = mapped_memory[byte];
    ret |= mapped_memory[byte+1] << 8;
    return ret;
  }
  return 0;
}

static JSValue js_set_int16(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc != 2) {
    JS_ThrowTypeError(ctx, "set_int16() expects 2, got %d", argc);
    return JS_EXCEPTION;
  }

  int byte,value;
  if(JS_ToInt32(ctx, &byte, argv[0])) {
    JS_ThrowTypeError(ctx, "set_int16() invalid arg for `byte`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &value, argv[1])) {
    JS_ThrowTypeError(ctx, "set_int16() invalid arg for `value`");
    return JS_EXCEPTION;
  }
  set_int16(byte, (int16_t)value);
  return JS_UNDEFINED;
}

static JSValue js_get_int16(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc != 1) {
    JS_ThrowTypeError(ctx, "get_int16() expects 1, got %d", argc);
    return JS_EXCEPTION;
  }

  int byte;
  if(JS_ToInt32(ctx, &byte, argv[0])) {
    JS_ThrowTypeError(ctx, "get_int16() invalid arg for `byte`");
    return JS_EXCEPTION;
  }
  int16_t val = get_int16(byte);
  JSValue js_val = JS_NewInt32(ctx, val);
  return js_val;
}



// void set_audio_out_L(int index, int16_t value) {
//   if (index >= 512/4) {
//     return;
//   }

//   int out256+index*4+0
// }

// void set_audio_out_R(int index, int16_t value) {

// }

// void get_audio_in_L() {

// }

// void get_audio_in_R() {

// }

void dirty_screen() {
  /* Mark that display needs to be pushed */
  display_pending = 1;
}

void clear_screen() {
  memset(screen_buffer, 0, 128*64);
  dirty_screen();
}

void set_pixel(int x, int y, int value) {
  if(x >= 0 && x < 128 && y >= 0 && y < 64) {
    screen_buffer[y*128+x] = value != 0 ? 1 : 0;
    dirty_screen();
  }
}

int get_pixel(int x, int y) {
  return screen_buffer[y*128+x] > 0 ? 1 : 0;
}

void draw_rect(int x, int y, int w, int h, int value) {
  if(w == 0 || h == 0) {
    return;
  }

  for(int yi = y; yi < y+h; yi++) {
    set_pixel(x, yi, value);
    set_pixel(x+w-1, yi, value);
  }

  for(int xi = x; xi < x+w; xi++) {
    set_pixel(xi, y, value);
    set_pixel(xi, y+h-1, value);
  }
}

void fill_rect(int x, int y, int w, int h, int value) {
  if(w == 0 || h == 0) {
    return;
  }

  for(int yi = y; yi < y+h; yi++) {
    for(int xi = x; xi < x+w; xi++) {
      set_pixel(xi, yi, value);
    }
  }
}

Font* load_font(char* filename, int charSpacing) {
  int width, height, comp;

  char charListFilename[100];
  sprintf(charListFilename, "%s.dat", filename);

  FILE* charListFP = fopen(charListFilename, "r");
  if(charListFP == NULL) {
    printf("ERROR loading font charList from: %s\n", charListFilename);
    return NULL;
  }

  char charList[256];
  fgets(charList, 256, charListFP);
  fclose(charListFP);

  int numChars = strlen(charList);

  uint32_t* data = (uint32_t*)stbi_load(filename, &width, &height, &comp, 4);
  if(data == NULL) {
    printf("ERROR loading font: %s\n", filename);
    return NULL;
  }

  Font* font = malloc(sizeof(Font));

  font->charSpacing = charSpacing;

  comp = 4;
  int x = 0;
  int y = 0;

  uint32_t borderColor = data[0];
  uint32_t emptyColor = data[(height-1) * width];

  printf("FONT DEBUG: file=%s, size=%dx%d, numChars=%d\n", filename, width, height, numChars);
  printf("FONT DEBUG: borderColor=0x%08x, emptyColor=0x%08x\n", borderColor, emptyColor);

  if (borderColor == emptyColor) {
    printf("FONT ERROR: borderColor == emptyColor, font will not load correctly!\n");
  }

  x = 0;

  for(int i = 0; i < numChars; i++) {
    FontChar fc;
    fc.width = 0;
    fc.height = 0;

    while(data[x] == borderColor) {
      x++;
    }

    int bx = x;
    int by = y;
    for(int by = 0; by < height; by++) {
        uint32_t color = data[by * width + x];
        if(color == borderColor) {
          fc.height = by;
          break;
        }
    }
    for(int bx = x; bx < width; bx++) {
        uint32_t color = data[bx];
        if(color == borderColor) {
          fc.width = bx - x;
          break;
        }
    }

    if(fc.width == 0 || fc.height == 0) {
      printf("FONT ERROR [%d/%d] char '%c' (0x%02x) has zero dimension: %d x %d at x=%d\n",
             i, numChars, charList[i], (unsigned char)charList[i], fc.width, fc.height, x);
      printf("FONT DEBUG: x position may have exceeded width (%d)\n", width);
      break;
    }

    if (i < 5 || i == numChars - 1) {
      printf("FONT DEBUG [%d] char '%c': %dx%d at x=%d\n", i, charList[i], fc.width, fc.height, x);
    }

    fc.data = malloc(fc.width * fc.height);

    int setPixels = 0;

    for(int yi = 0; yi < fc.height; yi++) {
      for(int xi = 0; xi < fc.width; xi++) {
        uint32_t color = data[(y + yi) * width + (x + xi)];
        int set = (color != borderColor && color != emptyColor);
        if(set) {
          setPixels++;
        }
        fc.data[yi * fc.width + xi] = set;
      }
    }

    font->charData[(int)charList[i]] = fc;

    x += fc.width+1;
    if(x >= width) {
      break;
    }
  }

  printf("Loaded bitmap font: %s (%d chars)\n", filename, numChars);
  return font;
}

Font* load_ttf_font(const char* filename, int pixel_height) {
  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    printf("ERROR loading TTF font: %s\n", filename);
    return NULL;
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  unsigned char* buffer = malloc(size);
  if (!buffer) {
    fclose(fp);
    return NULL;
  }
  fread(buffer, 1, size, fp);
  fclose(fp);

  Font* font = malloc(sizeof(Font));
  memset(font, 0, sizeof(Font));
  font->is_ttf = 1;
  font->ttf_buffer = buffer;
  font->charSpacing = 1;

  if (!stbtt_InitFont(&font->ttf_info, buffer, 0)) {
    printf("ERROR: stbtt_InitFont failed for %s\n", filename);
    free(buffer);
    free(font);
    return NULL;
  }

  font->ttf_scale = stbtt_ScaleForPixelHeight(&font->ttf_info, pixel_height);
  font->ttf_height = pixel_height;

  int ascent, descent, lineGap;
  stbtt_GetFontVMetrics(&font->ttf_info, &ascent, &descent, &lineGap);
  font->ttf_ascent = (int)(ascent * font->ttf_scale);

  printf("Loaded TTF font: %s (height=%d, scale=%.3f)\n", filename, pixel_height, font->ttf_scale);
  return font;
}

int glyph_ttf(Font* font, char c, int sx, int sy, int color) {
  int advance, lsb;
  stbtt_GetCodepointHMetrics(&font->ttf_info, c, &advance, &lsb);

  int x0, y0, x1, y1;
  stbtt_GetCodepointBitmapBox(&font->ttf_info, c, font->ttf_scale, font->ttf_scale, &x0, &y0, &x1, &y1);

  int w = x1 - x0;
  int h = y1 - y0;

  if (w <= 0 || h <= 0) {
    return sx + (int)(advance * font->ttf_scale);
  }

  unsigned char* bitmap = malloc(w * h);
  stbtt_MakeCodepointBitmap(&font->ttf_info, bitmap, w, h, w, font->ttf_scale, font->ttf_scale, c);

  /* Render with threshold (no anti-aliasing for 1-bit display)
   * Lower threshold (64) captures more of thin font strokes */
  int draw_x = sx + x0;
  int draw_y = sy + font->ttf_ascent + y0;

  for (int yi = 0; yi < h; yi++) {
    for (int xi = 0; xi < w; xi++) {
      if (bitmap[yi * w + xi] > 64) {
        set_pixel(draw_x + xi, draw_y + yi, color);
      }
    }
  }

  free(bitmap);
  return sx + (int)(advance * font->ttf_scale);
}

int glyph(Font* font, char c, int sx, int sy, int color) {
  FontChar fc = font->charData[(int)c];
  if(fc.data == NULL) {
    return sx + font->charSpacing;
  }

  for(int y = 0; y < fc.height; y++) {
    for(int x = 0; x < fc.width; x++) {
      if(fc.data[y * fc.width + x]) {
        set_pixel(sx + x, sy + y, color);
      }
    }
  }
  return sx + fc.width;
}

void print(int sx, int sy, const char* string, int color) {
  int x = sx;
  int y = sy;

  if(font == NULL) {
    /* Try TTF font first (unifont from Move's fonts directory) */
    font = load_ttf_font("/opt/move/Fonts/unifont_jp-14.0.01.ttf", 12);
    if(font == NULL) {
      /* Fall back to bitmap font */
      font = load_font("font.png", 1);
    }
  }

  if(font == NULL) {
    return;
  }

  for(int i = 0; i < strlen(string); i++) {
    if (font->is_ttf) {
      x = glyph_ttf(font, string[i], x, y, color);
    } else {
      x = glyph(font, string[i], x, y, color);
      x += font->charSpacing;
    }
  }
}

/* Process host-level MIDI for system shortcuts and input transforms
 * Takes pointer to MIDI bytes (status, data1, data2) for in-place modification
 * Returns 1 if message was consumed by host, 0 if should pass to module */
int process_host_midi(unsigned char *midi, int apply_transforms) {
  unsigned char status = midi[0];
  unsigned char data1 = midi[1];
  unsigned char data2 = midi[2];
  unsigned char msg_type = status & 0xF0;

  /* Apply MIDI transforms unless module wants raw MIDI */
  if (apply_transforms) {
    /* Velocity curve for Note On */
    if (msg_type == 0x90 && data2 > 0) {
      midi[2] = settings_apply_velocity(&g_settings, data2);
    }

    /* Aftertouch filter */
    if (msg_type == 0xA0 || msg_type == 0xD0) {
      uint8_t at_value = (msg_type == 0xA0) ? data2 : data1;
      if (!settings_apply_aftertouch(&g_settings, &at_value)) {
        return 1;  /* Aftertouch disabled, drop message */
      }
      /* Update the modified value */
      if (msg_type == 0xA0) {
        midi[2] = at_value;
      } else {
        midi[1] = at_value;
      }
    }

    /* Apply pad layout and transpose for Note On/Off on pad notes (68-99) */
    if ((msg_type == 0x90 || msg_type == 0x80) && data1 >= 68 && data1 <= 99) {
      int note = data1;

      /* Apply pad layout remapping */
      if (g_settings.pad_layout == PAD_LAYOUT_FOURTH) {
        /* Fourth layout: each row is a fourth (5 semitones) up */
        int row = (note - 68) / 8;
        int col = (note - 68) % 8;
        note = 60 + (row * 5) + col;
      }

      /* Apply transpose */
      note += host_transpose;

      /* Clamp to valid MIDI note range */
      if (note < 0) note = 0;
      if (note > 127) note = 127;

      midi[1] = (unsigned char)note;
    }
  }

  /* Handle CC messages for host shortcuts */
  if (msg_type != 0xB0) {
    return 0;  /* Not a CC, pass through (after transforms) */
  }

  unsigned char cc = data1;
  unsigned char value = data2;

  /* Track Shift key state */
  if (cc == CC_SHIFT) {
    host_shift_held = (value == 127);
    return 0;  /* Pass through so modules can also track it */
  }

  /* Shift + Jog Click = Exit Move Anything */
  if (cc == CC_JOG_CLICK && value == 127 && host_shift_held) {
    printf("Host: Shift+Wheel detected - exiting\n");
    global_exit_flag = 1;
    return 1;  /* Consumed, don't pass to module */
  }

  /* Back button: return to menu unless module owns UI */
  if (cc == CC_BACK && value == 127) {
    if (g_module_manager_initialized && mm_is_module_loaded(&g_module_manager) &&
        !mm_module_wants_raw_ui(&g_module_manager)) {
      g_reload_menu_ui = 1;
      return 1;
    }
  }

  /* Master volume knob - relative encoder */
  /* Only handle if module doesn't claim the knob */
  if (cc == CC_MASTER_KNOB && g_module_manager_initialized &&
      !mm_module_claims_master_knob(&g_module_manager)) {
    int current_vol = mm_get_host_volume(&g_module_manager);
    int delta;

    /* Relative encoder: 1-63 = clockwise (increment), 65-127 = counter-clockwise (decrement) */
    if (value >= 1 && value <= 63) {
      /* Clockwise - apply acceleration curve */
      delta = (value > 10) ? 5 : (value > 3) ? 2 : 1;
    } else if (value >= 65 && value <= 127) {
      /* Counter-clockwise - apply acceleration curve */
      int speed = 128 - value;  /* Convert to positive: 127->1, 65->63 */
      delta = (speed > 10) ? -5 : (speed > 3) ? -2 : -1;
    } else {
      delta = 0;
    }

    if (delta != 0) {
      int new_vol = current_vol + delta;
      mm_set_host_volume(&g_module_manager, new_vol);
      printf("Host: Volume %d -> %d\n", current_vol, mm_get_host_volume(&g_module_manager));
    }
    return 1;  /* Consumed by host */
  }

  /* Shift + Up/Down = Semitone transpose */
  if (host_shift_held && value == 127) {
    if (cc == CC_UP) {
      if (host_transpose < 48) {
        host_transpose++;
        printf("Host: Transpose +1 -> %d\n", host_transpose);
      }
      return 1;  /* Consumed */
    }
    if (cc == CC_DOWN) {
      if (host_transpose > -48) {
        host_transpose--;
        printf("Host: Transpose -1 -> %d\n", host_transpose);
      }
      return 1;  /* Consumed */
    }
  }

  return 0;  /* Pass through */
}

int queueMidiSend(int cable, unsigned char *buffer, int length)
{
    if (outgoing_midi_counter + length > 256)
    {
        printf("Outgoing MIDI send queue is full. Discarding messages.");
        return 0;
    }

    // printf("queueMidi: queueing %d bytes to outgoing MIDI ,counter:%d\n", length, outgoing_midi_counter);
    memcpy(((struct SPI_Memory *)mapped_memory)->outgoing_midi + outgoing_midi_counter, buffer, length);

    outgoing_midi_counter += length;

    if (outgoing_midi_counter >= 80)
    {
        int ioctl_result = ioctl(global_fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
        outgoing_midi_counter = 0;
    }
    return length;
}

int queueExternalMidiSend(unsigned char *buffer, int length)
{
    return queueMidiSend(2, buffer, length);
}

int queueInternalMidiSend(unsigned char *buffer, int length)
{
    return queueMidiSend(0, buffer, length);
}

static void reset_pending_leds(void) {
    for (int i = 0; i < 128; i++) {
        pending_note_color[i] = -1;
        pending_note_status[i] = 0x90;
        pending_note_cin[i] = 0x09;
        pending_cc_color[i] = -1;
        pending_cc_status[i] = 0xB0;
        pending_cc_cin[i] = 0x0B;
    }
}

static void queue_pending_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        pending_note_color[data1] = data2;
        pending_note_status[data1] = status;
        pending_note_cin[data1] = cin;
    } else if (type == 0xB0) {
        pending_cc_color[data1] = data2;
        pending_cc_status[data1] = status;
        pending_cc_cin[data1] = cin;
    }
}

static void flush_pending_leds(void) {
    int available = (LED_QUEUE_SAFE_BYTES - outgoing_midi_counter) / 4;
    int budget = LED_MAX_UPDATES_PER_TICK;
    if (available <= 0 || budget <= 0) {
        return;
    }
    if (budget > available) {
        budget = available;
    }

    int sent = 0;
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (pending_note_color[i] >= 0) {
            unsigned char msg[4] = {
                pending_note_cin[i],
                pending_note_status[i],
                (unsigned char)i,
                (unsigned char)pending_note_color[i]
            };
            pending_note_color[i] = -1;
            queueMidiSend(0, msg, 4);
            sent++;
        }
    }
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (pending_cc_color[i] >= 0) {
            unsigned char msg[4] = {
                pending_cc_cin[i],
                pending_cc_status[i],
                (unsigned char)i,
                (unsigned char)pending_cc_color[i]
            };
            pending_cc_color[i] = -1;
            queueMidiSend(0, msg, 4);
            sent++;
        }
    }
}

void onExternalMidiMessage(unsigned char midi_message[4])
{
    // js_on_external_midi_message()
}

void onInternalMidiMessage(unsigned char midi_message[4])
{
    // js_on_internal_midi_message()
}

void onMidiMessage(unsigned char midi_message[4])
{

    int cable;
    int code_index_number;

    if (cable == 0)
    {
        onInternalMidiMessage(midi_message);
    }

    if (cable == 2)
    {
        onExternalMidiMessage(midi_message);
    }
}

void clearPads(unsigned char *mapped_memory, int fd)
{

    // clear pads
    int j = 0;
    for (int i = 0, pad = 0; pad < 32; pad++)
    {
        j = i * 4;

        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 0] = 0 | 0x9;
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 1] = 0x90;
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 2] = (68 + pad);
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 3] = 0;

        if (i > 9)
        {
            int ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
            // memset(((struct SPI_Memory *)mapped_memory)->outgoing_midi, 0, 256);
            i = 0;
        }

        i++;
    }

    int ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
}

void clearSequencerButtons(unsigned char *mapped_memory, int fd)
{

    // clear pads
    int j = 0;
    for (int i = 0, pad = 0; pad < 16; pad++)
    {
        j = i * 4;

        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 0] = 0 | 0x9;
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 1] = 0x90;
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 2] = (16 + pad);
        ((struct SPI_Memory *)mapped_memory)->outgoing_midi[j + 3] = 0;

        if (i > 9)
        {
            int ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
            // memset(((struct SPI_Memory *)mapped_memory)->outgoing_midi, 0, 256);
            i = 0;
        }

        // printf("clearing button %d\n", pad);

        i++;
    }

    int ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
}

void kickM8(unsigned char *mapped_memory, int fd)
{
    unsigned char out_cable = 2;

    unsigned char LPPInitSysex[24] = {
        (unsigned char)(out_cable << 4 | 0x4), 0xF0, 126, 0,
        (unsigned char)(out_cable << 4 | 0x4), 6, 2, 0,
        (unsigned char)(out_cable << 4 | 0x4), 32, 41, 0x00,
        (unsigned char)(out_cable << 4 | 0x4), 0x00, 0x00, 0x00,
        (unsigned char)(out_cable << 4 | 0x4), 0x00, 0x00, 0x00,
        (unsigned char)(out_cable << 4 | 0x6), 0x00, 0xF7, 0x0};

    memcpy(((struct SPI_Memory *)mapped_memory)->outgoing_midi, LPPInitSysex, 23);
    int ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
}

#ifndef FALSE
enum
{
    FALSE = 0,
    TRUE = 1,
};
#endif

/* also used to initialize the worker context */
static JSContext *JS_NewCustomContext(JSRuntime *rt)
{
    JSContext *ctx;
    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    /* system modules */
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    return ctx;
}

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags)
{
    JSValue val;
    int ret;

    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE)
    {
        /* for the modules, we compile then run to be able to set
           import.meta */
        val = JS_Eval(ctx, buf, buf_len, filename,
                      eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val))
        {
            js_module_set_import_meta(ctx, val, TRUE, TRUE);
            val = JS_EvalFunction(ctx, val);
        }
        val = js_std_await(ctx, val);
    }
    else
    {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val))
    {
        js_std_dump_error(ctx);
        ret = -1;
    }
    else
    {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename, int module)
{
    uint8_t *buf;
    int ret, eval_flags = JS_EVAL_FLAG_STRICT; // Always eval in strict mode.
    size_t buf_len;

    printf("Loading control surface script: %s\n", filename);
    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf)
    {
        perror(filename);
        exit(1);
    }

    // if (module < 0) {
    //     module = (has_suffix(filename, ".mjs") ||
    //               JS_DetectModule((const char *)buf, buf_len));
    // }
    if (module)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;
    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);
    return ret;
}

static JSValue js_set_pixel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc < 2 || argc > 3) {
    JS_ThrowTypeError(ctx, "set_pixel() expects 2 or 3 arguments, got %d", argc);
    return JS_EXCEPTION;
  }

  int x,y,color;
  if(JS_ToInt32(ctx, &x, argv[0])) {
    JS_ThrowTypeError(ctx, "set_pixel() invalid arg for `x`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &y, argv[1])) {
    JS_ThrowTypeError(ctx, "set_pixel() invalid arg for `y`");
    return JS_EXCEPTION;
  }
  if(argc == 3) {
    if(JS_ToInt32(ctx, &color, argv[2])) {
      JS_ThrowTypeError(ctx, "set_pixel() invalid arg for `color`");
      return JS_EXCEPTION;
    }
  } else {
    color = 1;
  }
  set_pixel(x,y,color);
  return JS_UNDEFINED;
}

static JSValue js_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc < 4 || argc > 5) {
    JS_ThrowTypeError(ctx, "draw_rect() expects 4 or 5 arguments, got %d", argc);
    return JS_EXCEPTION;
  }

  int x,y,w,h,color;
  if(JS_ToInt32(ctx, &x, argv[0])) {
    JS_ThrowTypeError(ctx, "draw_rect: invalid arg for `x`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &y, argv[1])) {
    JS_ThrowTypeError(ctx, "draw_rect: invalid arg for `y`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &w, argv[2])) {
    JS_ThrowTypeError(ctx, "draw_rect: invalid arg for `w`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &h, argv[3])) {
    JS_ThrowTypeError(ctx, "draw_rect: invalid arg for `h`");
    return JS_EXCEPTION;
  }
  if(argc == 5) {
    if(JS_ToInt32(ctx, &color, argv[4])) {
      JS_ThrowTypeError(ctx, "draw_rect: invalid arg for `color`");
      return JS_EXCEPTION;
    }
  } else {
    color = 1;
  }
  draw_rect(x,y,w,h,color);
  return JS_UNDEFINED;
}

static JSValue js_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc < 4 || argc > 5) {
    JS_ThrowTypeError(ctx, "fill_rect() expects 4 or 5 arguments, got %d", argc);
    return JS_EXCEPTION;
  }

  int x,y,w,h,color;
  if(JS_ToInt32(ctx, &x, argv[0])) {
    JS_ThrowTypeError(ctx, "fill_rect: invalid arg for `x`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &y, argv[1])) {
    JS_ThrowTypeError(ctx, "fill_rect: invalid arg for `y`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &w, argv[2])) {
    JS_ThrowTypeError(ctx, "fill_rect: invalid arg for `w`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &h, argv[3])) {
    JS_ThrowTypeError(ctx, "fill_rect: invalid arg for `h`");
    return JS_EXCEPTION;
  }
  if(argc == 5) {
    if(JS_ToInt32(ctx, &color, argv[4])) {
      JS_ThrowTypeError(ctx, "fill_rect: invalid arg for `color`");
      return JS_EXCEPTION;
    }
  } else {
    color = 1;
  }
  fill_rect(x,y,w,h,color);
  return JS_UNDEFINED;
}

static JSValue js_clear_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc > 0) {
    JS_ThrowTypeError(ctx, "clear_screen() expects 0 arguments, got %d", argc);
    return JS_EXCEPTION;
  }
  clear_screen();
  return JS_UNDEFINED;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  if(argc < 3) {
    JS_ThrowTypeError(ctx, "print(x,y,string,color) expects 3,4 arguments, got %d", argc);
    return JS_EXCEPTION;
  }

  int x,y,color;
  if(JS_ToInt32(ctx, &x, argv[0])) {
    JS_ThrowTypeError(ctx, "print: invalid arg for `x`");
    return JS_EXCEPTION;
  }
  if(JS_ToInt32(ctx, &y, argv[1])) {
    JS_ThrowTypeError(ctx, "print: invalid arg for `y`");
    return JS_EXCEPTION;
  }

  JSValue string_val = JS_ToString(ctx, argv[2]);
  const char* string = JS_ToCString(ctx, string_val);

  color = 1;

  if(JS_ToInt32(ctx, &color, argv[3])) {
    JS_ThrowTypeError(ctx, "print: invalid arg for `color`");
    return JS_EXCEPTION;
  }

  print(x, y, string, color);

  JS_FreeValue(ctx, string_val);
  JS_FreeCString(ctx, string);
  return JS_UNDEFINED;
}

// static JSValue js_sum_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {

#define js_move_midi_external_send_buffer_size 4096
unsigned char js_move_midi_send_buffer[js_move_midi_external_send_buffer_size];
static JSValue js_move_midi_send(int cable, JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    // printf("js_move_midi_internal_send %d\n", argc);

    // internal_move_midi_external_send_start();

    int send_buffer_index = 0;

    if (argc != 1)
    {
        JS_ThrowTypeError(ctx, "move_midi_external_send() expects exactly 1 argument, but got %d", argc);
        return JS_EXCEPTION;
    }

    JSValueConst js_array = argv[0];
    if (!JS_IsArray(ctx, js_array))
    {
        JS_ThrowTypeError(ctx, "move_midi_external_send() argument needs to be an Array");
        return JS_EXCEPTION;
    }

    JSValue length_val = JS_GetPropertyStr(ctx, js_array, "length");

    if (JS_IsException(length_val))
    {
        // Should not happen for a valid array
        return JS_EXCEPTION;
    }

    unsigned int len;
    JS_ToUint32(ctx, &len, length_val);
    JS_FreeValue(ctx, length_val);

    //printf("[");
    JSValue entry;
    for (int i = 0; i < len; i++)
    {

        JSValue val = JS_GetPropertyUint32(ctx, js_array, i);
        if (JS_IsException(val))
        {
            return JS_EXCEPTION;
        }

        uint32_t byte_val;
        if (JS_ToUint32(ctx, &byte_val, val) != 0)
        {
            JS_FreeValue(ctx, val);
            return JS_ThrowTypeError(ctx, "Array element at index %u is not a number", i);
        }

        if (byte_val > 255)
        {
            JS_FreeValue(ctx, val);
            return JS_ThrowRangeError(ctx, "Array element at index %u (%u) is out of byte range (0-255)", i, byte_val);
        }

        // total_sum += byte_val;
        //printf("%d(%x)", byte_val, byte_val);
        /*if (i != len - 1)
        {
            printf(", ");
        }*/

        js_move_midi_send_buffer[send_buffer_index] = byte_val;
        send_buffer_index++;

        if (send_buffer_index >= js_move_midi_external_send_buffer_size)
        {
            JS_ThrowInternalError(ctx, "No more space in MIDI internal send buffer.");
            return JS_EXCEPTION;
        }

        JS_FreeValue(ctx, val);
    }

    //printf("]\n");

    if (cable == 0 && send_buffer_index == 4) {
        uint8_t cin = js_move_midi_send_buffer[0];
        uint8_t status = js_move_midi_send_buffer[1];
        uint8_t data1 = js_move_midi_send_buffer[2];
        uint8_t data2 = js_move_midi_send_buffer[3];
        uint8_t type = status & 0xF0;
        if (type == 0x90 || type == 0xB0) {
            queue_pending_led(cin, status, data1, data2);
            return JS_UNDEFINED;
        }
    }

    queueMidiSend(cable, (unsigned char *)js_move_midi_send_buffer, send_buffer_index);
    return JS_UNDEFINED;
}

static JSValue js_move_midi_external_send(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    return js_move_midi_send(2, ctx, this_val, argc, argv);
}

static JSValue js_move_midi_internal_send(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    return js_move_midi_send(0, ctx, this_val, argc, argv);
}

static JSValue js_exit(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    printf("Exit...\n");
    global_exit_flag = 1;
    return JS_UNDEFINED;
}

/* Wrapper functions for module manager MIDI callbacks */
static int mm_midi_send_internal_wrapper(const uint8_t *msg, int len) {
    return queueInternalMidiSend((unsigned char *)msg, len);
}

static int mm_midi_send_external_wrapper(const uint8_t *msg, int len) {
    return queueExternalMidiSend((unsigned char *)msg, len);
}

/* JS bindings for module management */

/* host_list_modules() -> [{id, name, version}, ...] */
static JSValue js_host_list_modules(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (!g_module_manager_initialized) {
        return JS_NewArray(ctx);
    }

    JSValue arr = JS_NewArray(ctx);
    int count = mm_get_module_count(&g_module_manager);

    for (int i = 0; i < count; i++) {
        const module_info_t *info = mm_get_module_info(&g_module_manager, i);
        if (!info) continue;

        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, info->id));
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, info->name));
        JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, info->version));
        JS_SetPropertyStr(ctx, obj, "index", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, obj, "component_type", JS_NewString(ctx, info->component_type));
        /* Check if ui.js exists for standalone mode */
        int has_ui = (info->ui_script[0] && access(info->ui_script, F_OK) == 0) ? 1 : 0;
        JS_SetPropertyStr(ctx, obj, "has_ui", JS_NewBool(ctx, has_ui));
        JS_SetPropertyUint32(ctx, arr, i, obj);
    }

    return arr;
}

/* Helper: load and eval a JS file without exiting on failure */
static int eval_file_safe(JSContext *ctx, const char *filename, int module)
{
    uint8_t *buf;
    int ret, eval_flags = JS_EVAL_FLAG_STRICT;
    size_t buf_len;

    printf("Loading module UI script: %s\n", filename);
    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        printf("Failed to load: %s\n", filename);
        return -1;
    }

    if (module)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;

    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);

    if (ret == 0) {
        /* Signal main loop to refresh JS function references */
        g_js_functions_need_refresh = 1;

        /* Call init() if it exists */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue init_func = JS_GetPropertyStr(ctx, global, "init");
        if (JS_IsFunction(ctx, init_func)) {
            JSValue result = JS_Call(ctx, init_func, global, 0, NULL);
            if (JS_IsException(result)) {
                js_std_dump_error(ctx);
            }
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, init_func);
        JS_FreeValue(ctx, global);
    }

    return ret;
}

static int eval_file_no_init(JSContext *ctx, const char *filename, int module)
{
    uint8_t *buf;
    int ret, eval_flags = JS_EVAL_FLAG_STRICT;
    size_t buf_len;

    printf("Loading module UI script: %s\n", filename);
    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        printf("Failed to load: %s\n", filename);
        return -1;
    }

    if (module)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;

    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);

    if (ret != 0) {
        printf("Failed to eval: %s\n", filename);
    }

    return ret;
}

/* host_load_module(id_or_index) -> bool */
static JSValue js_host_load_module(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1 || !g_module_manager_initialized) {
        return JS_FALSE;
    }

    int result;
    int index = -1;
    if (JS_IsNumber(argv[0])) {
        JS_ToInt32(ctx, &index, argv[0]);
        result = mm_load_module(&g_module_manager, index);
    } else {
        const char *id = JS_ToCString(ctx, argv[0]);
        if (!id) return JS_FALSE;
        result = mm_load_module_by_id(&g_module_manager, id);
        JS_FreeCString(ctx, id);
    }

    /* If DSP loaded successfully, also load module's UI script */
    if (result == 0) {
        const module_info_t *info = mm_get_current_module(&g_module_manager);
        if (info && info->ui_script[0]) {
            /* Load as ES module to enable imports */
            eval_file_safe(ctx, info->ui_script, 1);
        }
    }

    return result == 0 ? JS_TRUE : JS_FALSE;
}

/* host_load_ui_module(path) -> bool */
static JSValue js_host_load_ui_module(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    int ret = eval_file_no_init(ctx, path, 1);
    JS_FreeCString(ctx, path);

    return ret == 0 ? JS_TRUE : JS_FALSE;
}

/* host_unload_module() */
static JSValue js_host_unload_module(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (g_module_manager_initialized) {
        mm_unload_module(&g_module_manager);
        g_silence_blocks = 8;
    }
    return JS_UNDEFINED;
}

/* host_return_to_menu() */
static JSValue js_host_return_to_menu(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    g_reload_menu_ui = 1;
    return JS_UNDEFINED;
}

/* host_module_set_param(key, val) */
static JSValue js_host_module_set_param(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 2 || !g_module_manager_initialized) {
        return JS_UNDEFINED;
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);

    if (key && val) {
        mm_set_param(&g_module_manager, key, val);
    }

    if (key) JS_FreeCString(ctx, key);
    if (val) JS_FreeCString(ctx, val);

    return JS_UNDEFINED;
}

/* host_module_get_param(key) -> string or undefined */
static JSValue js_host_module_get_param(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1 || !g_module_manager_initialized) {
        return JS_UNDEFINED;
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_UNDEFINED;

    char buf[1024];
    int len = mm_get_param(&g_module_manager, key, buf, sizeof(buf));
    JS_FreeCString(ctx, key);

    if (len < 0) {
        return JS_UNDEFINED;
    }

    return JS_NewString(ctx, buf);
}

/* host_module_get_error() -> string or undefined */
static JSValue js_host_module_get_error(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    if (!g_module_manager_initialized) {
        return JS_UNDEFINED;
    }

    char buf[512];
    int len = mm_get_error(&g_module_manager, buf, sizeof(buf));

    if (len <= 0) {
        return JS_UNDEFINED;  /* No error */
    }

    return JS_NewString(ctx, buf);
}

/* host_module_send_midi([status, data1, data2], source) */
static JSValue js_host_module_send_midi(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1 || !g_module_manager_initialized) {
        return JS_UNDEFINED;
    }

    if (!JS_IsArray(ctx, argv[0])) {
        return JS_UNDEFINED;
    }

    uint32_t len = 0;
    JSValue len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(len_val) || JS_ToUint32(ctx, &len, len_val) < 0) {
        JS_FreeValue(ctx, len_val);
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, len_val);
    if (len < 3) {
        return JS_UNDEFINED;
    }

    uint8_t msg[3];
    for (uint32_t i = 0; i < 3; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
        int32_t val = 0;
        JS_ToInt32(ctx, &val, v);
        JS_FreeValue(ctx, v);
        msg[i] = (uint8_t)val;
    }

    int source = MOVE_MIDI_SOURCE_INTERNAL;
    if (argc >= 2) {
        if (JS_IsNumber(argv[1])) {
            JS_ToInt32(ctx, &source, argv[1]);
        } else {
            const char *src = JS_ToCString(ctx, argv[1]);
            if (src) {
                if (strcmp(src, "external") == 0) {
                    source = MOVE_MIDI_SOURCE_EXTERNAL;
                } else if (strcmp(src, "host") == 0) {
                    source = MOVE_MIDI_SOURCE_HOST;
                } else {
                    source = MOVE_MIDI_SOURCE_INTERNAL;
                }
                JS_FreeCString(ctx, src);
            }
        }
    }

    mm_on_midi(&g_module_manager, msg, 3, source);
    return JS_UNDEFINED;
}

/* host_is_module_loaded() -> bool */
static JSValue js_host_is_module_loaded(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (!g_module_manager_initialized) {
        return JS_FALSE;
    }
    return mm_is_module_loaded(&g_module_manager) ? JS_TRUE : JS_FALSE;
}

/* host_get_current_module() -> {id, name, version} or null */
static JSValue js_host_get_current_module(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (!g_module_manager_initialized) {
        return JS_NULL;
    }

    const module_info_t *info = mm_get_current_module(&g_module_manager);
    if (!info) {
        return JS_NULL;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewString(ctx, info->id));
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, info->name));
    JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, info->version));
    JS_SetPropertyStr(ctx, obj, "ui_script", JS_NewString(ctx, info->ui_script));

    return obj;
}

/* host_rescan_modules() -> count */
static JSValue js_host_rescan_modules(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (!g_module_manager_initialized) {
        return JS_NewInt32(ctx, 0);
    }

    int count = mm_scan_modules(&g_module_manager, DEFAULT_MODULES_DIR);
    return JS_NewInt32(ctx, count);
}

/* host_get_volume() -> int (0-100) */
static JSValue js_host_get_volume(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    if (!g_module_manager_initialized) {
        return JS_NewInt32(ctx, 100);
    }
    return JS_NewInt32(ctx, mm_get_host_volume(&g_module_manager));
}

/* host_set_volume(volume) -> void */
static JSValue js_host_set_volume(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    if (argc < 1 || !g_module_manager_initialized) {
        return JS_UNDEFINED;
    }

    int volume;
    if (JS_ToInt32(ctx, &volume, argv[0])) {
        return JS_UNDEFINED;
    }

    mm_set_host_volume(&g_module_manager, volume);
    return JS_UNDEFINED;
}

/* host_get_setting(key) -> string or undefined */
static JSValue js_host_get_setting(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_UNDEFINED;
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) {
        return JS_UNDEFINED;
    }

    JSValue result = JS_UNDEFINED;

    if (strcmp(key, "velocity_curve") == 0) {
        result = JS_NewString(ctx, settings_velocity_curve_name(g_settings.velocity_curve));
    } else if (strcmp(key, "aftertouch_enabled") == 0) {
        result = JS_NewInt32(ctx, g_settings.aftertouch_enabled);
    } else if (strcmp(key, "aftertouch_deadzone") == 0) {
        result = JS_NewInt32(ctx, g_settings.aftertouch_deadzone);
    } else if (strcmp(key, "pad_layout") == 0) {
        result = JS_NewString(ctx, settings_pad_layout_name(g_settings.pad_layout));
    } else if (strcmp(key, "clock_mode") == 0) {
        const char *mode_names[] = {"off", "internal", "external"};
        result = JS_NewString(ctx, mode_names[g_settings.clock_mode]);
    } else if (strcmp(key, "tempo_bpm") == 0) {
        result = JS_NewInt32(ctx, g_settings.tempo_bpm);
    }

    JS_FreeCString(ctx, key);
    return result;
}

/* host_set_setting(key, value) -> void */
static JSValue js_host_set_setting(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_UNDEFINED;
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) {
        return JS_UNDEFINED;
    }

    if (strcmp(key, "velocity_curve") == 0) {
        const char *val = JS_ToCString(ctx, argv[1]);
        if (val) {
            g_settings.velocity_curve = settings_parse_velocity_curve(val);
            JS_FreeCString(ctx, val);
        }
    } else if (strcmp(key, "aftertouch_enabled") == 0) {
        int val;
        if (!JS_ToInt32(ctx, &val, argv[1])) {
            g_settings.aftertouch_enabled = val ? 1 : 0;
        }
    } else if (strcmp(key, "aftertouch_deadzone") == 0) {
        int val;
        if (!JS_ToInt32(ctx, &val, argv[1])) {
            if (val < 0) val = 0;
            if (val > 50) val = 50;
            g_settings.aftertouch_deadzone = val;
        }
    } else if (strcmp(key, "pad_layout") == 0) {
        const char *val = JS_ToCString(ctx, argv[1]);
        if (val) {
            g_settings.pad_layout = settings_parse_pad_layout(val);
            JS_FreeCString(ctx, val);
        }
    } else if (strcmp(key, "clock_mode") == 0) {
        const char *val = JS_ToCString(ctx, argv[1]);
        if (val) {
            if (strcmp(val, "off") == 0) g_settings.clock_mode = CLOCK_MODE_OFF;
            else if (strcmp(val, "internal") == 0) g_settings.clock_mode = CLOCK_MODE_INTERNAL;
            else if (strcmp(val, "external") == 0) g_settings.clock_mode = CLOCK_MODE_EXTERNAL;
            JS_FreeCString(ctx, val);
            /* Reset clock state when mode changes */
            g_clock_started = 0;
            g_clock_accumulator = 0.0f;
        }
    } else if (strcmp(key, "tempo_bpm") == 0) {
        int val;
        if (!JS_ToInt32(ctx, &val, argv[1])) {
            if (val < 20) val = 20;
            if (val > 300) val = 300;
            g_settings.tempo_bpm = val;
        }
    }

    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

/* host_save_settings() -> int (0 success, -1 error) */
static JSValue js_host_save_settings(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    int result = settings_save(&g_settings, SETTINGS_PATH);
    return JS_NewInt32(ctx, result);
}

/* host_reload_settings() -> void */
static JSValue js_host_reload_settings(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    settings_load(&g_settings, SETTINGS_PATH);
    return JS_UNDEFINED;
}

/* host_set_refresh_rate(hz) - set display refresh rate (1-60 Hz) */
static JSValue js_host_set_refresh_rate(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_UNDEFINED;
    }

    int hz;
    if (JS_ToInt32(ctx, &hz, argv[0])) {
        return JS_UNDEFINED;
    }

    /* Clamp to reasonable range */
    if (hz < 1) hz = 1;
    if (hz > 60) hz = 60;

    /* Convert Hz to tick interval (assuming ~344 ticks/sec from audio block rate) */
    display_refresh_interval = 344 / hz;
    if (display_refresh_interval < 1) display_refresh_interval = 1;

    /* Reset countdown so new rate takes effect immediately */
    display_countdown = 0;

    return JS_UNDEFINED;
}

/* host_get_refresh_rate() -> current refresh rate in Hz */
static JSValue js_host_get_refresh_rate(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    int hz = 344 / display_refresh_interval;
    return JS_NewInt32(ctx, hz);
}

/* host_flush_display() - force immediate display update */
static JSValue js_host_flush_display(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    /* Synchronously push all 6 display slices */
    for (int sync = 1; sync <= 6; sync++) {
        push_screen(sync);
        /* Trigger hardware to read the slice */
        if (global_fd >= 0) {
            ioctl(global_fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
        }
        /* Delay to let hardware process each slice */
        struct timespec ts = { 0, 3000000 };  /* 3ms per slice */
        nanosleep(&ts, NULL);
    }
    /* Extra delay after full flush to ensure display is updated */
    struct timespec final_delay = { 0, 50000000 };  /* 50ms */
    nanosleep(&final_delay, NULL);
    display_pending = 0;
    screen_dirty = 0;
    return JS_UNDEFINED;
}

/* Helper: validate path is within BASE_DIR to prevent directory traversal */
static int validate_path(const char *path) {
    if (!path || strlen(path) < strlen(BASE_DIR)) return 0;
    if (strncmp(path, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    if (strstr(path, "..") != NULL) return 0;
    return 1;
}

/* host_file_exists(path) -> bool */
static JSValue js_host_file_exists(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    struct stat st;
    int exists = (stat(path, &st) == 0);

    JS_FreeCString(ctx, path);
    return exists ? JS_TRUE : JS_FALSE;
}

/* host_http_download(url, dest_path) -> bool */
static JSValue js_host_http_download(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *url = JS_ToCString(ctx, argv[0]);
    const char *dest_path = JS_ToCString(ctx, argv[1]);

    if (!url || !dest_path) {
        if (url) JS_FreeCString(ctx, url);
        if (dest_path) JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    /* Validate destination path */
    if (!validate_path(dest_path)) {
        fprintf(stderr, "host_http_download: invalid dest path: %s\n", dest_path);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    /* Build curl command - use -k to skip SSL verification, timeouts to prevent hangs */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -fsSLk --connect-timeout 10 --max-time 120 -o \"%s\" \"%s\" 2>&1",
             CURL_PATH, dest_path, url);

    int result = system(cmd);

    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, dest_path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_extract_tar(tar_path, dest_dir) -> bool */
static JSValue js_host_extract_tar(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Build tar command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" 2>&1",
             tar_path, dest_dir);

    int result = system(cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_extract_tar_strip(tar_path, dest_dir, strip_components) -> bool */
static JSValue js_host_extract_tar_strip(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    if (argc < 3) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);
    int strip = 0;
    JS_ToInt32(ctx, &strip, argv[2]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar_strip: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate strip components (0-5 reasonable range) */
    if (strip < 0 || strip > 5) {
        fprintf(stderr, "host_extract_tar_strip: invalid strip value: %d\n", strip);
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Build tar command with --strip-components */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" --strip-components=%d 2>&1",
             tar_path, dest_dir, strip);

    int result = system(cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_remove_dir(path) -> bool */
static JSValue js_host_remove_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    /* Validate path - must be within modules directory for safety */
    if (!validate_path(path)) {
        fprintf(stderr, "host_remove_dir: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Additional safety: must be within modules directory */
    if (strncmp(path, DEFAULT_MODULES_DIR, strlen(DEFAULT_MODULES_DIR)) != 0) {
        fprintf(stderr, "host_remove_dir: path must be within modules dir: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Build rm command */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>&1", path);

    int result = system(cmd);

    JS_FreeCString(ctx, path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_read_file(path) -> string or null */
static JSValue js_host_read_file(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_NULL;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_NULL;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_read_file: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Limit to 1MB for safety */
    if (size > 1024 * 1024) {
        fprintf(stderr, "host_read_file: file too large: %s\n", path);
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);

    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    JS_FreeCString(ctx, path);

    return result;
}

void init_javascript(JSRuntime **prt, JSContext **pctx)
{

    JSRuntime *rt;
    JSContext *ctx;
    memset(js_move_midi_send_buffer, 0, sizeof(js_move_midi_send_buffer));

    rt = JS_NewRuntime();
    if (!rt)
    {
        fprintf(stderr, "qjs: cannot allocate JS runtime\n");
        exit(2);
    }

    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);

    ctx = JS_NewCustomContext(rt);
    if (!ctx)
    {
        fprintf(stderr, "qjs: cannot allocate JS context\n");
        exit(2);
    }

    js_std_add_helpers(ctx, -1, 0);
    // char jsCode[] = "console.log('Hello world!')\0";
    // char jsCode[] = "console.log('Hello world!');\0";

    // eval_buf(ctx, jsCode, strlen(jsCode), "<cmdline>", 0);
    JSValue global_obj = JS_GetGlobalObject(ctx);

    JSValue move_midi_external_send_func = JS_NewCFunction(ctx, js_move_midi_external_send, "move_midi_external_send", 1);
    JS_SetPropertyStr(ctx, global_obj, "move_midi_external_send", move_midi_external_send_func);

    JSValue move_midi_internal_send_func = JS_NewCFunction(ctx, js_move_midi_internal_send, "move_midi_internal_send", 1);
    JS_SetPropertyStr(ctx, global_obj, "move_midi_internal_send", move_midi_internal_send_func);

    JSValue set_pixel_func = JS_NewCFunction(ctx, js_set_pixel, "set_pixel", 1);
    JS_SetPropertyStr(ctx, global_obj, "set_pixel", set_pixel_func);

    JSValue draw_rect_func = JS_NewCFunction(ctx, js_draw_rect, "draw_rect", 1);
    JS_SetPropertyStr(ctx, global_obj, "draw_rect", draw_rect_func);

    JSValue fill_rect_func = JS_NewCFunction(ctx, js_fill_rect, "fill_rect", 1);
    JS_SetPropertyStr(ctx, global_obj, "fill_rect", fill_rect_func);

    JSValue clear_screen_func = JS_NewCFunction(ctx, js_clear_screen, "clear_screen", 0);
    JS_SetPropertyStr(ctx, global_obj, "clear_screen", clear_screen_func);

    JSValue get_int16_func = JS_NewCFunction(ctx, js_get_int16, "get_int16", 0);
    JS_SetPropertyStr(ctx, global_obj, "get_int16", get_int16_func);

    JSValue set_int16_func = JS_NewCFunction(ctx, js_set_int16, "set_int16", 0);
    JS_SetPropertyStr(ctx, global_obj, "set_int16", set_int16_func);

    JSValue print_func = JS_NewCFunction(ctx, js_print, "print", 1);
    JS_SetPropertyStr(ctx, global_obj, "print", print_func);

    JSValue exit_func = JS_NewCFunction(ctx, js_exit, "exit", 0);
    JS_SetPropertyStr(ctx, global_obj, "exit", exit_func);

    /* Module management functions */
    JSValue host_list_modules_func = JS_NewCFunction(ctx, js_host_list_modules, "host_list_modules", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_list_modules", host_list_modules_func);

    JSValue host_load_module_func = JS_NewCFunction(ctx, js_host_load_module, "host_load_module", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_load_module", host_load_module_func);

    JSValue host_load_ui_module_func = JS_NewCFunction(ctx, js_host_load_ui_module, "host_load_ui_module", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_load_ui_module", host_load_ui_module_func);

    JSValue host_unload_module_func = JS_NewCFunction(ctx, js_host_unload_module, "host_unload_module", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_unload_module", host_unload_module_func);

    JSValue host_return_to_menu_func = JS_NewCFunction(ctx, js_host_return_to_menu, "host_return_to_menu", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_return_to_menu", host_return_to_menu_func);

    JSValue host_module_set_param_func = JS_NewCFunction(ctx, js_host_module_set_param, "host_module_set_param", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_module_set_param", host_module_set_param_func);

    JSValue host_module_get_param_func = JS_NewCFunction(ctx, js_host_module_get_param, "host_module_get_param", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_module_get_param", host_module_get_param_func);

    JSValue host_module_get_error_func = JS_NewCFunction(ctx, js_host_module_get_error, "host_module_get_error", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_module_get_error", host_module_get_error_func);

    JSValue host_module_send_midi_func = JS_NewCFunction(ctx, js_host_module_send_midi, "host_module_send_midi", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_module_send_midi", host_module_send_midi_func);

    JSValue host_is_module_loaded_func = JS_NewCFunction(ctx, js_host_is_module_loaded, "host_is_module_loaded", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_is_module_loaded", host_is_module_loaded_func);

    JSValue host_get_current_module_func = JS_NewCFunction(ctx, js_host_get_current_module, "host_get_current_module", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_get_current_module", host_get_current_module_func);

    JSValue host_rescan_modules_func = JS_NewCFunction(ctx, js_host_rescan_modules, "host_rescan_modules", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_rescan_modules", host_rescan_modules_func);

    JSValue host_get_volume_func = JS_NewCFunction(ctx, js_host_get_volume, "host_get_volume", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_get_volume", host_get_volume_func);

    JSValue host_set_volume_func = JS_NewCFunction(ctx, js_host_set_volume, "host_set_volume", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_set_volume", host_set_volume_func);

    JSValue host_get_setting_func = JS_NewCFunction(ctx, js_host_get_setting, "host_get_setting", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_get_setting", host_get_setting_func);

    JSValue host_set_setting_func = JS_NewCFunction(ctx, js_host_set_setting, "host_set_setting", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_set_setting", host_set_setting_func);

    JSValue host_save_settings_func = JS_NewCFunction(ctx, js_host_save_settings, "host_save_settings", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_save_settings", host_save_settings_func);

    JSValue host_reload_settings_func = JS_NewCFunction(ctx, js_host_reload_settings, "host_reload_settings", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_reload_settings", host_reload_settings_func);

    JSValue host_set_refresh_rate_func = JS_NewCFunction(ctx, js_host_set_refresh_rate, "host_set_refresh_rate", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_set_refresh_rate", host_set_refresh_rate_func);

    JSValue host_get_refresh_rate_func = JS_NewCFunction(ctx, js_host_get_refresh_rate, "host_get_refresh_rate", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_get_refresh_rate", host_get_refresh_rate_func);

    JSValue host_flush_display_func = JS_NewCFunction(ctx, js_host_flush_display, "host_flush_display", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_flush_display", host_flush_display_func);

    /* Store module functions */
    JSValue host_file_exists_func = JS_NewCFunction(ctx, js_host_file_exists, "host_file_exists", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_file_exists", host_file_exists_func);

    JSValue host_http_download_func = JS_NewCFunction(ctx, js_host_http_download, "host_http_download", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_http_download", host_http_download_func);

    JSValue host_extract_tar_func = JS_NewCFunction(ctx, js_host_extract_tar, "host_extract_tar", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar", host_extract_tar_func);

    JSValue host_extract_tar_strip_func = JS_NewCFunction(ctx, js_host_extract_tar_strip, "host_extract_tar_strip", 3);
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar_strip", host_extract_tar_strip_func);

    JSValue host_remove_dir_func = JS_NewCFunction(ctx, js_host_remove_dir, "host_remove_dir", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_remove_dir", host_remove_dir_func);

    JSValue host_read_file_func = JS_NewCFunction(ctx, js_host_read_file, "host_read_file", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_read_file", host_read_file_func);

    JS_FreeValue(ctx, global_obj);

    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);

    // Free our reference to the global object

    // eval_file(ctx, "move_m8.js", -1);

    // JSValue val;
    // val = JS_Eval(ctx, jsCode, strlen(jsCode), "foo.js", 0);
    *prt = rt;
    *pctx = ctx;
}

int getGlobalFunction(JSContext **pctx, const char *func_name, JSValue *retFunc)
{

    JSContext *ctx = *pctx;

    // 4. Get the global object
    JSValue global_obj = JS_GetGlobalObject(ctx);

    // --- Find and Call the 'greet' function ---

    JSValue func = JS_GetPropertyStr(ctx, global_obj, func_name);

    // 5. Check if it's a function
    if (!JS_IsFunction(ctx, func))
    {
        fprintf(stderr, "Error: '%s' is not a function or not found.\n", func_name);
        JS_FreeValue(ctx, func); // Free the non-function value
        return 0;
    }

    *retFunc = func;

    return 1;
}

int callGlobalFunction(JSContext **pctx, JSValue *pfunc, unsigned char *data)
{
    JSContext *ctx = *pctx;

    JSValue ret;

    if (data != 0)
    {
        JSValue newArray;

        // args[0] = JS_NewString(ctx, "foo");
        newArray = JS_NewArray(ctx);

        JSValue num;
        if (!JS_IsException(newArray))
        { // Check creation success

            for (int i = 0; i < 3; i++)
            {
                num = JS_NewInt32(ctx, data[i]);
                JS_SetPropertyUint32(ctx, newArray, i, num);
            }
        }

        JSValue args[1];
        args[0] = newArray;

        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 1, args);
    }
    else
    {
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 0, 0);
    }

    if (JS_IsException(ret))
    {
        printf("JS function failed\n");
        js_std_dump_error(ctx);
    }

    JS_FreeValue(ctx, ret);

    return JS_IsException(ret);
}

void deinit_javascript(JSRuntime **prt, JSContext **pctx)
{
    JSRuntime *rt = *prt;
    JSContext *ctx = *pctx;

    JSMemoryUsage stats;
    JS_ComputeMemoryUsage(rt, &stats);
    JS_DumpMemoryUsage(stdout, &stats, rt);

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

char packed_buffer[1024];

void push_screen(int sync) {
  // maybe this first 80=1 is necessary?
  if(sync == 0) {
    memset(mapped_memory+84, 0, 172);
    return;
  } else if(sync == 1) {
    int i = 0;
    for(int y = 0; y < 64/8; y++) {
      for(int x = 0; x < 128; x++) {
        int index = (y * 128 * 8) + x;
        unsigned char packed = 0;
        for(int j = 0; j<8; j++) {
          int packIndex = index + j * 128;
          packed |= screen_buffer[packIndex] << j;
        }
        packed_buffer[i] = packed;
        i++;
      }
    }
  }

  {
    int slice = sync - 1;
    mapped_memory[80] = slice+1;
    int sliceStart = 172 * slice;
    int sliceBytes = slice == 5 ? 164 : 172;
    for(int i = 0; i < sliceBytes; i++) {
      mapped_memory[84+i] = packed_buffer[sliceStart+i];
    }
  }
}

int main(int argc, char *argv[])
{

    JSRuntime *rt = 0;
    JSContext *ctx = 0;
    init_javascript(&rt, &ctx);

    char *command_line_script_name = 0;

    if (argc > 2)
    {
        printf("usage: move-anything <script.js>");
        exit(1);
    }

    if (argc == 2)
    {
        command_line_script_name = argv[1];
    }

    char default_script_name[] = "move_default.js";

    char *script_name = 0;

    if (command_line_script_name != 0)
    {
        printf("Loading script from command-line: %s\n", command_line_script_name);

        script_name = command_line_script_name;
    }
    else
    {
        printf("No script passed on the command-line, loading the default script: %s\n", default_script_name);
        script_name = default_script_name;
    }

    strncpy(g_menu_script_path, script_name, sizeof(g_menu_script_path) - 1);
    g_menu_script_path[sizeof(g_menu_script_path) - 1] = '\0';

    eval_file(ctx, script_name, -1);

    const char *device_path = "/dev/ablspi0.0";
    struct timespec sleep_time;
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 1 * 1000000;

    int fd;

    size_t length = 4096;
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;
    off_t offset = 0;

    // Open the device file.
    printf("Opening file\n");
    fd = open(device_path, O_RDWR);
    if (fd == -1)
    {
        perror("open");
        return 1;
    }

    global_fd = fd;

    printf("mmaping\n");
    mapped_memory = (unsigned char *)mmap(NULL, length, prot, flags, fd, offset);

    if (mapped_memory == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return 1;
    }

    // Clear mapped memory
    printf("Clearing mmapped memory\n");
    memset(mapped_memory, 0, 4096);
    reset_pending_leds();

    /* Initialize module manager */
    printf("Initializing module manager\n");
    mm_init(&g_module_manager, mapped_memory,
            mm_midi_send_internal_wrapper,
            mm_midi_send_external_wrapper);
    g_module_manager_initialized = 1;

    /* Scan for modules */
    printf("Scanning for modules in %s\n", DEFAULT_MODULES_DIR);
    int module_count = mm_scan_modules(&g_module_manager, DEFAULT_MODULES_DIR);
    printf("Found %d modules\n", module_count);

    /* Load host settings */
    printf("Loading host settings\n");
    settings_load(&g_settings, SETTINGS_PATH);

    int padIndex = 0;

    /*  // The lighting of white and RGB LEDs is controlled by note-on or control change messages sent to Push 2:

  Note On (nn):        1001cccc 0nnnnnnn 0vvvvvvv        [10010000 = 0x90 = 144]
  Control Change (cc): 1011cccc 0nnnnnnn 0vvvvvvv        [10110000 = 0xB0 = 176]
  The channel (cccc, 0…15) controls the LED animation, i.e. blinking, pulsing or one-shot transitions. Channel 0 means no animation. See LED Animation.

  The message type 1001 (for nn) or 1011 (for cc) and the note or controller number nnnnnnn (0…127) select which LED is addressed. See MIDI Mapping.

  The velocity vvvvvvv (0…127) selects a color index, which is interpreted differently for white and RGB LEDs. See Default Color Palettes (subset).
  */

    /*

        https://www.usb.org/sites/default/files/midi10.pdf

        CIN     MIDI_x Size     Description
        0x0     1, 2 or 3       Miscellaneous function codes. Reserved for future extensions.
        0x1     1, 2 or 3       Cable events. Reserved for future expansion.
        0x2     2               Two-byte System Common messages like MTC, SongSelect, etc.
        0x3     3               Three-byte System Common messages like SPP, etc.
        0x4     3               SysEx starts or continues
        0x5     1               Single-byte System Common Message or SysEx ends with following single byte.
        0x6     2               SysEx ends with following two bytes.
        0x7     3               SysEx ends with following three bytes.
        0x8     3               Note-off
        0x9     3               Note-on
        0xA     3               Poly-KeyPress
        0xB     3               Control Change
        0xC     2               Program Change
        0xD     2               Channel Pressure
        0xE     3               PitchBend Change
        0xF     1               Single Byte



        currentOutput.send([0xF0, 126, 0, 6, 2, 0, 32, 41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7]);

    */

    enum
    {
        SYSEX_START_OR_CONTINUE = 0x4,
        SYSEX_END_SINGLE_BYTE = 0x5,
        SYSEX_END_TWO_BYTE = 0x6,
        SYSEX_END_THREE_BYTE = 0x7,
        NOTE_OFF = 0x8,
        NOTE_ON = 0x9,
        POLY_KEYPRESS = 0xA,
        CONTROL_CHANGE = 0xB,
        PROGRAM_CHANGE = 0xC,
        CHANNEL_PRESSURE = 0xD,
        PITCH_BEND = 0xE,
        SINGLE_BYTE = 0xF
    };

    int ioctl_result = 0;
    ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xb, 0), 0x1312d00);

    clearPads(mapped_memory, fd);
    clearSequencerButtons(mapped_memory, fd);

    JSValue JSonMidiMessageExternal;
    getGlobalFunction(&ctx, "onMidiMessageExternal", &JSonMidiMessageExternal);

    JSValue JSonMidiMessageInternal;
    getGlobalFunction(&ctx, "onMidiMessageInternal", &JSonMidiMessageInternal);

    JSValue JSinit;
    getGlobalFunction(&ctx, "init", &JSinit);

    JSValue JSTick;
    int jsTickIsDefined = getGlobalFunction(&ctx, "tick", &JSTick);

    printf("JS:calling init\n");
    if(callGlobalFunction(&ctx, &JSinit, 0)) {
      printf("JS:init failed\n");
    }

    while (!global_exit_flag)
    {
        if (g_reload_menu_ui) {
            g_reload_menu_ui = 0;
            printf("Host: Back detected - returning to menu\n");
            if (g_module_manager_initialized) {
                mm_unload_module(&g_module_manager);
                g_silence_blocks = 8;
            }
            if (g_menu_script_path[0]) {
                eval_file(ctx, g_menu_script_path, -1);
                JSValue JSinit;
                getGlobalFunction(&ctx, "init", &JSinit);
                if (callGlobalFunction(&ctx, &JSinit, 0)) {
                    printf("JS:init failed\n");
                }
                g_js_functions_need_refresh = 1;
            }
        }

        if (g_silence_blocks > 0) {
            memset(mapped_memory + MOVE_AUDIO_OUT_OFFSET, 0, MOVE_AUDIO_BYTES_PER_BLOCK);
            g_silence_blocks--;
        }
        /* Refresh JS function references if a module UI was loaded */
        if (g_js_functions_need_refresh) {
            g_js_functions_need_refresh = 0;
            JS_FreeValue(ctx, JSTick);
            JS_FreeValue(ctx, JSonMidiMessageInternal);
            JS_FreeValue(ctx, JSonMidiMessageExternal);
            jsTickIsDefined = getGlobalFunction(&ctx, "tick", &JSTick);
            getGlobalFunction(&ctx, "onMidiMessageInternal", &JSonMidiMessageInternal);
            getGlobalFunction(&ctx, "onMidiMessageExternal", &JSonMidiMessageExternal);
            printf("JS function references refreshed\n");
        }

        if (jsTickIsDefined)
        {
            if(callGlobalFunction(&ctx, &JSTick, 0)) {
              printf("JS:tick failed\n");
            }
        }

        /* Render audio from DSP module (if loaded) */
        if (mm_is_module_loaded(&g_module_manager)) {
            mm_render_block(&g_module_manager);
        }

        /* Generate MIDI clock if enabled */
        if (g_settings.clock_mode == CLOCK_MODE_INTERNAL && g_settings.tempo_bpm > 0) {
            /* Send MIDI Start on first block */
            if (!g_clock_started) {
                uint8_t start_msg[1] = { 0xFA };  /* MIDI Start */
                mm_on_midi(&g_module_manager, start_msg, 1, MOVE_MIDI_SOURCE_HOST);
                g_clock_started = 1;
                printf("MIDI clock started at %d BPM\n", g_settings.tempo_bpm);
            }

            /* Generate clock pulses - 24 per quarter note */
            float samples_per_clock = (float)SAMPLE_RATE * 60.0f / (float)g_settings.tempo_bpm / 24.0f;
            g_clock_accumulator += (float)FRAMES_PER_BLOCK;

            while (g_clock_accumulator >= samples_per_clock) {
                g_clock_accumulator -= samples_per_clock;
                uint8_t clock_msg[1] = { 0xF8 };  /* MIDI Timing Clock */
                mm_on_midi(&g_module_manager, clock_msg, 1, MOVE_MIDI_SOURCE_HOST);
            }
        }

        flush_pending_leds();

        ioctl_result = ioctl(fd, _IOC(_IOC_NONE, 0, 0xa, 0), 0x300);
        outgoing_midi_counter = 0;

        int startByte = 2048;
        int length = 256;
        int endByte = startByte + length;

        int control_change = 0xb;

        int out_index = 0;

        memset(((struct SPI_Memory *)mapped_memory)->outgoing_midi, 0, 256);

        for (int i = startByte; i < endByte; i += 4)
        {
            if ((unsigned int)mapped_memory[i] == 0)
            {
                continue;
            }

            unsigned char *byte = &mapped_memory[i];
            unsigned char cable = *byte >> 4;
            unsigned char code_index_number = (*byte & 0b00001111);
            unsigned char midi_0 = *(byte + 1);
            unsigned char midi_1 = *(byte + 2);
            unsigned char midi_2 = *(byte + 3);


            if (byte[1] + byte[2] + byte[3] == 0)
            {
                continue;
            }

            //printf("%x %x %x %x\n", byte[0], byte[1], byte[2], byte[3]);

            /* Check if module wants raw MIDI (skip transforms) */
            int apply_transforms = !mm_module_wants_raw_midi(&g_module_manager);

            if (cable == 2)
            {
                /* External MIDI: no transforms, no UI - direct to DSP only */
                /* Route to DSP plugin */
                mm_on_midi(&g_module_manager, &byte[1], 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }

            if (cable == 0)
            {
                /* Process host-level shortcuts and apply transforms */
                int consumed = 0;

                /* Check if this is an internal control note that should be filtered from DSP
                 * For raw_midi modules, only pad notes (68-99) should go to DSP.
                 * Filter: capacitive touch (0-9), step buttons (16-31), track buttons (40-43) */
                uint8_t status = byte[1] & 0xF0;
                uint8_t note = byte[2];
                int is_internal_control = 0;
                if (status == 0x90 || status == 0x80) {
                    /* Note on/off - check if it's a control note */
                    is_internal_control = (note < 10) ||           /* capacitive touch */
                                          (note >= 16 && note <= 31) ||  /* step buttons */
                                          (note >= 40 && note <= 43);    /* track buttons */
                }

                if (!consumed) {
                    consumed = process_host_midi(&byte[1], apply_transforms);
                }

                /* Route to JS handler (unless consumed by host) - UI receives capacitive touch */
                if (!consumed && callGlobalFunction(&ctx, &JSonMidiMessageInternal, &byte[1])) {
                  printf("JS:onMidiMessageInternal failed\n");
                }
                /* Route to DSP plugin (unless consumed OR internal control note) */
                if (!consumed && !is_internal_control) {
                  mm_on_midi(&g_module_manager, &byte[1], 3, MOVE_MIDI_SOURCE_INTERNAL);
                }
            }

        }

        /* Start new display push if pending and not already pushing */
        if (display_pending && screen_dirty == 0) {
            screen_dirty = 1;
            display_pending = 0;
        }

        /* Continue pushing display if in progress */
        if(screen_dirty >= 1) {
          push_screen(screen_dirty-1);
          if(screen_dirty == 7) {
            screen_dirty = 0;
          } else {
            screen_dirty++;
          }
        }
    }

    if (munmap(mapped_memory, length) == -1)
    {
        perror("munmap");
    }

    close(fd);

    /* Cleanup module manager */
    printf("Cleaning up module manager\n");
    mm_destroy(&g_module_manager);
    g_module_manager_initialized = 0;

    printf("Deinitialize JS\n");

    JS_FreeValue(ctx, JSonMidiMessageExternal);
    JS_FreeValue(ctx, JSonMidiMessageInternal);
    JS_FreeValue(ctx, JSinit);
    if (jsTickIsDefined) {
        JS_FreeValue(ctx, JSTick);
    }


    printf("Exiting\n");
    // deinit is currenlty failing due to there being JS objects hanging around so...
    exit(0);
    deinit_javascript(&rt, &ctx);

    // js_std_free_handlers(rt);

    // JS_FreeContext(ctx);
    // JS_FreeRuntime(rt);

    return 0;
}
