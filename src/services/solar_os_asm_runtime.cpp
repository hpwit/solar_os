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

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_config.h"

#include "binding.h"
#include "script_executable.h"


struct solar_os_asm_program {
    ScriptExecutable exe;
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

/* RGB565, byte-swapped like a frame buffer (display). */
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

/* ------------------------------------------------------------ output */

/* enableGraphicMode(): on a board with a frame-buffer panel the script takes
 * the whole screen (see solar_os_asm_screen_begin()). This build has none, so
 * it does nothing: scripts that call it still compile and run, printing into
 * the terminal. */
static void host_enable_graphic_mode(void)
{
    (void)solar_os_asm_screen_begin();
}

static void text_emit(const char *text)
{
    if (s_text_out != NULL) {
        s_text_out(text);
    } else {
        fputs(text, stdout);
    }
}

extern "C" bool solar_os_asm_runtime_has_screen(void)
{
    return false;
}

extern "C" esp_err_t solar_os_asm_screen_begin(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" void solar_os_asm_screen_end(void)
{
}

/* ------------------------------------------------------------ bindings */

#define BIND_F(out, name, in, fn) \
    bindFunction((char *)(out), (char *)(name), (char *)(in), (void *)(fn))
#define BIND_V(type, name, ptr) \
    bindVariable((char *)(type), (char *)(name), NULL, (void *)(ptr))

extern "C" void solar_os_asm_runtime_init(void)
{
    static bool bound;
    if (bound) {
        return;
    }
    bound = true;

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

extern "C" esp_err_t solar_os_asm_load_binary(const uint8_t *data, uint32_t size,
                                              solar_os_asm_program_t **out,
                                              solar_os_asm_error_t *error)
{
    if (data == NULL || size == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_asm_runtime_init();
    solar_os_asm_program_t *program =
        new (std::nothrow) solar_os_asm_program_t{createExecutableFromBuffer(data, size)};
    if (program == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!program->exe.isExeExists()) {
        if (error != NULL) {
            memset(error, 0, sizeof(*error));
            error->failed = true;
            /* asmparser2 says why: "missing binding: name, function()" (every
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

extern "C" esp_err_t solar_os_asm_start(solar_os_asm_program_t *program,
                                        const char *function, uint32_t stack_bytes)
{
    if (program == NULL || function == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return program->exe.executeAsTask(function, s_argc > 0 ? s_arg_raw : NULL, s_argc,
                                      stack_bytes, 1, 1, "asm_script") ?
        ESP_OK : ESP_FAIL;
}

extern "C" bool solar_os_asm_finished(const solar_os_asm_program_t *program)
{
    return program == NULL || program->exe.state() == ScriptState::stopped ||
        program->exe.state() == ScriptState::notStarted;
}

extern "C" void solar_os_asm_stop(solar_os_asm_program_t *program)
{
    if (program == NULL) {
        return;
    }
    if (s_keyboard_target == &program->exe) {
        s_keyboard_target = NULL;
    }
    (void)program->exe.kill();
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
