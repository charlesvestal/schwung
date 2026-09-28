/* edit_follow.c -- see edit_follow.h. */
#include "edit_follow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { int used; ef_intent_t in; } pend_t;
typedef struct {
    uint32_t jid;                 /* 0 = empty */
    int      track, slot;
    uint64_t clip_id;
    uint32_t pre, post;           /* clip content before / after the paste */
    int      undone, dead;
} jrn_t;
typedef struct { uint32_t sid; int track; uint64_t clip_id; } stash_t;

static pend_t   g_pend[EF_PENDING];
static jrn_t    g_jrn[EF_JOURNAL];
static stash_t  g_stash[EF_STASH];
static uint32_t g_next_jid = 1, g_next_sid = 1;
static ef_stats_t g_stats;

void edit_follow_reset(void)
{
    memset(g_pend, 0, sizeof g_pend);
    memset(g_jrn, 0, sizeof g_jrn);
    memset(g_stash, 0, sizeof g_stash);
    memset(&g_stats, 0, sizeof g_stats);
}

ef_stats_t edit_follow_stats(void) { return g_stats; }

void edit_follow_intent(const ef_intent_t *in)
{
    for (int i = 0; i < EF_PENDING; i++)
        if (!g_pend[i].used) { g_pend[i].used = 1; g_pend[i].in = *in; return; }
    /* Full: drop the oldest -- intents only live a few hundred ms anyway. */
    int old = 0;
    for (int i = 1; i < EF_PENDING; i++) if (g_pend[i].in.t_ms < g_pend[old].in.t_ms) old = i;
    g_pend[old].in = *in;
}

static void do_intents(const move_model_t *now, const ef_notes_t *nn, const ef_notes_t *pn,
                       ef_cmd_fn cmd, void *ctx);

void edit_follow_tick(const move_model_t *now, const ef_notes_t *now_notes, const ef_notes_t *prev_notes,
                      uint64_t t_ms, ef_cmd_fn cmd, void *ctx)
{
    /* An intent can arrive AFTER the change it describes was published (the
     * press, Move's edit and our tick race); the edited clip's previous state
     * is kept until its next change, so checking on every tick is safe. */
    if (now && now->valid && cmd) do_intents(now, now_notes, prev_notes, cmd, ctx);
    for (int i = 0; i < EF_PENDING; i++)
        if (g_pend[i].used && t_ms - g_pend[i].in.t_ms > EF_TIMEOUT_MS) {
            g_pend[i].used = 0;
            g_stats.expired++;
        }
}

static int in_span(double t, double lo, double len) { return t >= lo - 1e-6 && t < lo + len - 1e-6; }

int edit_follow_is_paste(const ef_notes_t *pre, const ef_notes_t *post, double src, double dst, double len)
{
    if (!pre || !post || pre->n < 0 || post->n < 0) return 0;
    int appeared = 0;
    for (int i = 0; i < post->n; i++) {
        const mm_note_t *a = &post->notes[i];
        int known = 0;
        for (int j = 0; j < pre->n && !known; j++) known = (pre->notes[j].id == a->id);
        if (known) continue;                         /* not new */
        if (!in_span(a->start, dst, len)) return 0;  /* a new note elsewhere: some other edit */
        int matched = 0;
        for (int j = 0; j < pre->n && !matched; j++) {
            const mm_note_t *s = &pre->notes[j];
            matched = s->pitch == a->pitch && in_span(s->start, src, len) &&
                      fabs((s->start - src) - (a->start - dst)) < 1e-6 &&
                      fabs(s->dur - a->dur) < 1e-6 && fabs(s->vel - a->vel) < 1e-3;
        }
        if (!matched) return 0;
        appeared++;
    }
    return appeared > 0 ? 1 : -1;                    /* -1: nothing new yet */
}

/* " v=36,38": the distinct pitches of the notes that APPEARED -- which, on a
 * drum rack, are the voices Move pasted (it copies only the selected one's).
 * The chain decides whether that scopes anything; on a melodic track it does
 * not, and the paste stays whole-step. Empty when nothing appeared. */
static void appeared_pitches(const ef_notes_t *pre, const ef_notes_t *post, char *out, size_t cap)
{
    int seen[128] = {0}, n = 0;
    size_t w = 0;
    out[0] = '\0';
    for (int i = 0; post && pre && i < post->n; i++) {
        const mm_note_t *a = &post->notes[i];
        int known = 0;
        for (int j = 0; j < pre->n && !known; j++) known = (pre->notes[j].id == a->id);
        if (known || a->pitch < 0 || a->pitch > 127 || seen[a->pitch]) continue;
        seen[a->pitch] = 1;
        int k = snprintf(out + w, cap - w, "%s%d", n ? "," : " v=", a->pitch);
        if (k < 0 || (size_t)k >= cap - w) { out[0] = '\0'; return; }   /* all or nothing */
        w += (size_t)k;
        n++;
    }
}

static const mm_clip_t *clip_of(const move_model_t *m, int t, int s)
{
    if (t < 0 || t >= MM_TRACKS || s < 0 || s >= MM_SLOTS) return NULL;
    return m->track[t].slot[s].exists ? &m->track[t].slot[s] : NULL;
}

static void journal_add(const ef_intent_t *in, uint32_t jid, uint32_t post)
{
    /* A new edit after undos kills the redo branch, as in Move. */
    for (int i = 0; i < EF_JOURNAL; i++)
        if (g_jrn[i].jid && g_jrn[i].clip_id == in->clip_id && g_jrn[i].undone) g_jrn[i].dead = 1;
    jrn_t *j = &g_jrn[jid % EF_JOURNAL];
    memset(j, 0, sizeof *j);
    j->jid = jid;
    j->track = in->track;
    j->slot = in->slot;
    j->clip_id = in->clip_id;
    j->pre = in->pre_hash;
    j->post = post;
}

/* The latest live entry for the clip (Undo's candidate), or the earliest
 * undone one after it (Redo's). */
static jrn_t *journal_pick(uint64_t clip_id, int for_redo)
{
    jrn_t *best = NULL;
    for (int i = 0; i < EF_JOURNAL; i++) {
        jrn_t *j = &g_jrn[i];
        if (!j->jid || j->dead || j->clip_id != clip_id) continue;
        if (!for_redo && !j->undone && (!best || j->jid > best->jid)) best = j;
        if (for_redo && j->undone && (!best || j->jid < best->jid)) best = j;
    }
    return best;
}

static void do_intents(const move_model_t *now, const ef_notes_t *nn, const ef_notes_t *pn,
                       ef_cmd_fn cmd, void *ctx)
{
    char v[192];
    for (int i = 0; i < EF_PENDING; i++) {
        pend_t *p = &g_pend[i];
        if (!p->used) continue;
        const ef_intent_t *in = &p->in;
        const mm_clip_t *c = clip_of(now, in->track, in->slot);
        if (!c || c->clip_id != in->clip_id) { p->used = 0; continue; }   /* the clip went */
        const uint32_t state = mm_clip_state_hash(c);
        if (state == in->pre_hash) continue;                              /* not yet */

        if (in->kind == EF_DOUBLE) {
            /* Confirmed by geometry: the loop is now twice as long. */
            if (fabs(c->loop_end - (in->dst + in->len)) > 1e-6 || fabs(c->loop_start - in->src) > 1e-6) continue;
            uint32_t jid = g_next_jid++;
            snprintf(v, sizeof v, "%d %d %.9g %.9g %.9g %u", in->track, in->slot,
                     in->src, in->dst, in->len, jid);
            cmd(ctx, in->track, "lanes:paste_span", v);
            journal_add(in, jid, state);
            g_stats.pasted++;
        } else if (in->kind == EF_PASTE) {
            /* Judged on the note ids that APPEARED between the edited clip's
             * previous content state and now. Not on an exact pre-hash: Move
             * may land the notes and its own automation in two ticks, and the
             * source span is untouched by a paste either way. */
            int verdict = 0;
            if (nn && pn && nn->ref.valid && pn->ref.valid &&
                nn->ref.clip_id == in->clip_id && pn->ref.clip_id == in->clip_id)
                verdict = edit_follow_is_paste(pn, nn, in->src, in->dst, in->len);
            if (verdict < 0) continue;                 /* only the automation moved so far */
            if (verdict > 0) {
                uint32_t jid = g_next_jid++;
                char voices[96];   /* + ~40 of span fields < MMS_CMD_VAL; a list that overflows is dropped whole (unscoped) */
                appeared_pitches(pn, nn, voices, sizeof voices);
                snprintf(v, sizeof v, "%d %d %.9g %.9g %.9g %u%s", in->track, in->slot,
                         in->src, in->dst, in->len, jid, voices);
                cmd(ctx, in->track, "lanes:paste_span", v);
                journal_add(in, jid, state);
                g_stats.pasted++;
            } else {
                g_stats.declined++;
            }
        } else {
            /* Undo/Redo land when the clip reaches EXACTLY the state on the
             * other side of a mirrored paste -- which may take Move two ticks
             * (notes, then its envelopes), so a partial state keeps waiting. */
            const int redo = (in->kind == EF_REDO);
            jrn_t *j = journal_pick(in->clip_id, redo);
            if (j && !redo && state == j->pre) {
                j->post = in->pre_hash;               /* the settled post-paste state */
                j->undone = 1;
                snprintf(v, sizeof v, "undo %u", j->jid);
                cmd(ctx, in->track, "lanes:journal", v);
                g_stats.undone++;
            } else if (j && redo && state == j->post) {
                j->undone = 0;
                snprintf(v, sizeof v, "redo %u", j->jid);
                cmd(ctx, in->track, "lanes:journal", v);
                g_stats.redone++;
            } else {
                continue;                             /* not (yet) this clip's undo */
            }
        }
        p->used = 0;
    }
}

static int same_content(const mm_clip_t *x, const mm_clip_t *y)
{
    return x->notes_len > 0 && x->notes_len == y->notes_len && x->notes_hash == y->notes_hash &&
           x->region_start == y->region_start && x->region_end == y->region_end &&
           x->loop_start == y->loop_start && x->loop_end == y->loop_end && x->loop_on == y->loop_on;
}

static void do_clips(const move_model_t *now, const move_model_t *prev, ef_cmd_fn cmd, void *ctx)
{
    char v[64];
    for (int t = 0; t < MM_TRACKS; t++) {
        for (int s = 0; s < MM_SLOTS; s++) {
            const mm_clip_t *a = &prev->track[t].slot[s], *b = &now->track[t].slot[s];
            const int gone = a->exists && (!b->exists || b->clip_id != a->clip_id);
            const int came = b->exists && (!a->exists || b->clip_id != a->clip_id);
            if (gone) {
                uint32_t sid = g_next_sid++;
                stash_t *st = &g_stash[sid % EF_STASH];
                st->sid = sid;
                st->track = t;
                st->clip_id = a->clip_id;
                snprintf(v, sizeof v, "%d %d %u", t, s, sid);
                cmd(ctx, t, "lanes:stash", v);
                g_stats.stashed++;
            }
            if (!came) continue;
            int restored = 0;
            for (int k = 0; k < EF_STASH && !restored; k++) {
                stash_t *st = &g_stash[k];
                if (!st->sid || st->track != t || st->clip_id != b->clip_id) continue;
                snprintf(v, sizeof v, "%u %d %d", st->sid, t, s);
                cmd(ctx, t, "lanes:unstash", v);
                memset(st, 0, sizeof *st);
                g_stats.restored++;
                restored = 1;
            }
            if (restored) continue;
            for (int src = 0; src < MM_SLOTS; src++) {
                const mm_clip_t *p = &prev->track[t].slot[src];
                if (src == s || !p->exists || !same_content(p, b)) continue;
                snprintf(v, sizeof v, "%d %d", src, s);
                cmd(ctx, t, "lanes:copy_clip", v);
                g_stats.copied++;
                break;
            }
        }
    }
}

void edit_follow_on_change(const move_model_t *now, const move_model_t *prev,
                           const ef_notes_t *now_notes, const ef_notes_t *prev_notes,
                           uint64_t t_ms, ef_cmd_fn cmd, void *ctx)
{
    (void)t_ms;
    if (!now || !now->valid || !cmd) return;
    if (!prev->valid || now->doc_gen != prev->doc_gen) {
        /* A different document: Move's undo history is gone, and so is ours. */
        memset(g_pend, 0, sizeof g_pend);
        memset(g_jrn, 0, sizeof g_jrn);
        memset(g_stash, 0, sizeof g_stash);
        return;
    }
    do_clips(now, prev, cmd, ctx);
    do_intents(now, now_notes, prev_notes, cmd, ctx);
}
