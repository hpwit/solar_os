/*
 * asm: ESPLiveScript 2 (asmparser2) from the shell.
 *
 *   asm run [-p|-i] [-f function] <file.sc|file.bin> [params...]
 *            (the script's code in internal RAM, or PSRAM when no block is
 *            free; -p: always PSRAM, -i: always internal RAM)
 *                                           compile or load, then run; up to
 *                                           6 int/float params go to the
 *                                           function and to argc/arg()/argf()
 *   asm compile <file.sc> [-o <file.bin>]   compile; with -o also save
 *   asm save <file.sc> [<file.bin>]         compile and save the binary
 *
 * The compiler and the script run on a worker task; the app task only
 * forwards keys and prints the worker's messages. On boards with the
 * rgb-lgfx panel a running script owns the whole frame buffer (display,
 * updateDisplay(), ...) until it returns or is stopped with the app exit key or
 * Ctrl-C.
 */
#include "solar_os_asm_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_asm_runtime.h"
#include "solar_os_key_codes.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define TAG "solar_os_asm"
#define ASM_TASK_STACK (24U * 1024U)
#define ASM_SCRIPT_STACK (16U * 1024U)
#define ASM_TASK_PRIORITY 4
#define ASM_MESSAGE_MAX 200
#define ASM_MESSAGE_QUEUE_LEN 48
#define ASM_SOURCE_MAX (512U * 1024U)
#define ASM_CTRL_C 0x03

typedef enum {
    ASM_MODE_RUN,
    ASM_MODE_COMPILE,
    ASM_MODE_SAVE,
} asm_mode_t;

typedef struct {
    bool done;
    bool raw; /* script text: written as is (newlines included) */
    char text[ASM_MESSAGE_MAX];
} asm_message_t;

typedef struct {
    asm_mode_t mode;
    char in_path[SOLAR_OS_STORAGE_PATH_MAX];
    char out_path[SOLAR_OS_STORAGE_PATH_MAX];
    char function[48];
    solar_os_asm_code_memory_t code_memory; /* asm run -p, -i */
    QueueHandle_t messages;
    TaskHandle_t task;
    volatile bool task_done;
    volatile bool stop_requested;
    bool success;
    bool finished;
} asm_state_t;

static void *asm_state_slot;
#define S (*(asm_state_t *)asm_state_slot)

static solar_os_shell_io_t *asm_io(solar_os_context_t *ctx)
{
    return solar_os_context_shell_io(ctx);
}

static void asm_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "asm run [-p|-i] [-f function] <file.sc|file.bin> [params...]   compile or load, then run (-p: the code in PSRAM, -i: in internal RAM)");
    solar_os_shell_io_writeln(io, "        params (up to 6: int, float or text) go to main() and to argc/arg(i)/argf(i)/argstr(i)");
    solar_os_shell_io_writeln(io, "asm compile <file.sc> [-o <file.bin>]   compile; with -o also save the binary");
    solar_os_shell_io_writeln(io, "asm save <file.sc> [<file.bin>]         compile and save (default: same name .bin)");
    solar_os_shell_io_writeln(io, "A running script stops on the app exit key or Ctrl-C.");
}

/* ---------------------------------------------------------------- worker */

static void post(bool done, const char *fmt, ...)
{
    asm_message_t message = {.done = done};
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(message.text, sizeof(message.text), fmt, args);
        va_end(args);
    }
    (void)xQueueSend(S.messages, &message, pdMS_TO_TICKS(1000));
}

/* The compile's progress on one terminal line, rewritten in place:
 * "compiling game.sc...  48% optimizing". */
static void compile_progress(int percent, const char *stage, void *ctx)
{
    (void)ctx;
    asm_message_t message = {.raw = true};
    snprintf(message.text, sizeof(message.text), "\rcompiling %s... %3d%% %-10s", S.in_path, percent, stage);
    (void)xQueueSend(S.messages, &message, pdMS_TO_TICKS(100));
}

static void compile_progress_begin(void)
{
    compile_progress(0, "reading", NULL);
    solar_os_asm_set_compile_progress(compile_progress, NULL);
}

/* the progress line is left as it ended; the next messages start below */
static void compile_progress_end(void)
{
    solar_os_asm_set_compile_progress(NULL, NULL);
    asm_message_t message = {.raw = true};
    strlcpy(message.text, "\n", sizeof(message.text));
    (void)xQueueSend(S.messages, &message, pdMS_TO_TICKS(1000));
}

static void post_compile_error(const solar_os_asm_error_t *error)
{
    if (error->line > 0) {
        post(false, "asm: line %d: %s", error->line, error->message);
        if (error->source_line[0] != '\0') {
            post(false, "  %s", error->source_line);
            if (error->position > 0 && error->position < 100) {
                char marker[104];
                memset(marker, ' ', (size_t)error->position - 1U);
                marker[error->position - 1] = '^';
                marker[error->position] = '\0';
                post(false, "  %s", marker);
            }
        }
    } else {
        post(false, "asm: %s", error->message[0] ? error->message : "compile failed");
    }
}

static uint8_t *read_file(const char *path, size_t *size, bool text)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        post(false, "asm: cannot open %s", path);
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > ASM_SOURCE_MAX) {
        post(false, "asm: %s: empty or larger than %u bytes", path, (unsigned)ASM_SOURCE_MAX);
        fclose(f);
        return NULL;
    }
    uint8_t *data = solar_os_memory_alloc((size_t)st.st_size + 1U,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "asm.file");
    if (data == NULL) {
        post(false, "asm: out of memory");
        fclose(f);
        return NULL;
    }
    const size_t got = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        post(false, "asm: short read on %s", path);
        solar_os_memory_free(data);
        return NULL;
    }
    if (text) {
        data[got] = '\0';
    }
    *size = got;
    return data;
}

static bool has_suffix(const char *path, const char *suffix)
{
    const size_t n = strlen(path), m = strlen(suffix);
    return n >= m && strcasecmp(path + n - m, suffix) == 0;
}

static bool worker_compile(void)
{
    size_t size = 0;
    char *source = (char *)read_file(S.in_path, &size, true);
    if (source == NULL) {
        return false;
    }
    solar_os_asm_error_t error = {0};
    uint8_t *binary = NULL;
    uint32_t binary_size = 0;
    compile_progress_begin();
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = solar_os_asm_compile_binary(source, &binary, &binary_size, &error);
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
    compile_progress_end();
    solar_os_memory_free(source);
    if (err != ESP_OK) {
        post_compile_error(&error);
        return false;
    }
    post(false, "compile done in %lld ms (%u bytes binary)",
         (long long)elapsed_ms, (unsigned)binary_size);
    bool ok = true;
    if (S.out_path[0] != '\0') {
        FILE *f = fopen(S.out_path, "wb");
        const size_t written = f != NULL ? fwrite(binary, 1, binary_size, f) : 0;
        if (f != NULL && fclose(f) != 0) {
            ok = false;
        }
        if (f == NULL || written != binary_size || !ok) {
            post(false, "asm: cannot write %s", S.out_path);
            ok = false;
        } else {
            post(false, "asm: saved %s", S.out_path);
        }
    }
    free(binary); /* allocated by the compiler with malloc() */
    return ok;
}

/* Script text for the terminal (tprintf/tprintln, and printf & co. when the
 * script does not own the screen): passed on as written, like a serial
 * console, so printf without '\n' continues the line. */
static void asm_text_out(const char *text)
{
    asm_message_t message = {.raw = true};
    strlcpy(message.text, text, sizeof(message.text));
    (void)xQueueSend(S.messages, &message, pdMS_TO_TICKS(1000));
}

/* game.sc or game.bin -> game.wad next to it, read for wad[] & co. (see
 * solar_os_asm_set_wad()); NULL when there is none. */
static uint8_t *read_wad(void)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(path, S.in_path, sizeof(path));
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot == NULL || (slash != NULL && dot < slash) ||
        (size_t)(dot - path) + 5U > sizeof(path)) {
        return NULL;
    }
    strcpy(dot, ".wad");
    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }
    size_t size = 0;
    uint8_t *wad = read_file(path, &size, false);
    if (wad != NULL && (size < 12U || memcmp(wad + 1, "WAD", 3) != 0)) {
        post(false, "asm: %s is not a WAD", path);
        solar_os_memory_free(wad);
        return NULL;
    }
    if (wad != NULL) {
        post(false, "%s: %u bytes", path, (unsigned)size);
    }
    return wad;
}

static bool worker_run_loaded(void)
{
    const bool binary = has_suffix(S.in_path, ".bin");
    solar_os_asm_error_t error = {0};
    solar_os_asm_program_t *program = NULL;
    bool wants_screen;
    esp_err_t err;
    int64_t started;
    if (binary) {
        /* A saved binary is streamed from the flash or the SD card: its code
         * goes from the file into executable memory, the file is never held
         * whole. */
        FILE *f = fopen(S.in_path, "rb");
        if (f == NULL) {
            post(false, "asm: cannot open %s", S.in_path);
            return false;
        }
        /* A text-only script (no screen binding) leaves the terminal in
         * place and prints into it, like a DOS program. */
        wants_screen = solar_os_asm_file_uses_screen(f);
        rewind(f);
        post(false, "loading %s...", S.in_path);
        started = esp_timer_get_time();
        err = solar_os_asm_load_binary_file(f, &program, &error);
        fclose(f);
    } else {
        size_t size = 0;
        uint8_t *data = read_file(S.in_path, &size, true);
        if (data == NULL) {
            return false;
        }
        wants_screen = solar_os_asm_uses_screen(data, size);
        compile_progress_begin();
        started = esp_timer_get_time();
        err = solar_os_asm_load_source((const char *)data, &program, &error);
        compile_progress_end();
        solar_os_memory_free(data);
    }
    if (err != ESP_OK) {
        if (binary) {
            post(false, "asm: %s: %s", S.in_path, error.message);
        } else {
            post_compile_error(&error);
        }
        return false;
    }
    post(false, "%s done in %lld ms (%u bytes of code, in %s)",
         binary ? "load" : "compile",
         (long long)((esp_timer_get_time() - started) / 1000),
         (unsigned)solar_os_asm_code_size(program),
         solar_os_asm_code_in_psram(program) ? "PSRAM" : "internal RAM");

    const bool screen = wants_screen && solar_os_asm_screen_begin() == ESP_OK;
    solar_os_asm_set_text_output(asm_text_out);
    solar_os_asm_keyboard_reset();
    uint8_t *wad = read_wad();
    solar_os_asm_set_wad(wad);
    err = solar_os_asm_start(program, S.function, ASM_SCRIPT_STACK);
    if (err != ESP_OK) {
        post(false, "asm: cannot start %s()", S.function);
    } else {
        while (!S.stop_requested && !solar_os_asm_finished(program)) {
            solar_os_asm_keyboard_pump();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    const bool stopped = S.stop_requested && !solar_os_asm_finished(program);
    solar_os_asm_stop(program);
    /* Also when the script took the screen itself (enableGraphicMode()). */
    (void)screen;
    solar_os_asm_screen_end();
    char runtime[96];
    bool ok = err == ESP_OK;
    if (solar_os_asm_runtime_error(program, runtime, sizeof(runtime))) {
        post(false, "asm: runtime error: %s", runtime);
        ok = false;
    } else if (stopped) {
        post(false, "asm: stopped");
    } else if (ok) {
        post(false, "asm: %s() returned", S.function);
    }
    solar_os_asm_set_text_output(NULL);
    solar_os_asm_keyboard_reset();
    solar_os_asm_free(program);
    solar_os_asm_set_wad(NULL);
    solar_os_memory_free(wad);
    return ok;
}

/* This run's code memory, then internal RAM again. Without PSRAM code
 * (not an ESP32-S3), AUTO is internal RAM. */
static bool worker_run(void)
{
    if (!solar_os_asm_set_code_memory(S.code_memory)) {
        if (S.code_memory == SOLAR_OS_ASM_CODE_PSRAM) {
            post(false, "asm: -p: this board can't run code from PSRAM");
            return false;
        }
        (void)solar_os_asm_set_code_memory(SOLAR_OS_ASM_CODE_IRAM);
    }
    const bool ok = worker_run_loaded();
    (void)solar_os_asm_set_code_memory(SOLAR_OS_ASM_CODE_IRAM);
    return ok;
}

static void asm_task(void *arg)
{
    (void)arg;
    S.success = S.mode == ASM_MODE_RUN ? worker_run() : worker_compile();
    post(true, NULL);
    S.task_done = true;
    solar_os_task_delete_internal(NULL);
}

/* ------------------------------------------------------------- lifecycle */

/* Relative names are relative to the shell's current directory. */
static esp_err_t resolve(solar_os_context_t *ctx, solar_os_shell_io_t *io,
                         const char *arg, char *out)
{
    /* The shell's resolver uses the live session's current directory (as
     * scp/calc/files do); launch_cwd is the fallback. */
    esp_err_t err = solar_os_shell_resolve_path(ctx, arg, out, SOLAR_OS_STORAGE_PATH_MAX);
    if (err != ESP_OK) {
        err = solar_os_storage_resolve_path_at(ctx->launch_cwd, arg, out,
                                               SOLAR_OS_STORAGE_PATH_MAX);
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "asm: invalid path %s: %s\n", arg, esp_err_to_name(err));
    }
    return err;
}

static void default_binary_path(const char *source, char *out)
{
    strlcpy(out, source, SOLAR_OS_STORAGE_PATH_MAX);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot != NULL && (slash == NULL || dot > slash)) {
        *dot = '\0';
    }
    strlcat(out, ".bin", SOLAR_OS_STORAGE_PATH_MAX);
}

static esp_err_t asm_start(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = asm_io(ctx);
    const int argc = solar_os_context_argc(ctx);
    const char *verb = solar_os_context_argv(ctx, 1);
    const char *file = solar_os_context_argv(ctx, 2);
    const char *params[SOLAR_OS_ASM_ARGS_MAX];
    int param_count = 0;
    if (argc < 3 || verb == NULL || file == NULL) {
        asm_usage(io);
        solar_os_context_finish(ctx, argc < 2 ? 0 : 2, NULL);
        return ESP_OK;
    }
    strlcpy(S.function, "main", sizeof(S.function));
    S.code_memory = SOLAR_OS_ASM_CODE_AUTO;
    if (strcmp(verb, "run") == 0) {
        /* asm run [-p|-i] [-f function] <file> [params...] */
        S.mode = ASM_MODE_RUN;
        int index = 2;
        while (file != NULL &&
               (strcmp(file, "-p") == 0 || strcmp(file, "-i") == 0 || strcmp(file, "-f") == 0)) {
            if (strcmp(file, "-p") == 0 || strcmp(file, "-i") == 0) {
                S.code_memory = file[1] == 'p' ? SOLAR_OS_ASM_CODE_PSRAM : SOLAR_OS_ASM_CODE_IRAM;
                index += 1;
            } else {
                if (index + 2 >= argc) {
                    asm_usage(io);
                    solar_os_context_finish(ctx, 2, NULL);
                    return ESP_OK;
                }
                strlcpy(S.function, solar_os_context_argv(ctx, index + 1), sizeof(S.function));
                index += 2;
            }
            file = solar_os_context_argv(ctx, index);
        }
        if (file == NULL) {
            asm_usage(io);
            solar_os_context_finish(ctx, 2, NULL);
            return ESP_OK;
        }
        for (int k = index + 1; k < argc && param_count < SOLAR_OS_ASM_ARGS_MAX; k++) {
            params[param_count++] = solar_os_context_argv(ctx, k);
        }
    } else if (strcmp(verb, "compile") == 0 && (argc == 3 || argc == 5)) {
        S.mode = ASM_MODE_COMPILE;
        if (argc == 5) {
            if (strcmp(solar_os_context_argv(ctx, 3), "-o") != 0) {
                asm_usage(io);
                solar_os_context_finish(ctx, 2, NULL);
                return ESP_OK;
            }
            if (resolve(ctx, io, solar_os_context_argv(ctx, 4), S.out_path) != ESP_OK) {
                solar_os_context_finish(ctx, 2, NULL);
                return ESP_OK;
            }
        }
    } else if (strcmp(verb, "save") == 0 && argc <= 4) {
        S.mode = ASM_MODE_SAVE;
        if (argc == 4 && resolve(ctx, io, solar_os_context_argv(ctx, 3), S.out_path) != ESP_OK) {
            solar_os_context_finish(ctx, 2, NULL);
            return ESP_OK;
        }
    } else {
        asm_usage(io);
        solar_os_context_finish(ctx, 2, NULL);
        return ESP_OK;
    }
    if (resolve(ctx, io, file, S.in_path) != ESP_OK) {
        solar_os_context_finish(ctx, 2, NULL);
        return ESP_OK;
    }
    if (S.mode == ASM_MODE_SAVE && S.out_path[0] == '\0') {
        default_binary_path(S.in_path, S.out_path);
    }
    if (S.mode != ASM_MODE_RUN && has_suffix(S.in_path, ".bin")) {
        solar_os_shell_io_writeln(io, "asm: give the script source, not a .bin");
        solar_os_context_finish(ctx, 2, NULL);
        return ESP_OK;
    }
    if (S.mode == ASM_MODE_RUN) {
        solar_os_asm_set_args(params, param_count);
        solar_os_shell_io_printf(io, "asm: %s or Ctrl-C stops the script\n",
                                 solar_os_shell_io_app_exit_key(io));
    }
    solar_os_shell_io_flush(io);

    S.messages = solar_os_queue_create(ASM_MESSAGE_QUEUE_LEN, sizeof(asm_message_t));
    if (S.messages == NULL) {
        solar_os_context_finish(ctx, 1, "asm: out of memory");
        return ESP_OK;
    }
    /* Internal stack: the compiler and executeAsTask() touch IRAM code
     * buffers; keep the worker usable while the flash cache is off. */
    if (solar_os_task_create_pinned_internal(asm_task, "solar_os_asm", ASM_TASK_STACK,
                                             NULL, ASM_TASK_PRIORITY, &S.task, 1,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        solar_os_queue_delete(S.messages);
        S.messages = NULL;
        solar_os_context_finish(ctx, 1, "asm: task create failed");
        return ESP_OK;
    }
    return ESP_OK;
}

static void asm_drain(solar_os_context_t *ctx)
{
    if (S.messages == NULL || S.finished) {
        return;
    }
    solar_os_shell_io_t *io = asm_io(ctx);
    asm_message_t message;
    bool printed = false;
    while (xQueueReceive(S.messages, &message, 0) == pdPASS) {
        if (message.done) {
            S.finished = true;
            solar_os_shell_io_flush(io);
            solar_os_context_finish(ctx, S.success ? 0 : 1, NULL);
            return;
        }
        if (message.raw) {
            solar_os_shell_io_write(io, message.text);
        } else {
            solar_os_shell_io_writeln(io, message.text);
        }
        printed = true;
    }
    if (printed) {
        solar_os_shell_io_flush(io);
    }
}

static void asm_cleanup(void)
{
    if (asm_state_slot == NULL) {
        return;
    }
    if (S.messages != NULL) {
        solar_os_queue_delete(S.messages);
        S.messages = NULL;
    }
}

static void asm_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    S.stop_requested = true;
    if (S.task != NULL &&
        !solar_os_task_wait_done(S.task, &S.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }
    asm_cleanup();
}

static bool asm_state_release_ready(void)
{
    return asm_state_slot == NULL || S.task == NULL || S.task_done;
}

static bool asm_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        asm_drain(ctx);
        return true;
    }
    uint8_t ch = 0;
    uint8_t usage = 0;
    bool pressed = true;
    if (event->type == SOLAR_OS_EVENT_KEY) {
        ch = event->data.key.key;
        usage = (uint8_t)event->data.key.usage;
        pressed = event->data.key.action != SOLAR_OS_INPUT_KEY_RELEASE;
    } else if (event->type == SOLAR_OS_EVENT_CHAR) {
        ch = (uint8_t)event->data.ch;
    } else {
        return false;
    }
    if (pressed && (ch == SOLAR_OS_KEY_APP_EXIT || ch == ASM_CTRL_C)) {
        S.stop_requested = true;
        return true;
    }
    if (S.mode == ASM_MODE_RUN) {
        solar_os_asm_key_event(ch, usage, pressed);
    }
    return true;
}

const solar_os_app_t solar_os_asm_app = {
    .name = "asm",
    .summary = "ESPLiveScript compiler: compile, run and save binaries",
    .app_class = SOLAR_OS_APP_CLASS_COMMAND,
    /* SHELL_INLINE: run in the shell's own terminal (no clear screen), so
     * the compile/run messages stay in the shell transcript like ls. */
    .flags = SOLAR_OS_APP_FLAG_KEY_EVENTS | SOLAR_OS_APP_FLAG_SHELL_INLINE,
    .start = asm_start,
    .stop = asm_stop,
    .event = asm_event,
    .state_slot = &asm_state_slot,
    .state_size = sizeof(asm_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = asm_state_release_ready,
    .state_release_cleanup = asm_cleanup,
    .worker_stack_bytes = ASM_TASK_STACK,
    .tick_interval_ms = 20,
};
