#include "solar_os_asm_runtime.h"

/* asmparser2's ScriptExecutable layout depends on this macro (its component
 * defines it privately), so every translation unit including it must too. */
#ifndef ESP32
#define ESP32 1
#endif

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_config.h"
extern "C" {
#include "solar_os_task.h"
}

#include "asm_execute.h"
#include "binding.h"
#include "script_executable.h"

#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
#include "rgb_lgfx.h"
#endif

struct solar_os_asm_program {
    ScriptExecutable exe;
    /* The task running the script (solar_os_asm_start()), and whether the
     * script has returned. */
    TaskHandle_t task;
    volatile bool finished;
    const char *function;
};

/* ------------------------------------------------------------ colours */

/* FastLED's hsv2rgb_rainbow (MIT, (c) 2013 FastLED) so hsv()/hsv2() give
 * the same colours as the MaTouch sketches, which used CHSV -> CRGB. */
struct script_crgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

static inline uint8_t scale8(uint8_t i, uint8_t scale)
{
    return (uint8_t)(((uint16_t)i * (1U + (uint16_t)scale)) >> 8);
}

static inline uint8_t scale8_video(uint8_t i, uint8_t scale)
{
    return (uint8_t)((((int)i * (int)scale) >> 8) + ((i && scale) ? 1 : 0));
}

static script_crgb hsv_rainbow(uint8_t hue, uint8_t sat, uint8_t val)
{
    const uint8_t offset8 = (uint8_t)((hue & 0x1F) << 3);
    const uint8_t third = scale8(offset8, 256 / 3);
    const uint8_t twothirds = scale8(offset8, (256 * 2) / 3);
    uint8_t r, g, b;
    if (!(hue & 0x80)) {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) { r = 255 - third; g = third; b = 0; }
            else { r = 171; g = 85 + third; b = 0; }
        } else {
            if (!(hue & 0x20)) { r = 171 - twothirds; g = 170 + third; b = 0; }
            else { r = 0; g = 255 - third; b = third; }
        }
    } else {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) { r = 0; g = 171 - twothirds; b = 85 + twothirds; }
            else { r = third; g = 0; b = 255 - third; }
        } else {
            if (!(hue & 0x20)) { r = 85 + third; g = 0; b = 171 - third; }
            else { r = 170 + third; g = 0; b = 85 - third; }
        }
    }
    if (sat != 255) {
        if (sat == 0) {
            r = g = b = 255;
        } else {
            uint8_t desat = 255 - sat;
            desat = scale8_video(desat, desat);
            const uint8_t satscale = 255 - desat;
            r = scale8(r, satscale);
            g = scale8(g, satscale);
            b = scale8(b, satscale);
            r += desat;
            g += desat;
            b += desat;
        }
    }
    if (val != 255) {
        val = scale8_video(val, val);
        if (val == 0) {
            r = g = b = 0;
        } else {
            r = scale8(r, val);
            g = scale8(g, val);
            b = scale8(b, val);
        }
    }
    return {r, g, b};
}

static script_crgb host_hsv(int h, int s, int v)
{
    return hsv_rainbow((uint8_t)h, (uint8_t)s, (uint8_t)v);
}

/* RGB565, byte-swapped like the frame buffer (display). */
static uint16_t host_hsv2(int h, int s, int v)
{
    const script_crgb c = hsv_rainbow((uint8_t)h, (uint8_t)s, (uint8_t)v);
    const uint16_t j = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
    return (uint16_t)((j << 8) | (j >> 8));
}

/* ------------------------------------------------------------ misc */

static uint32_t host_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void host_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
}

static uint32_t host_rand(uint32_t n)
{
    return n > 0 ? esp_random() % n : 0;
}

static float host_sin(float x) { return sinf(x); }
static float host_atan2(float y, float x) { return atan2f(y, x); }
static float host_hypot(float x, float y) { return hypotf(x, y); }
static float host_log(float x) { return logf(x); }

static uint8_t host_sin8(uint8_t theta)
{
    return (uint8_t)(128.0f + 127.0f * sinf((float)theta * 6.2831853f / 256.0f));
}

static void host_reset_stat(void) {}

/* ------------------------------------------------------------ keyboard */

#define HID_USAGE_ESCAPE 0x29
#define HID_USAGE_RIGHT 0x4F
#define HID_USAGE_LEFT 0x50
#define HID_USAGE_DOWN 0x51
#define HID_USAGE_UP 0x52
#define HID_USAGE_ENTER 0x28
#define HID_USAGE_TAB 0x2B
#define KEY_RING 32

/* Layout of keyboard.h's key_return: what `key`, keyChar and keyCode see. */
struct script_key {
    uint8_t key_char;
    uint8_t key_code;
};

static script_key s_key;
/* readKey(): the app task produces, the script task consumes. */
static volatile uint8_t s_read_ring[KEY_RING];
static volatile uint8_t s_read_head, s_read_tail;
/* keyboard() callbacks: the app task produces, the asm worker consumes. */
static volatile script_key s_cb_ring[KEY_RING];
static volatile uint8_t s_cb_head, s_cb_tail;
static ScriptExecutable *volatile s_keyboard_target;
/* keyHeld(c): 1 while the key is down, by its readKey() character (arrows
 * as z/q/s/d, letters in lower case, Enter '\r', Tab '\t'). */
static volatile uint8_t s_held[256];

static void host_setup_keyboard(uint32_t handle)
{
    s_keyboard_target = reinterpret_cast<ScriptExecutable *>(handle);
}

/* The MaTouch games' convention: arrows as z/q/s/d, Escape as 27, and a
 * released direction as its letter + 128 (hold-to-move). */
static int host_read_key(void)
{
    if (s_read_tail == s_read_head) {
        return 0;
    }
    const uint8_t c = s_read_ring[s_read_tail];
    s_read_tail = (uint8_t)((s_read_tail + 1) % KEY_RING);
    return c;
}

static int host_key_held(int c)
{
    return c >= 0 && c < 256 ? s_held[c] : 0;
}

static void push_read(uint8_t c)
{
    const uint8_t next = (uint8_t)((s_read_head + 1) % KEY_RING);
    if (next != s_read_tail) {
        s_read_ring[s_read_head] = c;
        s_read_head = next;
    }
}

extern "C" void solar_os_asm_key_event(uint8_t character, uint8_t usage, bool pressed)
{
    uint8_t c = character;
    if (usage == HID_USAGE_ESCAPE) c = 27;
    else if (usage == HID_USAGE_UP) c = 'z';
    else if (usage == HID_USAGE_LEFT) c = 'q';
    else if (usage == HID_USAGE_DOWN) c = 's';
    else if (usage == HID_USAGE_RIGHT) c = 'd';
    {
        uint8_t h = c;
        if (usage == HID_USAGE_ENTER) h = '\r';
        else if (usage == HID_USAGE_TAB) h = '\t';
        else if (h >= 'A' && h <= 'Z') h = (uint8_t)(h - 'A' + 'a');
        s_held[h] = pressed ? 1 : 0;
    }
    if (!pressed) {
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        if (c == 'z' || c == 'q' || c == 's' || c == 'd') {
            push_read((uint8_t)(c | 0x80));
        }
        return;
    }
    if (c != 0) {
        push_read(c);
    }
    const uint8_t next = (uint8_t)((s_cb_head + 1) % KEY_RING);
    if (next != s_cb_tail) {
        s_cb_ring[s_cb_head].key_char = character;
        s_cb_ring[s_cb_head].key_code = usage;
        s_cb_head = next;
    }
}

extern "C" void solar_os_asm_keyboard_reset(void)
{
    s_keyboard_target = NULL;
    s_read_head = 0;
    s_read_tail = 0;
    s_cb_head = 0;
    s_cb_tail = 0;
    s_key.key_char = 0;
    s_key.key_code = 0;
    memset((void *)s_held, 0, sizeof(s_held));
}

extern "C" void solar_os_asm_keyboard_pump(void)
{
    while (s_cb_tail != s_cb_head) {
        s_key.key_char = s_cb_ring[s_cb_tail].key_char;
        s_key.key_code = s_cb_ring[s_cb_tail].key_code;
        s_cb_tail = (uint8_t)((s_cb_tail + 1) % KEY_RING);
        ScriptExecutable *target = s_keyboard_target;
        if (target != NULL) {
            (void)target->executeOnly("keyboard");
        }
    }
}

/* ------------------------------------------------------------ WAD */

/* The WAD the app read next to the script (game.sc -> game.wad), for the
 * scripts written for the asmparser2 Ironforge sketches: wad[] its bytes,
 * wadLoaded 1 when there is one, wadFind("TITLEPIC") and
 * wadSprite("HUSK", frame, angle) where a picture starts in wad[], -1 if
 * none. A WAD: "PWAD", the number of entries, where the directory is; the
 * directory: offset, size, an 8-character name each. */
static uint8_t *s_wad;
static int s_wad_loaded;

extern "C" void solar_os_asm_set_wad(uint8_t *data)
{
    s_wad = data;
    s_wad_loaded = data != NULL ? 1 : 0;
}

static int host_wad_find(const char *name)
{
    if (s_wad == NULL || name == NULL) {
        return -1;
    }
    int32_t n, dir;
    memcpy(&n, s_wad + 4, 4);
    memcpy(&dir, s_wad + 8, 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *e = s_wad + dir + 16 * i;
        if (strncmp((const char *)e + 8, name, 8) == 0) {
            int32_t pos;
            memcpy(&pos, e, 4);
            return pos;
        }
    }
    return -1;
}

static int host_wad_sprite(const char *prefix, int frame, int angle)
{
    char name[9];
    snprintf(name, sizeof(name), "%s%c%d", prefix, 'A' + frame, angle);
    return host_wad_find(name);
}

/* ------------------------------------------------------------ parameters */

static int s_argc;
static int32_t s_arg_raw[SOLAR_OS_ASM_ARGS_MAX]; /* int, float bit pattern or text address */
static int32_t s_arg_int[SOLAR_OS_ASM_ARGS_MAX];
static float s_arg_float[SOLAR_OS_ASM_ARGS_MAX];
/* Every parameter's text, kept for the whole run: a parameter that is not a
 * number is passed by address to a char * parameter (and argstr(i)). */
static char s_arg_text[SOLAR_OS_ASM_ARGS_MAX][64];

extern "C" void solar_os_asm_set_args(const char *const *texts, int count)
{
    s_argc = 0;
    for (int i = 0; i < count && i < SOLAR_OS_ASM_ARGS_MAX; i++) {
        const char *t = texts[i] != NULL ? texts[i] : "";
        strlcpy(s_arg_text[i], t, sizeof(s_arg_text[i]));
        char *end = NULL;
        const long as_int = strtol(t, &end, 0);
        const bool is_int = t[0] != '\0' && end != NULL && *end == '\0';
        const float as_float = strtof(t, &end);
        const bool is_float = !is_int && t[0] != '\0' && end != NULL && *end == '\0';
        if (is_int) {
            s_arg_int[i] = (int32_t)as_int;
            s_arg_float[i] = (float)s_arg_int[i];
            s_arg_raw[i] = s_arg_int[i];
        } else if (is_float) {
            s_arg_float[i] = as_float;
            s_arg_int[i] = (int32_t)as_float;
            memcpy(&s_arg_raw[i], &s_arg_float[i], sizeof(int32_t));
        } else {
            /* text: its address, for a char * parameter */
            s_arg_int[i] = 0;
            s_arg_float[i] = 0.0f;
            s_arg_raw[i] = (int32_t)(uintptr_t)s_arg_text[i];
        }
        s_argc = i + 1;
    }
}

static char *host_argstr(int i)
{
    static char empty[1] = "";
    return i >= 0 && i < s_argc ? s_arg_text[i] : empty;
}

static int host_arg(int i)
{
    return i >= 0 && i < s_argc ? s_arg_int[i] : 0;
}

static float host_argf(int i)
{
    return i >= 0 && i < s_argc ? s_arg_float[i] : 0.0f;
}

/* ------------------------------------------------------------ text */

static solar_os_asm_text_fn s_text_out;
/* True once the script called updateDisplay()/setPixelsize(): it drives the frames,
 * so text is drawn into the back buffer like keyboard5inch's printscreen.
 * Before that, each print is shown at once (a plain text console). */
static volatile bool s_script_shows;

extern "C" void solar_os_asm_set_text_output(solar_os_asm_text_fn fn)
{
    s_text_out = fn;
}

static void text_emit(const char *text);

static void host_vprint(const char *format, va_list args, bool newline)
{
    /* Fixed buffer: a script task may be killed at any time, and must not be
     * holding the heap lock when it is. */
    char text[256];
    int n = vsnprintf(text, sizeof(text) - 1U, format, args);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(text) - 2U) {
        n = (int)(sizeof(text) - 2U);
    }
    if (newline) {
        text[n] = '\n';
        text[n + 1] = '\0';
    }
    text_emit(text);
}

static void host_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    host_vprint(format, args, false);
    va_end(args);
}

static void host_printfln(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    host_vprint(format, args, true);
    va_end(args);
}

/* tprintf / tprintln: always into the SolarOS terminal. */
static void host_term_vprint(const char *format, va_list args, bool newline)
{
    char text[256];
    int n = vsnprintf(text, sizeof(text) - 1U, format, args);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(text) - 2U) {
        n = (int)(sizeof(text) - 2U);
    }
    if (newline) {
        text[n] = '\n';
        text[n + 1] = '\0';
    }
    if (s_text_out != NULL) {
        s_text_out(text);
    } else {
        fputs(text, stdout);
    }
}

static void host_tprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    host_term_vprint(format, args, false);
    va_end(args);
}

static void host_tprintln(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    host_term_vprint(format, args, true);
    va_end(args);
}

extern "C" bool solar_os_asm_uses_screen(const uint8_t *data, size_t size)
{
    /* Whole-word names of the screen bindings (in source text, or in a saved
     * binary's relocation names: "@_ext_display", "@_updateDisplay(...)"). */
    static const char *const names[] = {
        "enableGraphicMode", "display", "updateDisplay", "setPixelsize",
        "setResolution", "setCursor", "setDoubleBuffer",
        "printscreen", "triangle", "pixel", "scroll", "clear", "zoom", "stride",
    };
    if (data == NULL) {
        return false;
    }
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        const size_t len = strlen(names[n]);
        for (size_t i = 0; i + len <= size; i++) {
            if (data[i] != (uint8_t)names[n][0] || memcmp(data + i, names[n], len) != 0) {
                continue;
            }
            bool before = i > 0 && (isalnum(data[i - 1]) || data[i - 1] == '_');
            /* A saved binary prefixes the names: "@_ext_" (variables) and
             * "@_" (functions). */
            if (before && i >= 6 && memcmp(data + i - 6, "@_ext_", 6) == 0) {
                before = false;
            } else if (before && i >= 2 && memcmp(data + i - 2, "@_", 2) == 0) {
                before = false;
            }
            const bool after = i + len < size && (isalnum(data[i + len]) || data[i + len] == '_');
            if (!before && !after) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------ screen */

#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
static rgb_lgfx_t *s_panel;
static uint32_t s_width, s_height, s_stride;
static int s_sizex, s_sizey;
static int s_zoom = 1;

static void sync_size(void)
{
    s_sizex = (int)s_width;
    s_sizey = (int)s_height;
}

static void host_update_display(void)
{
    s_script_shows = true;
    rgb_lgfx_surface_show(s_panel);
}

static void host_clear(void)
{
    rgb_lgfx_surface_clear(s_panel);
}

/* setCursor(x, y): the next printf/printscreen text starts there (logical
 * pixels), without clearing anything. */
static void host_set_cursor(int x, int y)
{
    rgb_lgfx_surface_set_text_cursor(s_panel, x, y);
}

/* setResolution(M400x240): the resolution objects. Each holds the logical
 * size and the two dividers of the 800 x 480 panel, M<width>x<height> with
 * width 800/divx and height 480/divy, divx and divy 1..5 (25 objects). */
SCRIPT_STRUCT(resolution, {
    int width;
    int height;
    int divx;
    int divy;
})

static resolution s_resolutions[RGB_LGFX_MAX_DIVIDER * RGB_LGFX_MAX_DIVIDER];
static char s_resolution_names[RGB_LGFX_MAX_DIVIDER * RGB_LGFX_MAX_DIVIDER][12];

/* The script passes the object by value, which reaches a bound function as
 * its address. */
/* setDoubleBuffer(0): draw straight on screen, updateDisplay() swaps
 * nothing; setDoubleBuffer(1): the hidden buffer again (the default). */
static void host_set_double_buffer(int on)
{
    rgb_lgfx_surface_set_double_buffer(s_panel, on != 0);
}

static void host_set_resolution(const resolution *r)
{
    if (r == NULL) {
        return;
    }
    s_script_shows = true;
    rgb_lgfx_surface_set_resolution(s_panel, r->divx, r->divy, &s_width, &s_height);
    sync_size();
}

static void host_set_pixel_size(int size)
{
    s_script_shows = true;
    rgb_lgfx_surface_set_pixel_size(s_panel, size, &s_width, &s_height);
    sync_size();
}

static void host_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    rgb_lgfx_surface_fill_triangle(s_panel, x0, y0, x1, y1, x2, y2, color);
}

/* keyboard5inch's pixel(x, y, hue): a zoom x zoom block, hue 0 = black. */
static void host_pixel(int x, int y, int hue)
{
    uint16_t **slot = rgb_lgfx_surface_back_buffer_slot(s_panel);
    if (slot == NULL || *slot == NULL) {
        return;
    }
    const int zoom = s_zoom > 0 ? s_zoom : 1;
    const uint16_t c = hue == 0 ? 0 : host_hsv2(hue, 255, 255);
    for (int j = 0; j < zoom; j++) {
        const int py = y * zoom + j;
        if (py < 0 || py >= (int)s_height) continue;
        for (int i = 0; i < zoom; i++) {
            const int px = x * zoom + i;
            if (px < 0 || px >= (int)s_width) continue;
            (*slot)[(size_t)py * s_stride + (size_t)px] = c;
        }
    }
}

static void host_printscreen(const char *format, ...)
{
    /* Fixed buffer: a script task may be killed at any time, and must not be
     * holding the heap lock when it is. */
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    rgb_lgfx_surface_print_ansi(s_panel, text);
}
#endif

/* enableGraphicMode(): the script takes the whole screen now (if it does not
 * have it yet). The terminal stops drawing until the script ends, and
 * printf & co. go to the screen; tprintf/tprintln still go to the terminal
 * transcript. Does nothing on a board without a frame-buffer panel. */
static void host_enable_graphic_mode(void)
{
#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    if (s_panel != NULL && !rgb_lgfx_surface_acquired(s_panel)) {
        (void)solar_os_asm_screen_begin();
    }
#endif
}

static void text_emit(const char *text)
{
#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    if (s_panel != NULL && rgb_lgfx_surface_acquired(s_panel)) {
        if (s_script_shows) {
            rgb_lgfx_surface_print_ansi(s_panel, text);
        } else {
            rgb_lgfx_surface_print_ansi_now(s_panel, text);
        }
        return;
    }
#endif
    if (s_text_out != NULL) {
        s_text_out(text);
    } else {
        fputs(text, stdout);
    }
}

extern "C" bool solar_os_asm_file_uses_screen(FILE *file)
{
    /* Windows of 512 bytes, each starting 480 after the last: a name (17
     * bytes at most) and what comes before it are whole in one of them. */
    uint8_t window[512];
    size_t kept = 0;
    while (true) {
        const size_t got = fread(window + kept, 1, sizeof(window) - kept, file);
        if (got == 0) {
            return false;
        }
        const size_t size = kept + got;
        if (solar_os_asm_uses_screen(window, size)) {
            return true;
        }
        kept = size > 32U ? 32U : size;
        memmove(window, window + size - kept, kept);
    }
}

extern "C" bool solar_os_asm_runtime_has_screen(void)
{
#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    return rgb_lgfx_primary() != NULL;
#else
    return false;
#endif
}

extern "C" esp_err_t solar_os_asm_screen_begin(void)
{
#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    if (s_panel == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = rgb_lgfx_surface_acquire(s_panel);
    if (err == ESP_OK) {
        s_width = rgb_lgfx_width(s_panel);
        s_height = rgb_lgfx_height(s_panel);
        s_stride = rgb_lgfx_surface_stride(s_panel);
        s_zoom = 1;
        s_script_shows = false;
        sync_size();
        rgb_lgfx_surface_clear(s_panel);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" void solar_os_asm_screen_end(void)
{
#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    /* Also after enableGraphicMode(); nothing to do when not acquired. */
    if (s_panel != NULL && rgb_lgfx_surface_acquired(s_panel)) {
        rgb_lgfx_surface_release(s_panel);
    }
#endif
}

/* ------------------------------------------------------------ code memory */

/* A script's machine code needs one contiguous block of executable internal
 * RAM. Once SolarOS has run a while, no free piece is big enough even for
 * Valmora's 23.3 KB, so a block is set aside at boot and the loader gets it
 * through setExecutableAllocator(). From PSRAM the code runs about as fast,
 * but a script that redraws the whole 800x480 panel (Valmora) then shares
 * the PSRAM bus with the panel's refresh, and the picture shakes. A larger
 * script (Ironforge, 40.5 KB) goes to the heap, then to PSRAM. */
#ifndef SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES
#define SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES (26U * 1024U)
#endif

static void *s_exec_block;
static bool s_exec_block_used;
static portMUX_TYPE s_exec_lock = portMUX_INITIALIZER_UNLOCKED;

extern "C" void solar_os_asm_reserve_executable(void)
{
    if (s_exec_block != NULL) {
        return;
    }
    s_exec_block = heap_caps_malloc(SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES, MALLOC_CAP_EXEC);
    if (s_exec_block == NULL) {
        ESP_LOGW("asm", "no %u bytes of executable memory to set aside for scripts",
                 (unsigned)SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES);
    }
}

static void *exec_alloc(size_t size)
{
    bool mine = false;
    portENTER_CRITICAL(&s_exec_lock);
    if (s_exec_block != NULL && !s_exec_block_used && size <= SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES) {
        s_exec_block_used = true;
        mine = true;
    }
    portEXIT_CRITICAL(&s_exec_lock);
    return mine ? s_exec_block : heap_caps_malloc(size, MALLOC_CAP_EXEC);
}

static void exec_free(void *code)
{
    if (code != NULL && code == s_exec_block) {
        portENTER_CRITICAL(&s_exec_lock);
        s_exec_block_used = false;
        portEXIT_CRITICAL(&s_exec_lock);
    } else {
        heap_caps_free(code);
    }
}

extern "C" void solar_os_asm_set_compile_progress(solar_os_asm_progress_fn fn, void *ctx)
{
    setCompileProgress(fn, ctx);
}

extern "C" bool solar_os_asm_set_code_memory(solar_os_asm_code_memory_t where)
{
    return setScriptCodeMemory(where == SOLAR_OS_ASM_CODE_PSRAM  ? SCRIPT_CODE_PSRAM
                               : where == SOLAR_OS_ASM_CODE_AUTO ? SCRIPT_CODE_AUTO
                                                                 : SCRIPT_CODE_IRAM);
}

/* ------------------------------------------------------------ bindings */

/* Every name a script can use without declaring it, for the editor's
 * colouring and completion: the bindings below, as they are made, and the
 * language's own built-ins. Sorted once binding is done. */
#define KNOWN_MAX 160
static solar_os_asm_known_t s_known[KNOWN_MAX];
static size_t s_known_count;

static void know(const char *name, const char *type, const char *params, bool function)
{
    if (s_known_count < KNOWN_MAX) {
        s_known[s_known_count++] = (solar_os_asm_known_t){name, type, params, function};
    }
}

static int known_order(const void *a, const void *b)
{
    return strcmp(((const solar_os_asm_known_t *)a)->name, ((const solar_os_asm_known_t *)b)->name);
}

#define BIND_F(out, name, in, fn) \
    (bindFunction((char *)(out), (char *)(name), (char *)(in), (void *)(fn)), \
     know((name), (out), (in), true))
#define BIND_V(type, name, ptr) \
    (bindVariable((char *)(type), (char *)(name), NULL, (void *)(ptr)), \
     know((name), (type), NULL, false))

extern "C" void solar_os_asm_runtime_init(void)
{
    static bool bound;
    if (bound) {
        return;
    }
    bound = true;
    setExecutableAllocator(exec_alloc, exec_free);

    BIND_F("CRGB", "hsv", "int,int,int", host_hsv);
    BIND_F("uint16_t", "hsv2", "int,int,int", host_hsv2);
    BIND_F("uint32_t", "millis", NULL, host_millis);
    BIND_F("void", "delay", "uint32_t", host_delay);
    BIND_F("uint32_t", "rand", "uint32_t", host_rand);
    BIND_F("float", "sin", "float", host_sin);
    BIND_F("float", "atan2", "float,float", host_atan2);
    BIND_F("float", "hypot", "float,float", host_hypot);
    BIND_F("float", "log", "float", host_log);
    BIND_F("uint8_t", "sin8", "uint8_t", host_sin8);
    BIND_F("void", "resetStat", NULL, host_reset_stat);

    /* Before any compile: asmparser2 keeps a host binding of these names
     * instead of its built-in serial printf/printfln. */
    BIND_F("void", "printf", "char*,Args", host_printf);
    BIND_F("void", "printfln", "char*,Args", host_printfln);
    BIND_F("void", "println", "char*,Args", host_printfln);
    BIND_F("void", "tprintf", "char*,Args", host_tprintf);
    BIND_F("void", "tprintln", "char*,Args", host_tprintln);
    BIND_F("void", "enableGraphicMode", NULL, host_enable_graphic_mode);

    BIND_V("int", "argc", &s_argc);
    BIND_F("int", "arg", "int", host_arg);
    BIND_F("float", "argf", "int", host_argf);
    BIND_F("char *", "argstr", "int", host_argstr);

    BIND_F("int", "readKey", NULL, host_read_key);
    BIND_F("void", "setup_keyboard", "uint32_t", host_setup_keyboard);
    BIND_V("uint8_t", "keyChar", &s_key.key_char);
    BIND_V("uint8_t", "keyCode", &s_key.key_code);
    /* The keyboard5inch scripts declare `struct return_key { char key; char val; }`. */
    BIND_V("return_key", "key", &s_key);
    BIND_F("int", "keyHeld", "int", host_key_held);

    BIND_V("uint8_t *", "wad", &s_wad);
    BIND_V("int", "wadLoaded", &s_wad_loaded);
    BIND_F("int", "wadFind", "char*", host_wad_find);
    BIND_F("int", "wadSprite", "char*,int,int", host_wad_sprite);

#if SOLAR_OS_PACKAGE_DRIVER_DISPLAY_RGB_LGFX
    s_panel = rgb_lgfx_primary();
    if (s_panel != NULL) {
        s_width = rgb_lgfx_width(s_panel);
        s_height = rgb_lgfx_height(s_panel);
        s_stride = rgb_lgfx_surface_stride(s_panel);
        sync_size();
        /* A pointer, not an array: updateDisplay() swaps the frame buffers and the
         * script reads display again at every access. */
        BIND_V("uint16_t *", "display", rgb_lgfx_surface_back_buffer_slot(s_panel));
        BIND_V("uint32_t", "width", &s_width);
        BIND_V("uint32_t", "height", &s_height);
        BIND_V("uint32_t", "stride", &s_stride);
        BIND_V("int", "sizex", &s_sizex);
        BIND_V("int", "sizey", &s_sizey);
        BIND_V("int", "zoom", &s_zoom);
        BIND_V("uint16_t", "scroll", rgb_lgfx_surface_scroll_slot(s_panel));
        BIND_F("void", "updateDisplay", NULL, host_update_display);
        BIND_F("void", "clear", NULL, host_clear);
        BIND_F("void", "setCursor", "int,int", host_set_cursor);
        BIND_F("void", "setPixelsize", "int", host_set_pixel_size);
        bindStruct(resolution_definition);
        for (int dy = 1; dy <= RGB_LGFX_MAX_DIVIDER; dy++) {
            for (int dx = 1; dx <= RGB_LGFX_MAX_DIVIDER; dx++) {
                const int i = (dy - 1) * RGB_LGFX_MAX_DIVIDER + (dx - 1);
                resolution *r = &s_resolutions[i];
                r->width = (int)(s_stride / (uint32_t)dx);
                r->height = (int)(rgb_lgfx_height(s_panel) / (uint32_t)dy);
                r->divx = dx;
                r->divy = dy;
                snprintf(s_resolution_names[i], sizeof(s_resolution_names[i]), "M%dx%d",
                         r->width, r->height);
                BIND_V("resolution", s_resolution_names[i], r);
            }
        }
        BIND_F("void", "setResolution", "resolution", host_set_resolution);
        BIND_F("void", "setDoubleBuffer", "int", host_set_double_buffer);
        BIND_F("void", "pixel", "int,int,int", host_pixel);
        BIND_F("void", "triangle", "int,int,int,int,int,int,uint16_t", host_triangle);
        BIND_F("void", "printscreen", "char*,Args", host_printscreen);
    }
#endif
    /* built into the language: every script has them, bound or not */
    know("printf", "void", "char*,Args", true);
    know("printfln", "void", "char*,Args", true);
    know("alloc", "uint32_t", "int", true);
    know("free", "void", "uint32_t", true);
    know("writeInt", "void", "uint32_t,int,int", true);
    know("readInt", "int", "uint32_t,int", true);
    qsort(s_known, s_known_count, sizeof(s_known[0]), known_order);
}

extern "C" size_t solar_os_asm_known_names(const solar_os_asm_known_t **list)
{
    solar_os_asm_runtime_init();
    if (list != NULL) {
        *list = s_known;
    }
    return s_known_count;
}

extern "C" int solar_os_asm_known_kind(const char *word, size_t len)
{
    const solar_os_asm_known_t *list;
    size_t lo = 0;
    size_t hi = solar_os_asm_known_names(&list);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        int c = strncmp(list[mid].name, word, len);
        if (c == 0 && list[mid].name[len] != '\0') {
            c = 1;
        }
        if (c == 0) {
            return list[mid].function ? 1 : 2;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ compile/run */

static void copy_error(solar_os_asm_error_t *error, const char *fallback)
{
    if (error == NULL) {
        return;
    }
    const CompileError &e = lastCompileError();
    memset(error, 0, sizeof(*error));
    error->failed = true;
    if (e.failed) {
        error->line = e.line;
        error->position = e.position;
        strlcpy(error->message, e.message, sizeof(error->message));
        strlcpy(error->source_line, e.sourceLine, sizeof(error->source_line));
    } else {
        strlcpy(error->message, fallback, sizeof(error->message));
    }
}

extern "C" esp_err_t solar_os_asm_compile_binary(const char *source, uint8_t **out,
                                                 uint32_t *size,
                                                 solar_os_asm_error_t *error)
{
    if (source == NULL || out == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_asm_runtime_init();
    *out = parseScriptToBinary(source, size);
    if (*out == NULL) {
        copy_error(error, "compile failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t solar_os_asm_load_source(const char *source,
                                              solar_os_asm_program_t **out,
                                              solar_os_asm_error_t *error)
{
    if (source == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_asm_runtime_init();
    solar_os_asm_program_t *program =
        new (std::nothrow) solar_os_asm_program_t{parseScript(source)};
    if (program == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!program->exe.isExeExists()) {
        copy_error(error, "compiled, but loading failed: no executable memory or "
                          "unresolved binding (details on the serial console)");
        delete program;
        return ESP_FAIL;
    }
    *out = program;
    return ESP_OK;
}

static size_t file_reader(void *ctx, uint8_t *buf, size_t len)
{
    return fread(buf, 1, len, (FILE *)ctx);
}

extern "C" esp_err_t solar_os_asm_load_binary_file(FILE *file,
                                                   solar_os_asm_program_t **out,
                                                   solar_os_asm_error_t *error)
{
    if (file == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_asm_runtime_init();
    solar_os_asm_program_t *program =
        new (std::nothrow) solar_os_asm_program_t{createExecutableFromStream(file_reader, file)};
    if (program == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!program->exe.isExeExists()) {
        if (error != NULL) {
            memset(error, 0, sizeof(*error));
            error->failed = true;
            /* asmparser2 says why: "missing binding: led16, show16()" (every
             * name this firmware does not bind), "not a valid binary: ...",
             * or no executable memory. */
            const char *why = lastLoadError();
            strlcpy(error->message, why[0] != '\0' ? why : "load failed",
                    sizeof(error->message));
        }
        delete program;
        return ESP_FAIL;
    }
    *out = program;
    return ESP_OK;
}

extern "C" uint32_t solar_os_asm_code_size(const solar_os_asm_program_t *program)
{
    return program != NULL ? program->exe.exe.binary_size : 0;
}

extern "C" bool solar_os_asm_code_in_psram(const solar_os_asm_program_t *program)
{
    return program != NULL && executableCodeMemory(&program->exe.exe) == SCRIPT_CODE_PSRAM;
}

/* The script runs on its own task, with its stack in PSRAM: internal RAM is
 * what SolarOS runs short of (the code itself stays in the executable block
 * set aside at boot), and a script never reaches the flash -- no binding
 * writes a file -- which a task with a PSRAM stack must not. ESP-IDF's
 * PSRAM stacks are cached, so deep recursion (fib) stays about as fast.
 * Like ScriptExecutable::executeAsTask()'s task, it does not delete itself
 * once the script returns: solar_os_asm_stop() does. */
static void script_task(void *param)
{
    solar_os_asm_program_t *program = static_cast<solar_os_asm_program_t *>(param);
    if (s_argc > 0) {
        program->exe.execute(program->function, s_arg_raw, s_argc);
    } else {
        program->exe.execute(program->function);
    }
    program->finished = true;
    while (true) {
        vTaskSuspend(NULL);
    }
}

extern "C" esp_err_t solar_os_asm_start(solar_os_asm_program_t *program,
                                        const char *function, uint32_t stack_bytes)
{
    if (program == NULL || function == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_asm_stop(program);
    program->function = function;
    program->finished = false;
    if (solar_os_task_create_pinned_external(script_task, "asm_script", stack_bytes, program, 1,
                                             &program->task, 1,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        program->task = NULL;
        program->finished = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" bool solar_os_asm_finished(const solar_os_asm_program_t *program)
{
    return program == NULL || program->task == NULL || program->finished;
}

extern "C" void solar_os_asm_stop(solar_os_asm_program_t *program)
{
    if (program == NULL) {
        return;
    }
    if (s_keyboard_target == &program->exe) {
        s_keyboard_target = NULL;
    }
    TaskHandle_t task = program->task;
    program->task = NULL;
    if (task != NULL) {
        /* As ScriptExecutable::kill(): the unwind point for a safe_mode
         * error was on the task's stack, which is going away. */
        program->exe.exe.runtimeUnwind = NULL;
        solar_os_task_delete_external(task);
    }
    program->finished = true;
}

extern "C" bool solar_os_asm_runtime_error(solar_os_asm_program_t *program,
                                           char *buffer, size_t buffer_len)
{
    if (program == NULL || !program->exe.hasRuntimeError()) {
        return false;
    }
    const runtime_error_struct &e = program->exe.runtimeError();
    snprintf(buffer, buffer_len, "array bounds: %s[%d] (size %u)",
             e.array, (int)e.index, (unsigned)e.size);
    return true;
}

extern "C" void solar_os_asm_free(solar_os_asm_program_t *program)
{
    if (program == NULL) {
        return;
    }
    solar_os_asm_stop(program);
    delete program;
}
