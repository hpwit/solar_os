+++
title = "ESPLiveScript compiler"
id = "livescript"
section = "api"
summary = "Compile, run and save ESPLiveScript 2 programs with the asm command"
keywords = "asm espLivescript wad keyHeld ironforge asmparser2 compile script binary xtensa display updateDisplay keyboard matouch tprintf parameters argc argv"
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
asm run -p demo.bin              ... with the script's code in PSRAM (-i: internal RAM)
asm run demo.bin                 load a saved binary and run main()
asm compile demo.sc              compile only: report errors and binary size
asm compile demo.sc -o d.bin     compile and save the binary
asm save demo.sc [d.bin]         compile and save (default: demo.bin)
```

- Relative file names are relative to the shell's current directory
  (`cd /sdcard`, then `asm run game.sc`).
- `asm` runs in the shell's own terminal: its messages and what a text
  script prints stay in the shell transcript, like the output of `ls`.
- Compiling shows its progress on one line, rewritten as it goes:
  `compiling <file>...  48% optimizing` (the stages: reading, parsing,
  generating, optimizing, assembling, done; the percentage follows the
  time, so 50% is about half way), then `compile done in N ms`. A large
  script takes minutes on the board (Ironforge: about five), so the line
  shows it is working. Loading a `.bin` prints `loading <file>...` then
  `load done in N ms`.
- The app-exit key (Ctrl-]) or Ctrl-C stops a running script; Escape and the
  other keys go to the script.
- Compile errors show the line, the source line and a marker under the place
  the error was noticed. A binary that cannot be loaded says why, naming
  every binding it needs that this firmware lacks:
  `asm: tetris.bin: missing binding: led16, show16()`.

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
one with more gets 0 in the missing ones (0, 0.0 or NULL), which looks the
same as a 0 that was typed, so check `argc`:

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
| `tprintf(fmt, ...)`, `tprintln(fmt, ...)` | always into the SolarOS terminal, like a DOS program or the serial port |
| `printf(fmt, ...)`, `printfln(fmt, ...)`, `println(fmt, ...)` | on the screen when the script owns it (green, ANSI colours), into the terminal otherwise |
| `printscreen(fmt, ...)` | on the screen (keyboard5inch's ANSI text) |

A script that uses none of the screen bindings (`enableGraphicMode`,
`display`, `updateDisplay`, `setPixelsize`, `printscreen`, `triangle`, `pixel`,
`scroll`, `clear`, `zoom`, `stride`) is a text program: it leaves the
terminal in place and everything it prints goes into it. `asm` checks the
source, or a saved binary's binding names, before running; the names are
matched as whole words, comments included. `printf` without `\n` continues
the line.

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

On the screen, a script that never calls `updateDisplay()` is shown as a text
console: each print appears at once and the screen scrolls. Once a script
calls `updateDisplay()` or `setPixelsize()`, text appears with its next frame, as in
the MaTouch sketches.

## Graphic mode

A graphic script says so with `enableGraphicMode()`, first thing in `main()`:

```c
int main()
{
   enableGraphicMode();   // the whole screen is the script's from now on
   setPixelsize(2);
   ...
}
```

From then on the terminal stops drawing on the screen until the script ends,
so it never fights with the script's frames (flicker), and `printf` &co.
draw on the screen. `tprintf`/`tprintln` still go to the terminal, shown again
when the script ends. Because the name is in the source (and in a saved
binary), `asm` gives the script the screen before it starts. On a board
without a frame-buffer panel `enableGraphicMode()` does nothing.

## Names from the MaTouch sketches

The Arduino sketches of asmparser2 `examples/matouch` call the frame buffer
`led16` and show it with `show16()` / `show()`. In SolarOS they are
`display` and `updateDisplay()`: rename them when bringing a sketch's script
over. A binary saved with the old names is refused at load, with the names
to change (`missing binding: led16, show16()`): recompile its source.

## Saved binaries

A saved binary holds the machine code and the names of the host bindings it
uses, not their addresses: `asm run file.bin` resolves them against this
firmware. A binary that uses a binding the running firmware lacks is refused,
and the message lists every missing name (`missing binding: name, function()`).
Parameters work the same as for a source file. A binary is streamed from
the flash or the SD card as it loads: its code goes from the file straight
into executable memory, the file is never read into memory whole.

## Code memory

A script's machine code needs one contiguous block. SolarOS sets 26 KB of
executable internal RAM aside at boot, before it is broken up, and `asm run`
puts the code there when it fits (Valmora's 23.3 KB), else in any free
internal block big enough, else in PSRAM (Ironforge's 40.5 KB). The load
message says which.

In PSRAM the code is written there and run through the instruction bus's view
of the same memory, as native ELF modules are. It leaves the internal RAM
alone, and the code is fetched through the 16 KB instruction cache: Ironforge
runs as fast from PSRAM as from internal RAM. A script that redraws the whole
800x480 panel from PSRAM code shares the PSRAM bus with the panel's refresh,
and the picture can shake: that is what the block set aside is for.

`asm run -p` always puts the code in PSRAM, `asm run -i` always in internal
RAM (an error then gives the code's size and the largest free piece).

## Bindings

Every script can use, without declaring them:

| Binding | Meaning |
| --- | --- |
| `millis()`, `delay(ms)` | time since boot in ms, sleep |
| `rand(n)` | random number in 0..n-1 |
| `sin`, `atan2`, `hypot`, `log`, `sin8` | maths (`sin8` is FastLED's 0..255 sine) |
| `hsv(h,s,v)` | `CRGB` from FastLED's rainbow HSV |
| `hsv2(h,s,v)` | RGB565 colour, byte-swapped for `display` |
| `readKey()` | next key, 0 when none: arrows give `z` `q` `s` `d`, Escape 27, a released arrow its letter + 128 |
| `keyHeld(c)` | 1 while the key is down, by its `readKey()` character (arrows `z` `q` `s` `d`, letters in lower case, Enter 13, Tab 9), else 0: hold-to-move for any key |
| `keyChar`, `keyCode`, `key` | last key: character and USB HID usage |
| `setup_keyboard(_handle_)` | the script's `keyboard()` then runs on every key press |
| `enableGraphicMode()` | the script takes the whole screen (below) |
| `argc`, `arg(i)`, `argf(i)`, `argstr(i)` | the command-line parameters (above) |
| `tprintf`, `tprintln`, `printf`, `printfln`, `println` | text (above) |
| `wad`, `wadLoaded`, `wadFind(name)`, `wadSprite(prefix, frame, angle)` | the WAD next to the script (below) |

### A WAD next to the script

`asm run game.sc` (or `game.bin`) also reads `game.wad` when it is in the
same directory, into PSRAM, before the script starts, and frees it when the
script ends; it prints its size. The script sees its bytes as `wad` (a
`uint8_t *`), `wadLoaded` is 1, `wadFind("TITLEPIC")` gives where that
picture starts in `wad`, and `wadSprite("HUSK", 0, 1)` where `HUSKA1` starts
(-1 when there is none). Without the file, `wadLoaded` is 0. These are the
bindings of the asmparser2 Ironforge sketches, so their script needs no
change for its pictures.

On boards with the `rgb-lgfx` panel (`matouch7`) a script that uses the
screen owns it while it runs, as in the MaTouch sketches:

| Binding | Meaning |
| --- | --- |
| `display` | RGB565 back frame buffer, `stride` (800) pixels per row |
| `updateDisplay()` | show the back buffer at the next VSYNC and swap |
| `width`, `height`, `sizex`, `sizey` | logical size after `setPixelsize()` |
| `setResolution(M400x240)` | logical resolution, from the `M<width>x<height>` objects (below) |
| `setPixelsize(n)` | each pixel shown as n x n (400x240 for 2), the same as `setResolution` with both dividers n |
| `clear()` | clear the back buffer (and put the text at the top left) |
| `setCursor(x, y)` | where the next `printf` / `printscreen` text goes, in logical pixels; clears nothing |
| `setDoubleBuffer(on)` | `0`: draw straight on screen (`display`, text, triangles), `updateDisplay()` swaps nothing and returns at once; `1` (the default): draw into the hidden buffer, which starts as a copy of the screen, and `updateDisplay()` swaps at the next frame. Back to `1` when the script ends |
| `scroll` | hardware vertical scroll offset in lines |
| `printscreen(fmt, ...)` | text with ANSI colours, scrolling with `scroll` |
| `triangle(x0,y0,x1,y1,x2,y2,c)` | filled triangle |
| `pixel(x,y,hue)`, `zoom` | a `zoom` x `zoom` block of a hue, 0 for black |

### Resolutions

`setResolution()` sets the logical resolution: the 800x480 panel divided by
1 to 5 across and, independently, 1 to 5 down. Each of the 25 resolutions is
an object `M<width>x<height>`, of the bound structure `resolution`:

```c
struct resolution
{
   int width;    // logical width:  800 / divx
   int height;   // logical height: 480 / divy
   int divx;     // each logical pixel is divx panel pixels wide (1..5)
   int divy;     // ... and divy panel pixels high (1..5)
};
```

| height \ width | **800** (/1) | **400** (/2) | **266** (/3) | **200** (/4) | **160** (/5) |
| --- | --- | --- | --- | --- | --- |
| **480** (/1) | `M800x480` | `M400x480` | `M266x480` | `M200x480` | `M160x480` |
| **240** (/2) | `M800x240` | `M400x240` | `M266x240` | `M200x240` | `M160x240` |
| **160** (/3) | `M800x160` | `M400x160` | `M266x160` | `M200x160` | `M160x160` |
| **120** (/4) | `M800x120` | `M400x120` | `M266x120` | `M200x120` | `M160x120` |
| **96** (/5) | `M800x96` | `M400x96` | `M266x96` | `M200x96` | `M160x96` |

```c
setResolution(M400x240);        // 2 x 2 panel pixels per pixel
setResolution(M800x240);        // full width, half height
setResolution(M160x480);        // 5 x 1
int w = M266x160.width;         // the objects can be read: 266
```

After the call `width` and `height` hold the new logical size (`sizex`,
`sizey` too), both buffers are clear and the scroll is reset. Rows keep the
panel stride: pixel `(x, y)` is `display[y * stride + x]` at every
resolution. At width 266 (800 / 3) the last 2 panel columns stay black.
`setPixelsize(n)` is `setResolution` with both dividers `n` (`setPixelsize(6)`
is `M266x480`, as in keyboard5inch).

Both frame buffers start black. When the script ends or is stopped, the
terminal comes back.

```c
int main()
{
   setPixelsize(2);
   for (int f = 0; f < 600; f++)
   {
      for (int y = 0; y < height; y++)
         for (int x = 0; x < width; x++)
            display[y * stride + x] = hsv2(x + y + f, 255, 255);
      updateDisplay();
      if (readKey() == 27)
         return 0;
   }
}
```

## Examples

`examples/asm/` in the repository, also on the `matouch7` flash:

| File | What it shows |
| --- | --- |
| `fib.sc` | a text program: `asm run fib.sc` prints a table, `asm run fib.sc 40` computes fib(40) |
| `hello.sc` | a string parameter: `asm run hello.sc Yves 3` |
| `bouncingballs.sc` | the number of balls as a parameter: `asm run bouncingballs.sc 50` |
| `hello_screen.sc` | `printf` / `println` as a text console on the screen |
| `tetris.sc`, `neonracer.sc`, `mazechase.sc`, `prismbreak.sc` | the MaTouch games, with `enableGraphicMode()` |
| `vault13h.sc` | a first-person maze shooter in the VGA style: 320x240 in the 256-colour VGA default palette, each pixel 2x2 (`M400x240`), so 640x480 centred on the panel; three looks (`asm run vault13h.sc 1`, `2` or `3`), textures and distance shading through the palette |
| `ironforge.sc`, `ironforge.wad` | a first-person shooter in the style of the mid-90s (floor and ceiling heights, stairs, lava, the sky, doors, a key, three levels, three guns): the asmparser2 `examples/matouch/Ironforge` script at pixel size 3, the game's 240x160 in the middle of the 266x160 screen. Its monsters, face, guns, pickups and title come from `ironforge.wad` (Freedoom's art, BSD licence: `ironforge-freedoom-COPYING.txt`), read next to it. Keys held: arrows or Z Q S D walk and turn, Space fires, E or Enter uses, G or Tab the next gun, Escape quits. Compiling it takes about five and a half minutes on the board, so `asm save ironforge.sc` once, then `asm run ironforge.bin` |
| `valmora.sc`, `valmora.wad` | a top-down adventure in the style of the early role-playing games, at the full 800x480: a world map, three towns and 30 people to talk to, four torch-lit dungeons, three shards of a broken crown and a sorcerer to end (the asmparser2 `examples/matouch/Valmora` script; its tools there make the WAD). Every picture (32x32 tiles, 16 of the 4096 colours each), map and line of talk is in `valmora.wad`, read next to it. Arrows or Z Q S D walk (into someone to talk, into a monster to fight), M mends, F then a direction throws a spark, P drinks a potion, Space waits, Enter turns a page. `asm save valmora.sc` once, then `asm run valmora.bin`. At full resolution the picture can still shake a little when walking (the panel's refresh and the drawing share the PSRAM bus); the GameLauncher and the sketch don't |
| `valmoralow.sc`, `valmoralow.wad` | Valmora at 400x240 (16x16 tiles, a pixel font drawn for that size): steady on this panel. `asm run valmoralow.bin` (or `... 2`) fills the screen at pixel size 2; `asm run valmoralow.bin 1` shows it at pixel size 1, in the middle. Made by the same tools: `make_wad.py --low` |
| `resolutions.sc` | every `setResolution()` resolution in turn, with a bouncing 50 x 50 image and the resolution written on the screen: `asm run resolutions.sc 5` (5 s each) |
| `mandelbrot.sc`, `gameoflife.sc` | full-screen graphics, with `enableGraphicMode()` |

The games and the graphic demos come from the asmparser2 `examples/matouch`
sketches; the only change is the `enableGraphicMode()` call at the top of
`main()` (Ironforge also reads its keys with `keyHeld()` and quits on Escape,
where its sketch kept a bit per held key). A `.bin` saved before that call existed still gets the screen (its
`display` / `updateDisplay` names are recognised), but recompile it to have
the call in it.
