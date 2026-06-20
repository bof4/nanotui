/*
 * nanotui.h - minimal terminal UI toolkit, zero external deps.
 *
 * No ncurses, no terminfo database lookups, none of that. Just raw
 * ANSI/VT100 escapes + termios. If the terminal can do cursor
 * positioning and SGR colors (basically everything since the 90s,
 * including every modern emulator) this works.
 *
 * Build: just compile nanotui.c together with your program, link
 * against nothing but libc. Needs a POSIX-ish environment for
 * termios/ioctl/signal (Linux, macOS, *BSD - not raw Windows, sorry,
 * you'd need a termios shim or to swap the backend for the Win32
 * console API).
 *
 * Whole front+back screen buffer for a big 300x80 terminal is well
 * under half a meg, so the <1MB RSS budget has a lot of slack in it.
 */
#ifndef NANOTUI_H
#define NANOTUI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* color indices - these map onto the standard 16-color ANSI palette,
 * not 256-color. Deliberate choice: 16-color SGR codes (30-37/90-97)
 * are about as universally supported as it gets, more so than 256
 * color mode on some weirder terminals/serial consoles. */
enum {
    TUI_DEFAULT = -1,
    TUI_BLACK = 0, TUI_RED, TUI_GREEN, TUI_YELLOW,
    TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE,
    TUI_BR_BLACK = 8, TUI_BR_RED, TUI_BR_GREEN, TUI_BR_YELLOW,
    TUI_BR_BLUE, TUI_BR_MAGENTA, TUI_BR_CYAN, TUI_BR_WHITE
};

enum {
    TUI_ATTR_NONE = 0,
    TUI_ATTR_BOLD = 1 << 0,
    TUI_ATTR_DIM  = 1 << 1,
    TUI_ATTR_REV  = 1 << 2,   /* reverse video, e.g. for a selected row */
};

/* a handful of special keys decoded out of escape sequences. anything
 * else just comes back as its raw byte value. */
enum {
    TUI_KEY_UP = 1000,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_RESIZE,  /* synthetic - fired once after a SIGWINCH */
};

typedef struct tui tui_t;

typedef struct {
    int key;
    int is_special;
} tui_event_t;

tui_t *tui_init(void);
void   tui_shutdown(tui_t *t);

int tui_width(const tui_t *t);
int tui_height(const tui_t *t);

/* call tui_clear() at the top of every frame, draw your widgets, then
 * tui_flush() once at the end. flush diffs against what's already on
 * screen and only repaints the cells that actually changed. */
void tui_clear(tui_t *t);
void tui_flush(tui_t *t);

void tui_putc(tui_t *t, int x, int y, uint32_t codepoint, int fg, int bg, int attr);
void tui_text(tui_t *t, int x, int y, const char *utf8_str, int fg, int bg, int attr);
void tui_textf(tui_t *t, int x, int y, int fg, int bg, const char *fmt, ...);

void tui_box(tui_t *t, int x, int y, int w, int h, const char *title, int fg);
void tui_hline(tui_t *t, int x, int y, int w, int fg);
void tui_vline(tui_t *t, int x, int y, int h, int fg);

/* horizontal gauge/progress bar, pct clamped to [0,1]. Uses the
 * unicode eighth-block glyphs for sub-character resolution so it
 * doesn't look like a chunky 8-bit progress bar. */
void tui_gauge(tui_t *t, int x, int y, int w, double pct, int fg, const char *label);

/* bar/area chart, h rows tall. data[] is n samples in [0,1], oldest
 * first. if n > w only the last w samples are drawn. same eighth-block
 * trick as tui_gauge but stacked vertically. */
void tui_chart(tui_t *t, int x, int y, int w, int h, const double *data, int n, int fg);

/* headers/rows are plain UTF-8 C strings. column widths are computed
 * from content (clamped to fit max_w) - no manual sizing, no wrapping.
 * good enough for short, single-line rows. */
void tui_table(tui_t *t, int x, int y, int max_w,
               const char **headers, int ncols,
               const char *const *rows, int nrows, int fg);

/* non-blocking - returns 0 immediately if there's nothing to read */
int tui_poll_event(tui_t *t, tui_event_t *ev);

void tui_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif
#endif /* NANOTUI_H */
