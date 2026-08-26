/* spi_tally.c — see spi_tally.h. Pure: no I/O, no allocation, no locks. */

#include "spi_tally.h"

#include <stdio.h>
#include <string.h>

spi_tally_t shim_spi_tally;

void spi_tally_record(spi_tally_t *t, uint32_t tx_ns)
{
    if (!t || tx_ns == 0u) return;
    t->frames++;
    t->tx_ns_sum += tx_ns;
    t->tx_ns_last = tx_ns;
    if (tx_ns > t->tx_ns_max) t->tx_ns_max = tx_ns;
}

void spi_tally_reset(spi_tally_t *t, spi_tally_state_t *s)
{
    if (t) {
        t->frames = 0;
        t->tx_ns_sum = 0;
        t->tx_ns_max = 0;
        t->tx_ns_last = 0;
    }
    if (s) memset(s, 0, sizeof(*s));
}

void spi_tally_fold(spi_tally_state_t *s, const spi_tally_t *t, uint32_t irqs,
                    spi_tally_sample_t *out)
{
    if (!s || !t || !out) return;

    /* Read the producer's counters ONCE each. They are cumulative and only
     * ever rise, so a frame landing between these loads shifts one sample into
     * the next window rather than corrupting either. */
    uint64_t frames    = t->frames;
    uint64_t tx_ns_sum = t->tx_ns_sum;

    memset(out, 0, sizeof(*out));
    out->max_ns       = t->tx_ns_max;
    out->backlog      = s->backlog;
    out->backlog_peak = s->backlog_peak;

    if (!s->have_prev) {
        out->first = 1;
        s->prev_frames    = frames;
        s->prev_tx_ns_sum = tx_ns_sum;
        s->prev_irqs      = irqs;
        s->have_prev      = 1;
        return;
    }

    out->frames = frames - s->prev_frames;
    /* Deliberately a 32-bit subtraction of 32-bit operands: that is what makes
     * it modular, and modular is what survives the /proc counter's sign flip
     * at 2^31 and its wrap at 2^32. Do NOT widen either side. See spi_tally.h. */
    out->irqs   = irqs - s->prev_irqs;

    uint64_t ns_delta = tx_ns_sum - s->prev_tx_ns_sum;
    if (out->frames > 0) out->avg_ns = (uint32_t)(ns_delta / out->frames);

    out->headroom_ns = (out->avg_ns < SPI_TALLY_PERIOD_NS)
                     ? (SPI_TALLY_PERIOD_NS - out->avg_ns) : 0u;

    /* An IRQ that fired while we were busy is one the semaphore queued. Only a
     * POSITIVE difference is news: irqs < frames just means the window
     * boundaries caught a queued frame being worked off, which is the backlog
     * draining, not going negative. */
    if ((uint64_t)out->irqs > out->frames) {
        out->late = (uint32_t)((uint64_t)out->irqs - out->frames);
        s->backlog += out->late;
        if (out->late > s->backlog_peak) s->backlog_peak = out->late;
    }

    out->backlog      = s->backlog;
    out->backlog_peak = s->backlog_peak;

    s->prev_frames    = frames;
    s->prev_tx_ns_sum = tx_ns_sum;
    s->prev_irqs      = irqs;
}

int spi_tally_format(const spi_tally_sample_t *s, char *buf, int len)
{
    if (!s || !buf || len <= 0) return 0;
    if (s->first)
        return snprintf(buf, (size_t)len, "spi-tally: armed — baseline taken");

    return snprintf(buf, (size_t)len,
                    "spi-tally: %llu frames / %u irq  tx avg %uus (max %uus)  "
                    "headroom %uus  backlog %llu",
                    (unsigned long long)s->frames, s->irqs,
                    s->avg_ns / 1000u, s->max_ns / 1000u,
                    s->headroom_ns / 1000u,
                    (unsigned long long)s->backlog);
}

int spi_tally_format_late(const spi_tally_sample_t *s, char *buf, int len)
{
    if (!s || !buf || len <= 0) return 0;
    return snprintf(buf, (size_t)len,
                    "spi-tally: LATE %u irq(s) arrived while busy — "
                    "frames queued, not dropped (backlog %llu, worst window %llu)",
                    s->late,
                    (unsigned long long)s->backlog,
                    (unsigned long long)s->backlog_peak);
}
