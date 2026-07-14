#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "js_display.h"

static int pixel(int x, int y) {
    return js_display_screen_buffer[y * DISPLAY_WIDTH + x];
}

int main(void) {
    js_display_clear_clip();
    js_display_clear();

    js_display_set_clip(10, 20, 2, 3);
    js_display_set_pixel(9, 20, 1);
    js_display_set_pixel(10, 20, 1);
    js_display_set_pixel(11, 22, 1);
    js_display_set_pixel(12, 22, 1);
    assert(pixel(9, 20) == 0);
    assert(pixel(10, 20) == 1);
    assert(pixel(11, 22) == 1);
    assert(pixel(12, 22) == 0);

    js_display_clear();
    js_display_set_pixel(9, 20, 1);
    js_display_set_pixel(10, 20, 1);
    assert(pixel(9, 20) == 0);
    assert(pixel(10, 20) == 1);

    js_display_clear();
    js_display_set_clip(4, 4, 2, 2);
    js_display_fill_rect(3, 3, 4, 4, 1);
    assert(pixel(3, 3) == 0);
    assert(pixel(4, 4) == 1);
    assert(pixel(5, 5) == 1);
    assert(pixel(6, 6) == 0);

    js_display_clear();
    js_display_set_clip(0, 0, 0, 10);
    js_display_set_pixel(0, 0, 1);
    assert(pixel(0, 0) == 0);

    js_display_set_clip(-1, -1, 2, 2);
    js_display_set_pixel(0, 0, 1);
    js_display_set_pixel(1, 0, 1);
    assert(pixel(0, 0) == 1);
    assert(pixel(1, 0) == 0);

    js_display_clear();
    js_display_set_clip(INT_MAX, INT_MAX, INT_MAX, INT_MAX);
    js_display_set_pixel(DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1, 1);
    assert(pixel(DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1) == 0);

    js_display_set_clip(INT_MIN, INT_MIN, INT_MAX, INT_MAX);
    js_display_set_pixel(0, 0, 1);
    assert(pixel(0, 0) == 0);

    js_display_clear_clip();
    js_display_set_pixel(0, 0, 1);
    assert(pixel(0, 0) == 1);

    printf("ALL PASS\n");
    return 0;
}
