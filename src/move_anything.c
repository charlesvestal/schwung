#include <sys/mman.h>
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

#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"

#include "host/module_manager.h"

int global_fd = -1;
int global_exit_flag = 0;

/* Host-level input state for system shortcuts */
int host_shift_held = 0;

/* Move MIDI CC constants for system shortcuts */
#define CC_SHIFT 49
#define CC_JOG_CLICK 3

/* Module manager instance */
module_manager_t g_module_manager;
int g_module_manager_initialized = 0;

/* Flag to refresh JS function references after module UI load */
int g_js_functions_need_refresh = 0;

/* Default modules directory */
#define DEFAULT_MODULES_DIR "/data/UserData/move-anything/modules"

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
  if(screen_dirty == 0) {
    screen_dirty = 1;
  }
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

  /* Render with threshold (no anti-aliasing for 1-bit display) */
  int draw_x = sx + x0;
  int draw_y = sy + font->ttf_ascent + y0;

  for (int yi = 0; yi < h; yi++) {
    for (int xi = 0; xi < w; xi++) {
      if (bitmap[yi * w + xi] > 127) {
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

/* Process host-level MIDI for system shortcuts (Shift+Wheel to exit) */
/* Returns 1 if message was consumed by host, 0 if should pass to module */
int process_host_midi(unsigned char midi_0, unsigned char midi_1, unsigned char midi_2) {
  /* Only handle internal MIDI (cable 0) control changes */
  if ((midi_0 & 0xF0) != 0xB0) {
    return 0;  /* Not a CC, pass through */
  }

  unsigned char cc = midi_1;
  unsigned char value = midi_2;

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

    // flushMidi();
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

/* host_unload_module() */
static JSValue js_host_unload_module(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (g_module_manager_initialized) {
        mm_unload_module(&g_module_manager);
    }
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

    JSValue host_unload_module_func = JS_NewCFunction(ctx, js_host_unload_module, "host_unload_module", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_unload_module", host_unload_module_func);

    JSValue host_module_set_param_func = JS_NewCFunction(ctx, js_host_module_set_param, "host_module_set_param", 2);
    JS_SetPropertyStr(ctx, global_obj, "host_module_set_param", host_module_set_param_func);

    JSValue host_module_get_param_func = JS_NewCFunction(ctx, js_host_module_get_param, "host_module_get_param", 1);
    JS_SetPropertyStr(ctx, global_obj, "host_module_get_param", host_module_get_param_func);

    JSValue host_is_module_loaded_func = JS_NewCFunction(ctx, js_host_is_module_loaded, "host_is_module_loaded", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_is_module_loaded", host_is_module_loaded_func);

    JSValue host_get_current_module_func = JS_NewCFunction(ctx, js_host_get_current_module, "host_get_current_module", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_get_current_module", host_get_current_module_func);

    JSValue host_rescan_modules_func = JS_NewCFunction(ctx, js_host_rescan_modules, "host_rescan_modules", 0);
    JS_SetPropertyStr(ctx, global_obj, "host_rescan_modules", host_rescan_modules_func);

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

            //printf("MIDI: cable=%d %02x %02x %02x\n", cable, byte[1], byte[2], byte[3]);

            if (cable == 2)
            {
                /* Route to JS handler */
                if(callGlobalFunction(&ctx, &JSonMidiMessageExternal, &byte[1])) {
                  printf("JS:onMidiMessageExternal failed\n");
                }
                /* Route to DSP plugin */
                mm_on_midi(&g_module_manager, &byte[1], 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }

            if (cable == 0)
            {
                /* Process host-level shortcuts (Shift+Wheel to exit) */
                int consumed = process_host_midi(midi_0, midi_1, midi_2);

                /* Route to JS handler (unless consumed by host) */
                if (!consumed && callGlobalFunction(&ctx, &JSonMidiMessageInternal, &byte[1])) {
                  printf("JS:onMidiMessageInternal failed\n");
                }
                /* Route to DSP plugin */
                mm_on_midi(&g_module_manager, &byte[1], 3, MOVE_MIDI_SOURCE_INTERNAL);
            }

        }

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
