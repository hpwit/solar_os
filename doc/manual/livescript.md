+++
title = "ESPLiveScript compiler"
id = "livescript"
section = "api"
summary = "Compile, run and save ESPLiveScript 2 programs with the asm command"
keywords = "asm espLivescript asmparser2 compile script binary xtensa keyboard tprintf printf parameters argc argv"
packages_any = ["app_asm"]
+++

# ESPLiveScript compiler

`asm` runs [ESPLiveScript 2](https://github.com/hpwit/new-parser)
(`components/asmparser2`): a C-like language compiled to real Xtensa machine
code on the device. It is available on ESP32 and ESP32-S3 targets with the
`asm` package group (in the `full` flavor). The language itself (types,
structures, pointers, `ffloat`, ...) is described in the
[asmparser2 README](https://github.com/hpwit/new-parser#readme).

## Quick reference

```text
asm run demo.sc                  compile demo.sc and run main()
asm run demo.sc 40 1.5 Yves      ... with parameters for main()
asm run -f setup demo.sc         run another function than main()
asm run demo.bin                 load a saved binary and run main()
asm compile demo.sc              compile only: report errors and binary size
asm compile demo.sc -o d.bin     compile and save the binary
asm save demo.sc [d.bin]         compile and save (default: demo.bin)
```

- Relative file names are relative to the shell's current directory
  (`cd /sdcard`, then `asm run game.sc`).
- `asm` runs in the shell's own terminal: its messages and what a text
  script prints stay in the shell transcript, like the output of `ls`.
- Compiling prints `compiling <file>...` then `compile done in N ms`; loading
  a `.bin` prints `loading <file>...` then `load done in N ms`.
- The app-exit key (Ctrl-]) or Ctrl-C stops a running script; Escape and the
  other keys go to the script.
- Compile errors show the line, the source line and a marker under the place
  the error was noticed. A binary that cannot be loaded says why, naming
  every binding it needs that this firmware lacks:
  `asm: game.bin: missing binding: display, updateDisplay()`.

## Parameters

Up to 6 parameters follow the file name. Each one is:

| Written as | Becomes | Script parameter |
| --- | --- | --- |
| a whole number: `40`, `-3`, `0xFF` | an int | `int`, `uint32_t`, `uint8_t`, `bool`... |
| another number: `1.5`, `2e3` | a float | `float` |
| anything else: `Yves`, `red` | a string, passed by address | `char *` |

They are passed to the function that runs (`int main(int n)` receives `40`),
and the script can also read them anywhere:

| Binding | Meaning |
| --- | --- |
| `argc` | how many parameters were given |
| `arg(i)` | parameter `i` as an int |
| `argf(i)` | parameter `i` as a float |
| `argstr(i)` | parameter `i` as text (`char *`) |

A function with fewer parameters than given values ignores the extra ones;
one with more sees garbage in the missing ones, so check `argc`:

```c
// asm run hello.sc Yves 3
void main(char *name, int times)
{
   if (argc < 1)
      name = "world";
   if (argc < 2)
      times = 1;
   for (int i = 1; i <= times; i++)
      tprintln("%d: hello %s", i, name);
}
```

Underneath, `asm` uses asmparser2's `executeAsTask(name, args, nargs, ...)`
(see "Parameters for a task" in the asmparser2 README).

## Printing

| Function | Prints |
| --- | --- |
| `tprintf(fmt, ...)`, `tprintln(fmt, ...)` | into the SolarOS terminal, like a DOS program or the serial port |
| `printf(fmt, ...)`, `printfln(fmt, ...)`, `println(fmt, ...)` | the same: into the terminal |

A script prints into the shell's terminal, which stays in place while it
runs. `printf` without `\n` continues the line.

```c
// asm run fib.sc 40     ->  fib(40) = 102334155 and the time it took
int fib(int n)
{
   if (n < 2)
      return n;
   return fib(n - 1) + fib(n - 2);
}

int main(int n)
{
   uint32_t start = millis();
   int value = fib(n);
   tprintln("fib(%d) = %d   (%d ms)", n, value, millis() - start);
}
```

## Graphic mode

A graphic script says so with `enableGraphicMode()`, first thing in `main()`.
On a board whose display driver gives scripts the frame buffer, the script
then owns the whole screen and the terminal stops drawing until it ends, so
the two never fight over the screen (flicker). The name is also looked for
in the source and in a saved binary (`@_ext_NAME` / `@_NAME(...)` entries),
as are the frame-buffer bindings' names (`display`, `updateDisplay`,
`setPixelsize`...), so the script gets the screen before it starts. This build has no such
driver: `enableGraphicMode()` does nothing, and the script prints into the
terminal.

## Saved binaries

A saved binary holds the machine code and the names of the host bindings it
uses, not their addresses: `asm run file.bin` resolves them against this
firmware. A binary that uses a binding the running firmware lacks is refused,
and the message lists every missing name (`missing binding: name, function()`).
Parameters work the same as for a source file.

## Bindings

Every script can use, without declaring them:

| Binding | Meaning |
| --- | --- |
| `millis()`, `delay(ms)` | time since boot in ms, sleep |
| `rand(n)` | random number in 0..n-1 |
| `sin`, `atan2`, `hypot`, `log`, `sin8` | maths (`sin8` is FastLED's 0..255 sine) |
| `hsv(h,s,v)` | `CRGB` from FastLED's rainbow HSV |
| `hsv2(h,s,v)` | RGB565 colour, byte-swapped |
| `readKey()` | next key, 0 when none: arrows give `z` `q` `s` `d`, Escape 27, a released arrow its letter + 128 |
| `keyChar`, `keyCode`, `key` | last key: character and USB HID usage |
| `setup_keyboard(_handle_)` | the script's `keyboard()` then runs on every key press |
| `enableGraphicMode()` | graphic mode (below) |
| `argc`, `arg(i)`, `argf(i)`, `argstr(i)` | the command-line parameters (above) |
| `tprintf`, `tprintln`, `printf`, `printfln`, `println` | text (above) |

## Examples

`examples/asm/` in the repository (copy them to the SD card or the flash):

| File | What it shows |
| --- | --- |
| `fib.sc` | a text program: `asm run fib.sc` prints a table, `asm run fib.sc 40` computes fib(40) |
| `hello.sc` | a string parameter: `asm run hello.sc Yves 3` |
