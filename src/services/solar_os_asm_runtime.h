#pragma once

/*
 * C interface to ESPLiveScript 2 (components/asmparser2): compile a C-like
 * script to Xtensa machine code on the device, save/load the compiled binary,
 * and run it on its own task.
 *
 * Scripts get these host bindings:
 *   keyboard readKey(), keyChar, keyCode, key, setup_keyboard(_handle_)
 *            (then the script's keyboard() runs on every key press)
 *   text     printf(), printfln(), println(), tprintf(), tprintln() print in
 *            the SolarOS terminal, like a DOS program or the serial port
 *   params   argc, arg(i) (int), argf(i) (float), argstr(i) (char *):
 *            asm run file.sc 40 1.5 Yves
 *   misc     millis(), delay(ms), rand(n), sin(), atan2(), hypot(), log(),
 *            sin8(), hsv(), hsv2(), resetStat()
 * The screen hooks below are an extension point for a board driver that
 * gives a script its frame buffer; this build has none, so they report no
 * screen and every script prints into the terminal.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
bool solar_os_asm_runtime_has_screen(void);

/* Compile to a relocatable binary. *out is malloc'd; free() it. */
esp_err_t solar_os_asm_compile_binary(const char *source,
                                      uint8_t **out,
                                      uint32_t *size,
                                      solar_os_asm_error_t *error);

esp_err_t solar_os_asm_load_source(const char *source,
                                   solar_os_asm_program_t **out,
                                   solar_os_asm_error_t *error);
esp_err_t solar_os_asm_load_binary(const uint8_t *data,
                                   uint32_t size,
                                   solar_os_asm_program_t **out,
                                   solar_os_asm_error_t *error);
uint32_t solar_os_asm_code_size(const solar_os_asm_program_t *program);

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

/* Full-screen frame-buffer mode for a running script: ESP_ERR_NOT_SUPPORTED
 * in this build (no frame-buffer driver). */
esp_err_t solar_os_asm_screen_begin(void);
void solar_os_asm_screen_end(void);

/* Keyboard: the app forwards key presses/releases; the worker delivers them
 * to the script's keyboard() callback when it registered one. */
void solar_os_asm_key_event(uint8_t character, uint8_t hid_usage, bool pressed);
void solar_os_asm_keyboard_reset(void);
void solar_os_asm_keyboard_pump(void);

#ifdef __cplusplus
}
#endif
