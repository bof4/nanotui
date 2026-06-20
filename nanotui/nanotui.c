/*
 * nanotui.c - see nanotui.h for the why/what. this file is the how.
 *
 * Rough structure:
 *   - termios raw mode + alt screen on init, restore everything on shutdown
 *   - a "back" buffer that widgets draw into, and a "front" buffer that
 *     mirrors what's actually on the screen right now
 *   - flush() walks both, only emits escapes for cells that differ
 *   - everything else is just glyph-pushing into the back buffer
 *
 * Not handled (yet, maybe never): wide CJK glyphs that eat two
 * terminal columns, combining characters, anything fancier than basic
 * UTF-8 decode/encode. Fine for box-drawing chars, ASCII text and the
 * block glyphs the gauge/chart widgets use, which is all this was
 * built for.
 */
/* need this before any system headers, or -std=c11 hides sigaction,
 * nanosleep, etc behind strict ISO C visibility */
#define _POSIX_C_SOURCE 200809L

#include "nanotui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>

typedef struct {
    uint32_t ch;
    int8_t   fg;
    int8_t   bg;
    uint8_t  attr;
} cell_t;

struct tui {
    int w, h;
    cell_t *front;
    cell_t *back;
};

static struct termios g_orig_termios;
static int g_raw_active = 0;
static volatile sig_atomic_t g_resized = 0;

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

static void get_term_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        /* serial console or something equally weird, just guess */
        *w = 80;
        *h = 24;
    }
}

static void enable_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;

    /* ISIG off too - apps built on this are expected to handle their
     * own quit key (q, Esc, whatever) rather than relying on ^C */
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_active = 1;
}

static void disable_raw_mode(void)
{
    if (g_raw_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw_active = 0;
}

static void mark_front_dirty(tui_t *t)
{
    /* fill front buffer with a value no real cell will ever have, so
     * the very first flush paints the whole screen instead of
     * skipping cells that happen to already be blank/zeroed */
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; i++) {
        t->front[i].ch = 0xFFFFFFFFu;
        t->front[i].fg = -2;
        t->front[i].bg = -2;
        t->front[i].attr = 0xFF;
    }
}

tui_t *tui_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return NULL; /* running under a pipe/redirect, nothing to draw to */

    tui_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    get_term_size(&t->w, &t->h);

    size_t n = (size_t)t->w * (size_t)t->h;
    t->front = calloc(n, sizeof(cell_t));
    t->back  = calloc(n, sizeof(cell_t));
    if (!t->front || !t->back) {
        free(t->front);
        free(t->back);
        free(t);
        return NULL;
    }

    enable_raw_mode();
    fputs("\x1b[?1049h", stdout);  /* alt screen - keep their scrollback intact */
    fputs("\x1b[?25l", stdout);    /* hide cursor */
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    mark_front_dirty(t);
    return t;
}

void tui_shutdown(tui_t *t)
{
    if (!t) return;

    fputs("\x1b[?25h", stdout);    /* show cursor again */
    fputs("\x1b[?1049l", stdout);  /* back to the normal screen */
    fflush(stdout);
    disable_raw_mode();

    free(t->front);
    free(t->back);
    free(t);
}

int tui_width(const tui_t *t)  { return t ? t->w : 0; }
int tui_height(const tui_t *t) { return t ? t->h : 0; }

static int do_resize(tui_t *t)
{
    int nw, nh;
    get_term_size(&nw, &nh);
    if (nw == t->w && nh == t->h)
        return 0;

    free(t->front);
    free(t->back);
    size_t n = (size_t)nw * (size_t)nh;
    t->front = calloc(n, sizeof(cell_t));
    t->back  = calloc(n, sizeof(cell_t));
    t->w = nw;
    t->h = nh;
    mark_front_dirty(t);

    /* full repaint, no point trying to be clever here */
    fputs("\x1b[2J", stdout);
    fflush(stdout);
    return 1;
}

void tui_clear(tui_t *t)
{
    if (!t || !t->back) return;
    memset(t->back, 0, (size_t)t->w * (size_t)t->h * sizeof(cell_t));
    /* memset zero gives ch=0, fg=0, bg=0 - 0 is TUI_BLACK, not
     * TUI_DEFAULT, so fix that up rather than special-casing it in
     * every single draw call */
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; i++) {
        t->back[i].ch = ' ';
        t->back[i].fg = TUI_DEFAULT;
        t->back[i].bg = TUI_DEFAULT;
    }
}

/* ---- UTF-8 ------------------------------------------------------- */

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* advances *p past one utf8 codepoint, returns the decoded value.
 * garbage in -> garbage out, no real validation, this isn't meant to
 * survive hostile input */
static uint32_t utf8_decode(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    int extra;

    if (s[0] < 0x80) { cp = s[0]; extra = 0; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else { *p += 1; return '?'; } /* invalid lead byte */

    int i;
    for (i = 1; i <= extra; i++) {
        if (s[i] == 0) { *p += i; return cp; } /* truncated, bail */
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += extra + 1;
    return cp;
}

/* ---- drawing into the back buffer -------------------------------- */

void tui_putc(tui_t *t, int x, int y, uint32_t codepoint, int fg, int bg, int attr)
{
    if (!t || x < 0 || y < 0 || x >= t->w || y >= t->h)
        return;
    cell_t *c = &t->back[(size_t)y * t->w + x];
    c->ch = codepoint;
    c->fg = (int8_t)fg;
    c->bg = (int8_t)bg;
    c->attr = (uint8_t)attr;
}

void tui_text(tui_t *t, int x, int y, const char *str, int fg, int bg, int attr)
{
    if (!t || !str) return;
    const char *p = str;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        tui_putc(t, cx, y, cp, fg, bg, attr);
        cx++;
        if (cx >= t->w) break; /* no wrapping, just clip - keep it simple */
    }
}

void tui_textf(tui_t *t, int x, int y, int fg, int bg, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_text(t, x, y, buf, fg, bg, TUI_ATTR_NONE);
}

void tui_hline(tui_t *t, int x, int y, int w, int fg)
{
    for (int i = 0; i < w; i++)
        tui_putc(t, x + i, y, 0x2500 /* ─ */, fg, TUI_DEFAULT, 0);
}

void tui_vline(tui_t *t, int x, int y, int h, int fg)
{
    for (int i = 0; i < h; i++)
        tui_putc(t, x, y + i, 0x2502 /* │ */, fg, TUI_DEFAULT, 0);
}

void tui_box(tui_t *t, int x, int y, int w, int h, const char *title, int fg)
{
    if (!t || w < 2 || h < 2) return;

    tui_putc(t, x,         y,         0x250C, fg, TUI_DEFAULT, 0); /* ┌ */
    tui_putc(t, x + w - 1, y,         0x2510, fg, TUI_DEFAULT, 0); /* ┐ */
    tui_putc(t, x,         y + h - 1, 0x2514, fg, TUI_DEFAULT, 0); /* └ */
    tui_putc(t, x + w - 1, y + h - 1, 0x2518, fg, TUI_DEFAULT, 0); /* ┘ */

    tui_hline(t, x + 1, y, w - 2, fg);
    tui_hline(t, x + 1, y + h - 1, w - 2, fg);
    tui_vline(t, x, y + 1, h - 2, fg);
    tui_vline(t, x + w - 1, y + 1, h - 2, fg);

    if (title && *title) {
        /* drop it into the top border, padded with a space either
         * side - htop/btop do the same thing, looks decent */
        char buf[128];
        snprintf(buf, sizeof(buf), " %s ", title);
        int avail = w - 4;
        if (avail > 0)
            tui_text(t, x + 2, y, buf, fg, TUI_DEFAULT, TUI_ATTR_BOLD);
    }
}

/* eighth-block glyphs, index 0 (empty) to 8 (full). "left n eighths"
 * variants - the filled part grows from the left, which is what you
 * want for a horizontal bar */
static const uint32_t g_left_eighths[9] = {
    ' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588
};

/* same idea but "lower n eighths" - filled part grows from the
 * bottom, for vertical bars/charts */
static const uint32_t g_lower_eighths[9] = {
    ' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588
};

void tui_gauge(tui_t *t, int x, int y, int w, double pct, int fg, const char *label)
{
    if (!t || w <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;

    /* reserve room on the right for a percentage readout unless the
     * bar is too narrow to bother */
    char pctbuf[8];
    snprintf(pctbuf, sizeof(pctbuf), "%3d%%", (int)(pct * 100.0 + 0.5));
    int pct_w = (w >= 8) ? (int)strlen(pctbuf) + 1 : 0;
    int bar_w = w - pct_w;
    if (bar_w < 1) bar_w = w;

    int total_eighths = (int)(pct * bar_w * 8.0 + 0.5);
    if (total_eighths > bar_w * 8) total_eighths = bar_w * 8;

    for (int i = 0; i < bar_w; i++) {
        int e = total_eighths - i * 8;
        if (e < 0) e = 0;
        if (e > 8) e = 8;
        tui_putc(t, x + i, y, g_left_eighths[e], fg, TUI_DEFAULT, 0);
    }
    if (pct_w > 0)
        tui_text(t, x + bar_w + 1, y, pctbuf, fg, TUI_DEFAULT, 0);

    if (label && *label) {
        /* overlay the label on top of the bar itself rather than
         * eating extra rows for it - reverse video so it stays
         * readable regardless of how full the bar is */
        int lx = x + 1;
        const char *p = label;
        while (*p && lx < x + bar_w - 1) {
            uint32_t cp = utf8_decode(&p);
            tui_putc(t, lx, y, cp, TUI_DEFAULT, TUI_DEFAULT, TUI_ATTR_REV);
            lx++;
        }
    }
}

void tui_chart(tui_t *t, int x, int y, int w, int h, const double *data, int n, int fg)
{
    if (!t || w <= 0 || h <= 0 || n <= 0) return;

    int start = (n > w) ? n - w : 0;
    int cols = (n > w) ? w : n;
    int xoff = w - cols; /* right-align if we have fewer samples than columns */

    for (int i = 0; i < cols; i++) {
        double v = data[start + i];
        if (v < 0) v = 0;
        if (v > 1) v = 1;

        int eighths = (int)(v * h * 8.0 + 0.5);
        if (eighths > h * 8) eighths = h * 8;

        for (int row = 0; row < h; row++) {
            /* row 0 is the top, but we fill bottom-up */
            int rows_from_bottom = h - 1 - row;
            int e = eighths - rows_from_bottom * 8;
            if (e < 0) e = 0;
            if (e > 8) e = 8;
            tui_putc(t, x + xoff + i, y + row, g_lower_eighths[e], fg, TUI_DEFAULT, 0);
        }
    }
}

void tui_table(tui_t *t, int x, int y, int max_w,
               const char **headers, int ncols,
               const char *const *rows, int nrows, int fg)
{
    if (!t || ncols <= 0) return;

    int *col_w = calloc((size_t)ncols, sizeof(int));
    if (!col_w) return;

    for (int c = 0; c < ncols; c++)
        col_w[c] = headers && headers[c] ? (int)strlen(headers[c]) : 0;

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++) {
            const char *cell = rows[(size_t)r * ncols + c];
            int len = cell ? (int)strlen(cell) : 0;
            if (len > col_w[c]) col_w[c] = len;
        }
    }

    /* if the natural total is wider than max_w, just shrink columns
     * proportionally. crude, but this lib isn't trying to be a
     * spreadsheet renderer. */
    int total = 0;
    for (int c = 0; c < ncols; c++) total += col_w[c] + 2;
    if (max_w > 0 && total > max_w) {
        double scale = (double)max_w / (double)total;
        for (int c = 0; c < ncols; c++) {
            col_w[c] = (int)(col_w[c] * scale);
            if (col_w[c] < 3) col_w[c] = 3;
        }
    }

    int row_y = y;
    if (headers) {
        int cx = x;
        for (int c = 0; c < ncols; c++) {
            tui_text(t, cx, row_y, headers[c] ? headers[c] : "", fg, TUI_DEFAULT, TUI_ATTR_BOLD);
            cx += col_w[c] + 2;
        }
        row_y++;
        tui_hline(t, x, row_y, (max_w > 0 ? max_w : (cx - x)), fg);
        row_y++;
    }

    for (int r = 0; r < nrows; r++) {
        int cx = x;
        for (int c = 0; c < ncols; c++) {
            const char *cell = rows[(size_t)r * ncols + c];
            tui_text(t, cx, row_y, cell ? cell : "", fg, TUI_DEFAULT, TUI_ATTR_NONE);
            cx += col_w[c] + 2;
        }
        row_y++;
    }

    free(col_w);
}

/* ---- flush / diff -------------------------------------------------- */

static int sgr_append(char *out, size_t cap, int fg, int bg, int attr)
{
    /* always a full reset + reapply rather than trying to track which
     * SGR params are already set on the real terminal. simpler, and
     * the diffing in tui_flush already keeps us from emitting this
     * for every single cell anyway. */
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "\x1b[0");

    if (attr & TUI_ATTR_BOLD) pos += (size_t)snprintf(out + pos, cap - pos, ";1");
    if (attr & TUI_ATTR_DIM)  pos += (size_t)snprintf(out + pos, cap - pos, ";2");
    if (attr & TUI_ATTR_REV)  pos += (size_t)snprintf(out + pos, cap - pos, ";7");

    if (fg == TUI_DEFAULT) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";39");
    } else if (fg >= 0 && fg < 8) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";%d", 30 + fg);
    } else if (fg >= 8 && fg < 16) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";%d", 90 + (fg - 8));
    }

    if (bg == TUI_DEFAULT) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";49");
    } else if (bg >= 0 && bg < 8) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";%d", 40 + bg);
    } else if (bg >= 8 && bg < 16) {
        pos += (size_t)snprintf(out + pos, cap - pos, ";%d", 100 + (bg - 8));
    }

    out[pos++] = 'm';
    return (int)pos;
}

void tui_flush(tui_t *t)
{
    if (!t) return;

    static char outbuf[1 << 16];
    size_t pos = 0;

    int cur_x = -1, cur_y = -1;
    int last_fg = -2, last_bg = -2, last_attr = -1;

    for (int y = 0; y < t->h; y++) {
        for (int x = 0; x < t->w; x++) {
            size_t idx = (size_t)y * t->w + x;
            cell_t *bc = &t->back[idx];
            cell_t *fc = &t->front[idx];

            if (bc->ch == fc->ch && bc->fg == fc->fg && bc->bg == fc->bg && bc->attr == fc->attr)
                continue;

            if (pos > sizeof(outbuf) - 64) {
                fwrite(outbuf, 1, pos, stdout);
                pos = 0;
            }

            if (cur_y != y || cur_x != x)
                pos += (size_t)snprintf(outbuf + pos, sizeof(outbuf) - pos, "\x1b[%d;%dH", y + 1, x + 1);

            if (bc->fg != last_fg || bc->bg != last_bg || bc->attr != last_attr) {
                pos += (size_t)sgr_append(outbuf + pos, sizeof(outbuf) - pos, bc->fg, bc->bg, bc->attr);
                last_fg = bc->fg;
                last_bg = bc->bg;
                last_attr = bc->attr;
            }

            char utf[4];
            int n = utf8_encode(bc->ch ? bc->ch : ' ', utf);
            memcpy(outbuf + pos, utf, (size_t)n);
            pos += (size_t)n;

            cur_x = x + 1;
            cur_y = y;
            *fc = *bc;
        }
    }

    if (pos) fwrite(outbuf, 1, pos, stdout);
    fflush(stdout);
}

/* ---- input ---------------------------------------------------------- */

int tui_poll_event(tui_t *t, tui_event_t *ev)
{
    if (!ev) return 0;

    if (g_resized) {
        g_resized = 0;
        if (t) do_resize(t);
        ev->is_special = 1;
        ev->key = TUI_KEY_RESIZE;
        return 1;
    }

    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    if (c == 0x1b) {
        /* could be a bare Esc or the start of an arrow-key sequence.
         * VMIN=0/VTIME=0 means these reads come back immediately even
         * if there's nothing there, so this doesn't block waiting to
         * find out which. */
        unsigned char b1 = 0, b2 = 0;
        ssize_t n1 = read(STDIN_FILENO, &b1, 1);
        ssize_t n2 = (n1 > 0) ? read(STDIN_FILENO, &b2, 1) : 0;

        if (n1 > 0 && n2 > 0 && b1 == '[') {
            ev->is_special = 1;
            switch (b2) {
                case 'A': ev->key = TUI_KEY_UP;    return 1;
                case 'B': ev->key = TUI_KEY_DOWN;  return 1;
                case 'C': ev->key = TUI_KEY_RIGHT; return 1;
                case 'D': ev->key = TUI_KEY_LEFT;  return 1;
            }
        }
        ev->is_special = 0;
        ev->key = 27;
        return 1;
    }

    ev->is_special = 0;
    ev->key = c;
    return 1;
}

void tui_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
