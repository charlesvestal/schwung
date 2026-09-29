/* move_info_pub.h -- the shim side of move_info.h (the publisher). Internal. */
#ifndef MOVE_INFO_PUB_H
#define MOVE_INFO_PUB_H
#include "move_info.h"
#include "move_model.h"

/* Fill `o` from a model snapshot; `live` 0 (or m NULL) gives all-unknown. */
void move_info_build(const move_model_t *m, int live, move_info_t *o);
/* Write the segment if anything changed. Reader thread (SCHED_OTHER). */
void move_info_publish(const move_model_t *m, int live);
/* A model that stopped being read publishes valid = 0. Shim worker. */
void move_info_set_live(int live);
/* The export modules dlsym. RT-safe copy. */
int schwung_move_info(move_info_t *out, size_t cap);
#endif
