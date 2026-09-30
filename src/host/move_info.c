#define _GNU_SOURCE
/*
 * move_info.c -- publishes move_info.h's snapshot from the live song model.
 *
 * Written on the model READER thread (SCHED_OTHER, cores 0-2), from the tick
 * hook, so nothing here runs on the SPI callback except the export's copy.
 * The segment is created on the first publish; until then the export answers
 * 0 ("this Schwung does not provide it"), which is also the truthful answer
 * on a firmware the model cannot resolve.
 */
#define MOVE_INFO_NO_READER
#include "move_info_pub.h"

#include <pthread.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MOVE_INFO_SHM_PATH   /* tests point it at a plain file */
#define MOVE_INFO_SHM_PATH "/dev/shm/schwung-move-info"
#endif

static move_info_shm_t *g_shm;
static int g_shm_failed;
static move_info_t g_last;   /* what was last written, for `changes` */
/* Two writers: the reader thread publishes, the shim worker marks a stalled
 * model not-live. Both SCHED_OTHER, so a plain mutex; the RT side never
 * takes it (the export is a lock-free seqlock copy). */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static move_info_shm_t *shm_map(void)
{
    if (g_shm || g_shm_failed) return g_shm;
    int fd = open(MOVE_INFO_SHM_PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { g_shm_failed = 1; return NULL; }
    if (ftruncate(fd, sizeof(move_info_shm_t)) != 0) { close(fd); g_shm_failed = 1; return NULL; }
    void *p = mmap(NULL, sizeof(move_info_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { g_shm_failed = 1; return NULL; }
    g_shm = (move_info_shm_t *)p;
    /* A segment left by an older build is rewritten whole below; the magic
     * goes in LAST so a reader never sees it over a half-built struct. */
    return g_shm;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    memset(dst + n, 0, cap - n);
}

void move_info_build(const move_model_t *m, int live, move_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    o->version = MOVE_INFO_VERSION;
    o->valid = (uint8_t)(live && m && m->valid);
    o->playing = o->metronome_on = o->midi_clock_sync = o->input_monitoring = 255;
    o->root_note = o->selected_track = o->global_quant = -1;
    o->groove = -1.0f;
    o->master_db = -1000.0f;
    o->song_beats = -1.0;
    for (int t = 0; t < MOVE_INFO_TRACKS; t++) {
        move_info_track_t *T = &o->track[t];
        T->color_id = -1; T->type = -1;
        T->muted = T->soloed = T->selected = 255;
        T->volume_db = -1000.0f;
    }
    if (!o->valid) return;

    if (m->clock_valid) {
        o->playing = m->playing ? 1 : 0;
        o->song_beats = m->song_beats;
    }
    o->metronome_on = m->metronome_on ? 1 : 0;
    o->midi_clock_sync = m->clock_sync;
    o->input_monitoring = m->input_monitor;
    if (m->root_note >= 0 && m->root_note < 12) o->root_note = (int8_t)m->root_note;
    if (m->selected_track >= 0 && m->selected_track < MOVE_INFO_TRACKS) o->selected_track = (int8_t)m->selected_track;
    if (m->global_quant >= 0 && m->global_quant < 127) {
        o->global_quant = (int8_t)m->global_quant;
        const char *qn = move_model_quant_name(m->global_quant);
        if (qn) copy_str(o->global_quant_name, sizeof o->global_quant_name, qn);
    }
    if (m->ts_upper > 0 && m->ts_upper < 256) o->ts_upper = (uint8_t)m->ts_upper;
    if (m->ts_lower > 0 && m->ts_lower < 256) o->ts_lower = (uint8_t)m->ts_lower;
    if (m->tempo > 0 && isfinite(m->tempo)) o->tempo = (float)m->tempo;
    if (m->groove >= 0 && isfinite(m->groove)) o->groove = (float)m->groove;
    if (m->master_valid && isfinite(m->master_db)) o->master_db = (float)m->master_db;
    copy_str(o->scale, sizeof o->scale, m->scale);
    for (int t = 0; t < MOVE_INFO_TRACKS && t < MM_TRACKS; t++) {
        const mm_track_t *S = &m->track[t];
        move_info_track_t *T = &o->track[t];
        copy_str(T->name, sizeof T->name, S->name);
        if (S->color_id >= 0 && S->color_id < 32768) T->color_id = (int16_t)S->color_id;
        if (S->type >= 0 && S->type < 127) T->type = (int8_t)S->type;
        T->selected = S->selected ? 1 : 0;
        if (S->mixer_valid) {
            T->muted = S->muted ? 1 : 0;
            T->soloed = S->soloed ? 1 : 0;
            if (isfinite(S->volume)) T->volume_db = (float)S->volume;
        }
    }
}

/* Everything but the moving clock counts as a change. */
static int differs(const move_info_t *a, const move_info_t *b)
{
    move_info_t x = *a, y = *b;
    x.changes = y.changes = 0;
    x.song_beats = y.song_beats = 0;
    return memcmp(&x, &y, sizeof x) != 0;
}

static void publish_locked(const move_model_t *m, int live)
{
    move_info_shm_t *shm = shm_map();
    if (!shm) return;
    move_info_t o;
    move_info_build(m, live, &o);
    const int first = memcmp(shm->magic, MOVE_INFO_MAGIC, sizeof MOVE_INFO_MAGIC) != 0;
    o.changes = g_last.changes + ((first || differs(&o, &g_last)) ? 1 : 0);
    if (!first && !differs(&o, &g_last) && o.song_beats == g_last.song_beats) return;
    g_last = o;
    uint32_t s = __atomic_load_n(&shm->seq, __ATOMIC_RELAXED);
    if (s & 1) s++;                       /* a writer that died mid-write */
    __atomic_store_n(&shm->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(&shm->info, &o, sizeof o);
    __atomic_store_n(&shm->seq, s + 2, __ATOMIC_RELEASE);
    if (first) memcpy(shm->magic, MOVE_INFO_MAGIC, sizeof MOVE_INFO_MAGIC);
}

void move_info_publish(const move_model_t *m, int live)
{
    pthread_mutex_lock(&g_mu);
    publish_locked(m, live);
    pthread_mutex_unlock(&g_mu);
}

/* The model stopped being read: say so, keep the last values out of reach. */
void move_info_set_live(int live)
{
    pthread_mutex_lock(&g_mu);
    if (!live && g_shm && g_last.valid) publish_locked(NULL, 0);
    pthread_mutex_unlock(&g_mu);
}

/* THE EXPORT modules find with dlsym(RTLD_DEFAULT, "schwung_move_info").
 * A copy out of a page the shim already mapped: no syscalls, RT-safe. */
__attribute__((visibility("default")))
int schwung_move_info(move_info_t *out, size_t cap)
{
    if (!g_shm) return 0;
    return move_info_copy(g_shm, out, cap);
}
