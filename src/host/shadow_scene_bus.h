/*
 * shadow_scene_bus.h -- SCENES on the three FX buses the shim hosts itself:
 * Master FX (scope 0) and the two send buses (scopes 1, 2).
 *
 * The chain slots morph inside their own chain host (chain_scene.c), through
 * chain_mod. These buses have no chain host, so this unit does the same job
 * directly: one scene_table_t per bus (scene_morph.h), and a DRIVE per locked
 * (position, param) that captures the knob's base, writes the morphed value
 * with the plugin's own set_param, and writes the base back when the scene
 * stops driving it. Positions are "fx1".."fxN", as the bus keys spell them.
 *
 * The base is tracked LIVE, the way the Master FX LFO tracks it
 * (mfx_lfo_update_base_from_set_param): a user write to a driven parameter
 * becomes its new base and the morph is re-applied over it.
 *
 * Everything here runs on the SPI callback. The plugin structs are reached
 * through an accessor the host binds, so tests/host/test_scene_bus.c can run
 * it against fake plugins.
 */
#ifndef SHADOW_SCENE_BUS_H
#define SHADOW_SCENE_BUS_H

#include <stdint.h>
#include "scene_morph.h"

#define SCENE_BUS_SCOPES 4          /* master_fx, send1, send2, host */
#define SCENE_HOST_SCOPE 3

/*
 * THE HOST SCOPE: settings the shim itself holds, with no plugin behind them.
 *
 *   target "slot1".."slot4"      params volume (0..4), pan (-1..1)
 *   target "send1", "send2"      param  return (0..127); send1 also to_send2
 *   target "mfx_lfo1", "mfx_lfo2" the Master FX LFO fields
 *
 * Pairs carry the module id "host" and are never dormant. They are applied
 * EVERY frame, not on dirty: the host keeps the user's value (the BASE) and a
 * scene only ever supplies an OVERRIDE beside it (`apply(on=1, v)`), so the
 * morph always runs from the live base and a save never sees the scene. When
 * a pair stops contributing it is switched off (`apply(on=0)`).
 */
#define SCENE_HOST_MODULE "host"
typedef struct {
    int  (*get)(const char *target, const char *param, float *out);   /* the BASE */
    void (*apply)(const char *target, const char *param, int on, float v);
    /* What is APPLIED right now (the override), 1 if one is on. Diagnostics
     * only -- the `scenes:driven` read. */
    int  (*peek)(const char *target, const char *param, float *out);
} scene_host_io_t;
void shadow_scene_host_bind(const scene_host_io_t *io);
/* The edit arm for a host setting: 1 when consumed as a lock. */
int  shadow_scene_host_edit_write(const char *target, const char *param, const char *val);
/* A write about to land on a host setting (call BEFORE it lands, so the old
 * value is still readable): if a scene drives it, the LIVE TAKEOVER anchors
 * it at the fader (scene_morph.h). */
void shadow_scene_host_note_write(const char *target, const char *param, const char *val);
/* Armed and locked: the lock, else -1 (the host answers its base). */
int  shadow_scene_host_read(const char *target, const char *param, char *buf, int len);

/* Plugin access, abstracted so a test can fake it without the shim. */
typedef struct {
    void *(*slot_at)(int scope, int pos);             /* opaque slot or NULL */
    int   (*positions)(int scope);                     /* how many positions */
    const char *(*module_id)(void *slot);              /* "" when empty */
    const char *(*chain_params)(void *slot);           /* JSON array or NULL */
    int   (*get_param)(void *slot, const char *key, char *buf, int len);
    void  (*set_param)(void *slot, const char *key, const char *val);
} scene_bus_io_t;

void shadow_scene_bus_bind(const scene_bus_io_t *io);
void shadow_scene_bus_reset(void);

/* "master_fx" -> 0, "send1" -> 1, "send2" -> 2, "host" -> 3, else -1. */
int  shadow_scene_bus_scope(const char *prefix, int prefix_len);

/* The table verbs ("lock", "unlock", "clear", "copy", "load"); 1 = handled. */
int  shadow_scene_bus_set_verb(int scope, const char *verb, const char *val);
/* "dump", "count", "rev", "locks", "driven"; bytes written, or -1.
 * "driven" is a DIAGNOSTIC measured at the destination: one line per param a
 * scene drives right now, "<target> <param> <value>", the value asked of the
 * PLUGIN itself (every other read of a driven param answers the knob, by
 * design) -- or, in the host scope, the override the host has applied. */
int  shadow_scene_bus_get_verb(int scope, const char *verb, char *buf, int len);

/* The edit arm: 1 when the write was consumed as a lock (pos is 0-based). */
int  shadow_scene_bus_edit_write(int scope, int pos, const char *param, const char *val);
/* A plain read of a DRIVEN param answers the lock while armed, else the base.
 * Bytes written, or -1 to fall through to the plugin. */
int  shadow_scene_bus_read(int scope, int pos, const char *param, char *buf, int len);
/* `<param>:base` -- the KNOB, armed or not: what a SAVE must read. The plain
 * read answers the lock while armed (for the knob on screen), and a save that
 * shares its key records the lock as the user's value. -1 = not driven, ask
 * the plugin (which then holds the knob). */
int  shadow_scene_bus_read_base(int scope, int pos, const char *param, char *buf, int len);
/* A write that reached the plugin: if the param is driven, it is the new base.
 * A BULK write (scene_write_is_bulk: state / preset / load) re-captures every
 * base at that position from the plugin instead: the drives are dropped
 * WITHOUT writing and re-engage next tick from what the load put there. */
void shadow_scene_bus_note_write(int scope, int pos, const char *param, const char *val);

/* Around a `state` read of one position: the knob's base goes into the
 * plugin for the read and the morph is put back after, so a save records what
 * the user set rather than where the fader was. */
void shadow_scene_bus_state_begin(int scope, int pos);
void shadow_scene_bus_state_end(int scope, int pos);

/* Once per frame, with the fader already slewed. */
void shadow_scene_bus_tick(uint8_t a, uint8_t b, float x, uint8_t edit, uint8_t edit_flags);
uint16_t shadow_scene_bus_rev(void);
uint8_t  shadow_scene_bus_take_flash(void);

/* chain_params JSON: one param's shape. Exposed for the test. */
typedef struct {
    int kind;          /* SCENE_KIND_* */
    float min, max, def;
    int option_count;
} scene_bus_meta_t;
int  scene_bus_param_meta(const char *json, const char *param, scene_bus_meta_t *out);
/* An enum value by option NAME -> index, or -1. */
int  scene_bus_option_index(const char *json, const char *param, const char *name);
/* A host setting's shape; 0 when (target, param) is not one. */
int  scene_host_meta(const char *target, const char *param, scene_bus_meta_t *out);

#endif
