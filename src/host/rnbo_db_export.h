/* rnbo_db_export.h — Export RNBO Runner sets from sqlite DB as pack directories */
#ifndef RNBO_DB_EXPORT_H
#define RNBO_DB_EXPORT_H

/* Reads the RNBO Runner sqlite DB and generates pack directories
 * (info.json + set JSON) for each saved set. Skips sets that are
 * already up-to-date (mtime check). No-op if DB doesn't exist. */
void export_rnbo_runner_sets(const char *packs_path);

#endif
