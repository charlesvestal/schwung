# Set Pages in Schwung Manager

The Set Pages editor (`schwung-manager/set_pages.go`) arranges Sets on the seven
inactive pages; the loaded page is read-only. It reads and writes the same
`set_pages/` files as `src/host/shadow_set_pages.c` and does not touch
`UserLibrary/Sets/`.

## Locking with the device

Both sides take an exclusive `flock` on `schwung/.set-pages.lock`:

- The device's switch thread takes it before it publishes the new page and
  holds it until `set_pages/current_page.txt` is written.
- The manager takes it for a load and for the whole of a save.

Without that shared lock, a manager save that started during the device's
D-Bus save window still saw the target page as inactive: it moved the page's
Sets into `.manager-edit-*` while the device was restoring the same page, and
the next page change rewrote `xattrs.txt` without them.

The manager refuses a load or save while the device holds the lock, and the
device waits (in its background thread, never the MIDI thread) for a manager
save. A save whose revision no longer matches is rejected with
`pages changed; refresh and try again`.

## Metadata

`xattrs.txt` lines are `SET_ID ATTR VALUE`, matching what
`shadow_set_pages.c` writes. Only `user.song-index` is rewritten when a Set
changes slot; the other four Set-folder attributes are preserved.

Metadata is read per page and is tolerant of what a failed device move leaves
behind:

- A line with no matching Set directory is ignored and dropped when the page's
  `xattrs.txt` is regenerated on save.
- A Set directory with no slot line falls back to the `user.song-index` xattr,
  then to the first free slot on the page, and heals the metadata on save.
- A malformed line is ignored.

## Revision and recovery

The snapshot's revision hashes `current_page.txt` plus file metadata (path,
mode, size, mtime) under each inactive stash, so a draft is rejected after any
change without reading every Set's contents.

A save stages every moved Set in `.manager-edit-*`, writes a journal plus new
`xattrs.txt`/`manifest.txt` files, then swaps the live files. If a move fails,
the manager reverses the moves and removes the staging directory when every
rename succeeds. The directory (and its recovery files) is left only when a
rollback rename failed; `readSetPages` then blocks further edits until it is
resolved.
