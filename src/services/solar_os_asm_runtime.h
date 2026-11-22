#pragma once

/*
 * C interface to ESPLiveScript 2 (components/asmparser2): compile a C-like
 * script to Xtensa machine code on the device, save/load the compiled binary,
 * and run it on its own task.
 *
 * Scripts get the host bindings of the MaTouch / keyboard5inch sketches:
 *   screen   display, updateDisplay(), setPixelsize(n), clear(), scroll,
 *            width/height, sizex/sizey, stride, zoom, pixel(x,y,hue),
 *            triangle(...), printscreen(fmt,...), hsv(), hsv2(), resetStat()
 *   keyboard readKey(), keyChar, keyCode, key, setup_keyboard(_handle_)
 *            (then the script's keyboard() runs on every key press)
 *   text     printf(), printfln(), println() print on the screen (ANSI colours,
 *            as keyboard5inch's printscreen) when the script owns it, in the
 *            terminal otherwise; tprintf(), tprintln() always print in the
 *            SolarOS terminal, like a DOS program or the serial port
 *   params   argc, arg(i) (int), argf(i) (float), argstr(i) (char *):
 *            asm run file.sc 40 1.5 Yves
 *   misc     millis(), delay(ms), rand(n), sin(), atan2(), hypot(), log(),
 *            sin8()
 * The screen bindings exist only when the board has the rgb-lgfx panel.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool failed;
    int line;
    int position;
    char message[240];
    char source_line[120];
} solar_os_asm_error_t;

typedef struct solar_os_asm_program solar_os_asm_program_t;

/* Registers the host bindings once; safe to call repeatedly. */
void solar_os_asm_runtime_init(void);
/* Every name a script can use without declaring it: the bindings of this
 * firmware and the language's built-ins (printf, alloc...), sorted by name,
 * for the editor's colouring and completion. type: the variable's type or
 * the function's result; params: a function's parameter types ("int,int",
 * NULL for none). */
typedef struct {
    const char *name;
    const char *type;
    const char *params;
    bool function;
} solar_os_asm_known_t;
size_t solar_os_asm_known_names(const solar_os_asm_known_t **list);
/* 0: not a known name, 1: a function, 2: a variable */
int solar_os_asm_known_kind(const char *word, size_t len);
/* At boot, before the internal RAM is broken up: sets aside
 * SOLAR_OS_ASM_EXECUTABLE_RESERVE_BYTES of executable memory for scripts'
 * code (one script at a time; a larger or a second one uses the heap, then
 * PSRAM). */
void solar_os_asm_reserve_executable(void);
/* Where the next scripts' code goes: executable internal RAM (the
 * default), PSRAM run through its instruction-bus alias, or AUTO: internal
 * RAM when a block is free, else PSRAM. PSRAM and AUTO need an ESP32-S3
 * with PSRAM: false is returned elsewhere, and nothing changes. */
typedef enum {
    SOLAR_OS_ASM_CODE_IRAM,
    SOLAR_OS_ASM_CODE_PSRAM,
    SOLAR_OS_ASM_CODE_AUTO,
} solar_os_asm_code_memory_t;
bool solar_os_asm_set_code_memory(solar_os_asm_code_memory_t where);
/* How far a compile has got (asmparser2's setCompileProgress()): while
 * solar_os_asm_load_source() or solar_os_asm_compile_binary() compiles, fn
 * is called with a percentage (0 to 100, never going back) and the stage
 * ("reading", "parsing", "generating", "optimizing", "assembling", "done"),
 * at most once per percent, on the compiling task. NULL: no calls. */
typedef void (*solar_os_asm_progress_fn)(int percent, const char *stage, void *ctx);
void solar_os_asm_set_compile_progress(solar_os_asm_progress_fn fn, void *ctx);
bool solar_os_asm_runtime_has_screen(void);

/* Compile to a relocatable binary. *out is malloc'd; free() it. */
esp_err_t solar_os_asm_compile_binary(const char *source,
                                      uint8_t **out,
                                      uint32_t *size,
                                      solar_os_asm_error_t *error);

esp_err_t solar_os_asm_load_source(const char *source,
                                   solar_os_asm_program_t **out,
                                   solar_os_asm_error_t *error);
/* A saved binary, read from `file` as it loads: the code goes from the
 * file straight into its executable block (createExecutableFromStream()),
 * the file is never held whole. */
esp_err_t solar_os_asm_load_binary_file(FILE *file,
                                        solar_os_asm_program_t **out,
                                        solar_os_asm_error_t *error);
uint32_t solar_os_asm_code_size(const solar_os_asm_program_t *program);
/* Whether the loaded script's code went to PSRAM. */
bool solar_os_asm_code_in_psram(const solar_os_asm_program_t *program);

/* Script parameters (asm run file.sc 40 1.5 Yves): up to 6. A whole
 * integer (decimal or 0x) is an int, another number a float, anything else
 * a string passed by address (char *). They are passed to `function`
 * (int main(int n) gets 40) and read by the argc / arg(i) / argf(i) /
 * argstr(i) bindings. */
#define SOLAR_OS_ASM_ARGS_MAX 6
void solar_os_asm_set_args(const char *const *texts, int count);

/* Runs `function` on its own task (core 1, away from the panel ISRs),
 * with the parameters set by solar_os_asm_set_args(). */
esp_err_t solar_os_asm_start(solar_os_asm_program_t *program,
                             const char *function,
                             uint32_t stack_bytes);
bool solar_os_asm_finished(const solar_os_asm_program_t *program);
/* Kills the script task if it is still running. */
void solar_os_asm_stop(solar_os_asm_program_t *program);
bool solar_os_asm_runtime_error(solar_os_asm_program_t *program,
                                char *buffer,
                                size_t buffer_len);
void solar_os_asm_free(solar_os_asm_program_t *program);

/* The terminal output: tprintf/tprintln always, printf/printfln/println when
 * the script does not own the screen. One call per printed piece of text
 * (newlines included, as written by the script). */
typedef void (*solar_os_asm_text_fn)(const char *text);
void solar_os_asm_set_text_output(solar_os_asm_text_fn fn);

/* True when a script (source, or a saved binary's binding names) uses the
 * screen bindings; a text-only script leaves the terminal in place. */
bool solar_os_asm_uses_screen(const uint8_t *data, size_t size);
/* The same for a file, read a piece at a time from where it is; it is left
 * at its end. */
bool solar_os_asm_file_uses_screen(FILE *file);

/* Full-screen frame-buffer mode for a running script (rgb-lgfx boards). */
esp_err_t solar_os_asm_screen_begin(void);
void solar_os_asm_screen_end(void);

/* Keyboard: the app forwards key presses/releases; the worker delivers them
 * to the script's keyboard() callback when it registered one. */
void solar_os_asm_key_event(uint8_t character, uint8_t hid_usage, bool pressed);
void solar_os_asm_keyboard_reset(void);
void solar_os_asm_keyboard_pump(void);

/* The WAD for wad[], wadLoaded, wadFind() and wadSprite(), or NULL; the
 * caller keeps it until the script has stopped. */
void solar_os_asm_set_wad(uint8_t *data);

#ifdef __cplusplus
}
#endif
