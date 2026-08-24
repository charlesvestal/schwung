#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_thread_audit.h"

/* Field numbers in proc(5), 1-based, counting the bracketed comm as field 2. */
#define FIELD_UTIME     14
#define FIELD_STIME     15
#define FIELD_PROCESSOR 39
#define FIELD_RT_PRIO   40
#define FIELD_POLICY    41

int rt_thread_parse_stat(const char *line, rt_thread_info_t *out)
{
    if (!line || !out) return 0;

    memset(out, 0, sizeof(*out));

    out->tid = atoi(line);
    if (out->tid <= 0) return 0;

    /* comm is bracketed and may contain spaces AND parentheses — "Audio
     * Main/SPI" has the first, a name like "worker (2)" has both. Delimit on
     * the LAST ')', never on whitespace. */
    const char *open = strchr(line, '(');
    const char *close = open ? strrchr(open, ')') : NULL;
    if (!open || !close || close < open) return 0;

    int comm_len = (int)(close - open - 1);
    if (comm_len < 0) return 0;
    if (comm_len > RT_AUDIT_COMM_LEN - 1) comm_len = RT_AUDIT_COMM_LEN - 1;
    memcpy(out->comm, open + 1, (size_t)comm_len);
    out->comm[comm_len] = '\0';

    /* Everything after the ')' is field 3 onward, all whitespace-separated. */
    const char *p = close + 1;
    int field = 2;                 /* we have just consumed comm */
    int got_cpu = 0, got_prio = 0, got_policy = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        field++;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;

        if (field == FIELD_UTIME)      out->utime = strtoul(tok, NULL, 10);
        else if (field == FIELD_STIME) out->stime = strtoul(tok, NULL, 10);
        else if (field == FIELD_PROCESSOR) { out->cpu = atoi(tok);    got_cpu = 1; }
        else if (field == FIELD_RT_PRIO) { out->rtprio = atoi(tok); got_prio = 1; }
        else if (field == FIELD_POLICY)  { out->policy = atoi(tok); got_policy = 1; break; }
    }

    /* A line that stops before field 41 tells us nothing about scheduling, and
     * reporting it as SCHED_OTHER 0 would be a false all-clear. */
    if (!got_cpu || !got_prio || !got_policy) return 0;

    return 1;
}

int rt_thread_is_realtime(const rt_thread_info_t *t)
{
    if (!t) return 0;
    if (t->policy != RT_AUDIT_SCHED_FIFO && t->policy != RT_AUDIT_SCHED_RR)
        return 0;
    return t->rtprio > 0;
}

int rt_thread_count_realtime(const rt_thread_info_t *t, int n)
{
    if (!t || n <= 0) return 0;
    int c = 0;
    for (int i = 0; i < n; i++)
        if (rt_thread_is_realtime(&t[i])) c++;
    return c;
}

int rt_thread_new_realtime(const rt_thread_info_t *prev, int prev_n,
                           const rt_thread_info_t *cur, int cur_n,
                           rt_thread_info_t *out, int out_max)
{
    if (!cur || cur_n <= 0 || !out || out_max <= 0) return 0;
    if (prev_n < 0) prev_n = 0;

    int written = 0;
    for (int i = 0; i < cur_n && written < out_max; i++) {
        if (!rt_thread_is_realtime(&cur[i])) continue;

        int seen = 0;
        for (int j = 0; j < prev_n; j++) {
            if (prev && prev[j].tid == cur[i].tid) { seen = 1; break; }
        }
        if (seen) continue;

        out[written++] = cur[i];
    }
    return written;
}

int rt_thread_format(const rt_thread_info_t *t, const char *module,
                     char *buf, int buf_len)
{
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';
    if (!t) return 0;

    const char *pol = (t->policy == RT_AUDIT_SCHED_FIFO) ? "FIFO"
                    : (t->policy == RT_AUDIT_SCHED_RR)   ? "RR"
                    : "OTHER";

    /* Describe the thread; the CALLER says whether it is a baseline entry or a
     * new arrival, because only the caller knows.
     *
     * The name is reported and never interpreted. An earlier version appended
     * "name inherited — NOT the SPI thread" whenever comm was "Audio Main/SPI",
     * which was a conclusion drawn from a name match — the exact reasoning this
     * tool exists to avoid — and measurement showed it was wrong: Move runs SIX
     * threads under that name as its own, none of them inherited from us. All
     * the name can honestly carry is that it does not identify anything. */
    int shared_name = (strcmp(t->comm, "Audio Main/SPI") == 0);

    int n = snprintf(buf, (size_t)buf_len,
                     "tid=%d %s:%d cpu=%d comm=\"%s\"%s%s%s",
                     t->tid, pol, t->rtprio, t->cpu, t->comm,
                     module ? " after loading " : "",
                     module ? module : "",
                     shared_name ? " (shared name — Move uses it too; identify by tid)" : "");

    if (n < 0) { buf[0] = '\0'; return 0; }
    if (n >= buf_len) n = buf_len - 1;
    return n;
}

static int burn_find(const rt_thread_info_t *a, int n, int tid)
{
    for (int i = 0; i < n; i++)
        if (a[i].tid == tid) return i;
    return -1;
}

int rt_thread_burners(const rt_thread_info_t *base, int base_n,
                      const rt_thread_info_t *prev, int prev_n,
                      const rt_thread_info_t *cur, int cur_n,
                      int hz, int min_ms,
                      rt_thread_burn_t *out, int out_max)
{
    if (!cur || cur_n <= 0 || !out || out_max <= 0) return 0;
    if (hz <= 0) return 0;
    if (base_n < 0) base_n = 0;
    if (prev_n < 0) prev_n = 0;

    int written = 0;
    for (int i = 0; i < cur_n; i++) {
        if (!rt_thread_is_realtime(&cur[i])) continue;

        /* Move's own audio threads are permanently busy at FIFO 70; they are
         * the environment, not the finding. */
        if (base && burn_find(base, base_n, cur[i].tid) >= 0) continue;

        int pi = prev ? burn_find(prev, prev_n, cur[i].tid) : -1;
        unsigned long before = (pi >= 0) ? (prev[pi].utime + prev[pi].stime) : 0;
        unsigned long now = cur[i].utime + cur[i].stime;

        /* A tid absent from prev is brand new; count its whole lifetime so a
         * thread that does all its damage in its first second is not missed. */
        if (now < before) continue;                 /* tid reuse — do not guess */
        unsigned long ticks = now - before;

        int ms = (int)((ticks * 1000UL) / (unsigned long)hz);
        if (ms < min_ms) continue;

        /* Insertion sort, worst first: the list is at most a handful and the
         * top entry is the one anybody reads. */
        int at = written;
        while (at > 0 && out[at - 1].cpu_ms < ms) at--;
        if (written < out_max) written++;
        for (int k = (written < out_max ? written : out_max) - 1; k > at; k--)
            out[k] = out[k - 1];
        if (at < out_max) {
            out[at].thread = cur[i];
            out[at].cpu_ms = ms;
        }
    }
    return written;
}

int rt_thread_format_burn(const rt_thread_burn_t *b, const char *module,
                          int window_ms, char *buf, int buf_len)
{
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';
    if (!b) return 0;

    char who[192];
    rt_thread_format(&b->thread, module, who, sizeof(who));

    /* Percent of the window is the number that maps onto the harm: `Link Main`
     * runs at FIFO 35 and only gets the CPU this thread does not take. */
    int pct = (window_ms > 0) ? (int)((b->cpu_ms * 100) / window_ms) : 0;

    int n = snprintf(buf, (size_t)buf_len, "%s BURNED %d ms (%d%% of %d ms)",
                     who, b->cpu_ms, pct, window_ms);
    if (n < 0) { buf[0] = '\0'; return 0; }
    if (n >= buf_len) n = buf_len - 1;
    return n;
}

int rt_thread_audit_scan(rt_thread_info_t *out, int out_max)
{
    if (!out || out_max <= 0) return -1;

    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < out_max) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;

        char path[128];
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;   /* thread exited between readdir and open */

        char line[1024];
        if (fgets(line, sizeof(line), f) && rt_thread_parse_stat(line, &out[n]))
            n++;

        fclose(f);
    }
    closedir(d);

    return n;
}
