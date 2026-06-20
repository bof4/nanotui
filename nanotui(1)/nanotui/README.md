# nanotui

Zero-dependency TUI toolkit in C. No ncurses, no terminfo, nothing —
just raw ANSI/VT100 escapes and termios. Built this mostly because I
wanted a CPU/RAM monitor that doesn't pull in half of Homebrew to draw
a progress bar.

```
make            # builds ./demo
make static     # builds ./demo_static (see "about that <1MB" below)
./demo          # q or Esc to quit
```

## What's in here

- `nanotui.h` / `nanotui.c` — the actual library. Double-buffered
  screen (you draw into a back buffer, `tui_flush()` diffs it against
  what's already on screen and only repaints what changed), plus a
  handful of widgets: text, boxes, lines, a gauge bar, a bar/area
  chart, and a bare-bones table. Everything in the gauge/chart widgets
  uses the Unicode eighth-block glyphs (▏▎▍▌▋▊▉█ and friends) so a bar
  at 37% actually looks like 37%, not a staircase.
- `demo_hwmon.c` — the hardware monitor that was the whole point of
  this. Reads `/proc/stat` for per-core CPU usage, `/proc/meminfo` for
  RAM, `/proc/loadavg` for load average, and draws it all with the
  widgets above.

## Portability

The library itself (`nanotui.c`) only needs termios + ioctl + signal,
which is any POSIX system — Linux, macOS, the BSDs. It is **not**
Windows-portable as-is; the console API is different enough that
you'd want a separate backend rather than a `#ifdef` patch job.

The demo (`demo_hwmon.c`) is Linux-only because it reads `/proc`
directly. Wiring it up on macOS would mean swapping that bit for
`host_statistics()`/`sysctl`, which I didn't bother doing here.

Also not handled: wide glyphs that occupy two terminal columns (CJK,
some emoji), combining characters. UTF-8 decode/encode is correct for
single-width codepoints, which covers ASCII, box-drawing chars, and
the block glyphs the widgets use — just don't expect a Chinese label
in a gauge to line up nicely.

## About that "<1MB" thing

The screen buffers themselves are tiny — front + back buffer for a
huge 300x80 terminal is about 375KB, nowhere close to a megabyte. But
if you check actual process RSS on a normal dynamically-linked build:

```
VmRSS:  ~1.9 MB
```

That's not the library's fault. Most of it is `libc.so` / `ld.so`
pages that get counted against this process's RSS even though they're
shared across every other process on the system also using glibc.
`Pss` (proportional share) for the same process is more like 680KB,
and the actual private/dirty memory this program owns — heap, stack,
its own buffers — is under 150KB.

If you want a number that matches the "single tiny binary" pitch
without the dynamic-linking asterisk, build it static:

```
make static
./demo_static
```

That measured at ~900KB RSS in testing here. Bigger binary on disk
(static glibc isn't small), smaller footprint in RAM, which is the
tradeoff you're making.

## Why ANSI escapes instead of ncurses

ncurses means linking against a library that may or may not be present
or the same version on whatever box you're building on, plus it wants
a terminfo database to know what your terminal supports. Raw escapes
mean the only dependency is "does this terminal understand cursor
positioning and SGR color," which has been true for basically every
terminal emulator since the 90s. Trade-off is you lose ncurses' actual
terminal capability detection — if someone's on some genuinely exotic
terminal that doesn't speak VT100-ish escapes, this won't degrade
gracefully, it'll just look wrong. Haven't hit that in practice.

## License

Do whatever you want with it.
