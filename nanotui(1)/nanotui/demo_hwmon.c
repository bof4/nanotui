/*
 * demo_hwmon.c - little hardware monitor built on top of nanotui.
 *
 * Reads CPU usage and memory from /proc, which means this demo only
 * runs on Linux (the lib itself doesn't care, it's just termios/ioctl
 * and works on any POSIX box - it's this file specifically that's
 * tied to /proc). Porting the stats-gathering to macOS would mean
 * swapping in host_statistics()/sysctl calls instead, didn't bother.
 *
 * q or Esc to quit.
 */
#define _POSIX_C_SOURCE 200809L

#include "nanotui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CORES 64
#define HISTORY_LEN 200

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_jiffies_t;

static int read_cpu_stats(cpu_jiffies_t *total, cpu_jiffies_t *per_core, int *ncores)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[256];
    int n = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0)
            break; /* cpu lines are always first, stop once they end */

        cpu_jiffies_t j = {0};
        char tag[16];
        int matched;

        if (line[3] == ' ') {
            /* aggregate "cpu  ..." line */
            matched = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                              tag, &j.user, &j.nice, &j.system, &j.idle,
                              &j.iowait, &j.irq, &j.softirq, &j.steal);
            if (matched >= 5) *total = j;
        } else {
            /* per-core "cpuN ..." line */
            if (n >= MAX_CORES) continue;
            matched = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                              tag, &j.user, &j.nice, &j.system, &j.idle,
                              &j.iowait, &j.irq, &j.softirq, &j.steal);
            if (matched >= 5) {
                per_core[n] = j;
                n++;
            }
        }
    }
    fclose(f);
    *ncores = n;
    return 0;
}

static double pct_busy(const cpu_jiffies_t *prev, const cpu_jiffies_t *cur)
{
    unsigned long long pidle = prev->idle + prev->iowait;
    unsigned long long cidle = cur->idle + cur->iowait;
    unsigned long long ptotal = prev->user + prev->nice + prev->system + pidle + prev->irq + prev->softirq + prev->steal;
    unsigned long long ctotal = cur->user + cur->nice + cur->system + cidle + cur->irq + cur->softirq + cur->steal;

    long long dtotal = (long long)(ctotal - ptotal);
    long long didle  = (long long)(cidle - pidle);
    if (dtotal <= 0) return 0.0;

    double busy = (double)(dtotal - didle) / (double)dtotal;
    if (busy < 0) busy = 0;
    if (busy > 1) busy = 1;
    return busy;
}

/* returns 0..1 used fraction, or -1 on failure */
static double read_mem_usage(unsigned long *total_kb, unsigned long *avail_kb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    unsigned long t = 0, a = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &t) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &a) == 1) continue;
    }
    fclose(f);
    if (t == 0) return -1;

    *total_kb = t;
    *avail_kb = a;
    return 1.0 - ((double)a / (double)t);
}

static void read_loadavg(double *out3)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) { out3[0] = out3[1] = out3[2] = 0; return; }
    if (fscanf(f, "%lf %lf %lf", &out3[0], &out3[1], &out3[2]) != 3) {
        out3[0] = out3[1] = out3[2] = 0;
    }
    fclose(f);
}

/* green under 60%, yellow under 85%, red above - same thresholds
 * htop/btop use, no point reinventing them */
static int color_for_load(double pct)
{
    if (pct < 0.60) return TUI_GREEN;
    if (pct < 0.85) return TUI_YELLOW;
    return TUI_RED;
}

int main(void)
{
    tui_t *t = tui_init();
    if (!t) {
        fprintf(stderr, "not running in a terminal, nothing to do here\n");
        return 1;
    }

    cpu_jiffies_t prev_total = {0}, cur_total = {0};
    cpu_jiffies_t prev_core[MAX_CORES] = {0}, cur_core[MAX_CORES] = {0};
    int ncores = 0;

    read_cpu_stats(&prev_total, prev_core, &ncores);

    static double history[HISTORY_LEN];
    int hist_n = 0;

    int running = 1;
    while (running) {
        read_cpu_stats(&cur_total, cur_core, &ncores);

        double overall = pct_busy(&prev_total, &cur_total);
        double core_pct[MAX_CORES];
        for (int i = 0; i < ncores; i++)
            core_pct[i] = pct_busy(&prev_core[i], &cur_core[i]);

        prev_total = cur_total;
        memcpy(prev_core, cur_core, sizeof(cpu_jiffies_t) * (size_t)ncores);

        if (hist_n < HISTORY_LEN) {
            history[hist_n++] = overall;
        } else {
            memmove(history, history + 1, sizeof(double) * (HISTORY_LEN - 1));
            history[HISTORY_LEN - 1] = overall;
        }

        unsigned long mem_total = 0, mem_avail = 0;
        double mem_pct = read_mem_usage(&mem_total, &mem_avail);
        if (mem_pct < 0) mem_pct = 0;

        double load[3];
        read_loadavg(load);

        int W = tui_width(t);
        int H = tui_height(t);

        tui_clear(t);
        tui_box(t, 0, 0, W, H, "nanotui hardware monitor  (q to quit)", TUI_CYAN);

        int inner_x = 2;
        int inner_w = W - 4;
        int y = 2;

        tui_textf(t, inner_x, y, TUI_WHITE, TUI_DEFAULT, "load avg: %.2f  %.2f  %.2f", load[0], load[1], load[2]);
        y += 2;

        /* per-core gauges, two columns if there's room */
        int cols = (inner_w >= 60 && ncores > 4) ? 2 : 1;
        int col_w = (cols == 2) ? (inner_w - 2) / 2 : inner_w;
        int rows_per_col = (ncores + cols - 1) / cols;

        for (int i = 0; i < ncores; i++) {
            int col = i / rows_per_col;
            int row = i % rows_per_col;
            int gx = inner_x + col * (col_w + 2);
            int gy = y + row;
            if (gy >= H - 1) continue;

            char label[16];
            snprintf(label, sizeof(label), "core%d", i);
            tui_gauge(t, gx, gy, col_w, core_pct[i], color_for_load(core_pct[i]), label);
        }
        y += rows_per_col + 1;

        tui_text(t, inner_x, y, "overall cpu:", TUI_WHITE, TUI_DEFAULT, TUI_ATTR_NONE);
        y += 1;
        tui_gauge(t, inner_x, y, inner_w, overall, color_for_load(overall), NULL);
        y += 2;

        tui_text(t, inner_x, y, "history:", TUI_WHITE, TUI_DEFAULT, TUI_ATTR_NONE);
        y += 1;
        int chart_h = H - y - 4;
        if (chart_h > 1) {
            tui_chart(t, inner_x, y, inner_w, chart_h, history, hist_n, TUI_GREEN);
            y += chart_h + 1;
        }

        tui_text(t, inner_x, y, "memory:", TUI_WHITE, TUI_DEFAULT, TUI_ATTR_NONE);
        y += 1;
        char memlabel[64];
        snprintf(memlabel, sizeof(memlabel), "%lu / %lu MB", (mem_total - mem_avail) / 1024, mem_total / 1024);
        tui_gauge(t, inner_x, y, inner_w, mem_pct, color_for_load(mem_pct), memlabel);

        tui_flush(t);

        /* poll for a quit key for ~400ms total, checking every 20ms so
         * the keypress feels responsive instead of laggy */
        for (int waited = 0; waited < 400; waited += 20) {
            tui_event_t ev;
            while (tui_poll_event(t, &ev)) {
                if (!ev.is_special && (ev.key == 'q' || ev.key == 27)) {
                    running = 0;
                }
            }
            if (!running) break;
            tui_sleep_ms(20);
        }
    }

    tui_shutdown(t);
    return 0;
}
