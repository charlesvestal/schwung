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
    uint64_t t_confirm;           /* post keeps settling for EF_SETTLE_MS after this */
} jrn_t;
#define EF_SETTLE_MS 300
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
                       uint64_t t_ms, ef_cmd_fn cmd, void *ctx);
static void journal_settle(const move_model_t *now, uint64_t t_ms);

void edit_follow_tick(const move_model_t *now, const ef_notes_t *now_notes, const ef_notes_t *prev_notes,
                      uint64_t t_ms, ef_cmd_fn cmd, void *ctx)
{
    /* An intent can arrive AFTER the change it describes was published (the
     * press, Move's edit and our tick race); the edited clip's previous state
     * is kept until its next change, so checking on every tick is safe. */
    if (now && now->valid) journal_settle(now, t_ms);
    if (now && now->valid && cmd) do_intents(now, now_notes, prev_notes, t_ms, cmd, ctx);
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

static void journal_add(const ef_intent_t *in, uint32_t jid, uint32_t post, uint64_t t_ms)
{
    /* A new edit after undos kills the redo branch, as in Move -- whose
     * history is the whole document's, not one clip's. */
    for (int i = 0; i < EF_JOURNAL; i++)
        if (g_jrn[i].jid && g_jrn[i].undone) g_jrn[i].dead = 1;
    jrn_t *j = &g_jrn[jid % EF_JOURNAL];
    memset(j, 0, sizeof *j);
    j->jid = jid;
    j->track = in->track;
    j->slot = in->slot;
    j->clip_id = in->clip_id;
    j->pre = in->pre_hash;
    j->post = post;
    j->t_confirm = t_ms;
}

/* A journaled clip, wherever Move has it now: by identity on its track. */
static const mm_clip_t *clip_by_id(const move_model_t *m, int t, uint64_t id)
{
    if (t < 0 || t >= MM_TRACKS) return NULL;
    for (int s = 0; s < MM_SLOTS; s++)
        if (m->track[t].slot[s].exists && m->track[t].slot[s].clip_id == id) return &m->track[t].slot[s];
    return NULL;
}

/* Move lands a paste's notes and its own envelopes a tick or two apart, so
 * the "after" state keeps following the clip for a moment after the
 * confirmation. */
static void journal_settle(const move_model_t *now, uint64_t t_ms)
{
    for (int i = 0; i < EF_JOURNAL; i++) {
        jrn_t *j = &g_jrn[i];
        if (!j->jid || j->undone || j->dead || t_ms - j->t_confirm > EF_SETTLE_MS) continue;
        const mm_clip_t *c = clip_by_id(now, j->track, j->clip_id);
        if (c) j->post = mm_clip_state_hash(c);
    }
}

static void do_intents(const move_model_t *now, const ef_notes_t *nn, const ef_notes_t *pn,
                       uint64_t t_ms, ef_cmd_fn cmd, void *ctx)
{
    char v[192];
    for (int i = 0; i < EF_PENDING; i++) {
        pend_t *p = &g_pend[i];
        if (!p->used) continue;
        const ef_intent_t *in = &p->in;
        if (in->kind == EF_UNDO || in->kind == EF_REDO) {
            /* MOVE'S UNDO IS THE WHOLE DOCUMENT'S, not the clip on screen:
             * it can revert a paste on another track, or on a clip that is no
             * longer the selected one. So an Undo is matched against EVERY
             * journaled paste -- the one whose clip now sits exactly at its
             * pre-paste state (Redo: at its post-paste state) is the one Move
             * reverted. Nothing matching yet keeps waiting (Move may take two
             * ticks); nothing ever matching expires, which is Move undoing
             * something that was not a mirrored paste. */
            const int redo = (in->kind == EF_REDO);
            jrn_t *best = NULL;
            for (int k = 0; k < EF_JOURNAL; k++) {
                jrn_t *j = &g_jrn[k];
                if (!j->jid || j->dead || j->undone != redo) continue;
                const mm_clip_t *jc = clip_by_id(now, j->track, j->clip_id);
                if (!jc || mm_clip_state_hash(jc) != (redo ? j->post : j->pre)) continue;
                if (!best || (redo ? j->jid < best->jid : j->jid > best->jid)) best = j;
            }
            if (!best) continue;
            best->undone = !redo;
            snprintf(v, sizeof v, "%s %u", redo ? "redo" : "undo", best->jid);
            cmd(ctx, best->track, "lanes:journal", v);
            if (redo) g_stats.redone++; else g_stats.undone++;
            p->used = 0;
            continue;
        }
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
            journal_add(in, jid, state, t_ms);
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
                journal_add(in, jid, state, t_ms);
                g_stats.pasted++;
            } else {
                g_stats.declined++;
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
    if (!now || !now->valid || !cmd) return;
    if (!prev->valid || now->doc_gen != prev->doc_gen) {
        /* A different document: Move's undo history is gone, and so is ours. */
        memset(g_pend, 0, sizeof g_pend);
        memset(g_jrn, 0, sizeof g_jrn);
        memset(g_stash, 0, sizeof g_stash);
        return;
    }
    do_clips(now, prev, cmd, ctx);
    journal_settle(now, t_ms);
    do_intents(now, now_notes, prev_notes, t_ms, cmd, ctx);
}
