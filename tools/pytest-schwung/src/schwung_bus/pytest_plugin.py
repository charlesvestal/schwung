"""Pytest entry point for pytest-schwung.

Fixtures:
  ``bus``                  session-scoped SchwungBus, connected and
                           ping-validated. Auto-skips collected tests if
                           the daemon is unreachable.
  ``fresh_move``           L2 reset: trigger restart-move.sh, wait for shim.
                           ~3 s. Same set, transient state cleared.
  ``pristine_set``         L2+ function-scoped: overwrite Move's
                           test-template Song.abl with the repo's
                           canonical empty version, then restart-move.
                           ~3 s per test. Relies on a session-scoped
                           xattr swap (see ``_template_staged``) so
                           that Move resolves ``currentSongIndex`` to
                           our template UUID without needing to touch
                           Settings.json.
  ``pristine_set_class``   L2+ class-scoped: same as ``pristine_set``
                           but fires once per ``class`` test group.
                           Tests in the same class run in declaration
                           order and share evolving state. Useful for
                           short scenario tests where you don't want
                           to pay 3 s per micro-step. Ordering matters
                           — tests later in the class can depend on
                           setup from earlier tests.
  ``midi_out_capture``     function-scoped MidiOutSession. The fixture
                           subscribes on setup, yields a session handle,
                           unsubscribes on teardown. Tests call
                           ``session.drain()`` to read captured events
                           (multiple times in one test is fine — each
                           drain returns events since the last).
"""

from __future__ import annotations

import json
import shlex
import socket
import uuid
from typing import NamedTuple

import pytest

from .client import SchwungBus, SchwungBusError, MidiOutSession
from .device_files import DeviceFiles, DeviceFilesError
from .pristine_constants import (
    DEVICE_STAGING_PATH,
    REPO_TEMPLATE_PATH,
    TEMPLATE_DISPLAY_NAME,
)


# Where on the device we record the xattr-swap state that the
# pristine fixture must restore on teardown. Persisted across the
# session so a crash mid-test (Ctrl+C, OOM, network loss) can be
# auto-recovered by the next session start.
SETS_ROOT_ON_DEVICE = "/data/UserData/UserLibrary/Sets"
SETTINGS_ON_DEVICE  = "/data/UserData/settings/Settings.json"
RECOVERY_FILE       = "/data/UserData/schwung/_pristine_xattr_recovery.json"


class _Template(NamedTuple):
    """The test set the pristine fixture resets to, resolved at runtime.

    ``uuid`` is discovered (existing set named ``TEMPLATE_DISPLAY_NAME``)
    or freshly minted; ``dir`` / ``song_path`` are derived from it.
    """
    uuid: str
    dir: str        # SETS_ROOT_ON_DEVICE/<uuid>
    song_path: str  # SETS_ROOT_ON_DEVICE/<uuid>/<display name>/Song.abl


def _do_restart(bus, timeout: int = 15) -> None:
    """Trigger restart-move and block until shim recovers.

    Shared by ``fresh_move`` and ``pristine_set`` so the protocol +
    timeout live in one place — when wait_for_shim_ready learns
    about a new freeze pattern, both fixtures get it.
    """
    bus.restart_move()
    bus.wait_for_shim_ready(timeout=timeout)


@pytest.fixture(scope="session")
def bus() -> SchwungBus:
    b = SchwungBus()
    try:
        b.connect()
        b.ping()  # confirm protocol handshake works, not just TCP accept
    except (OSError, socket.timeout, SchwungBusError) as e:
        pytest.skip(
            f"schwung-testd unreachable at {b.host}:{b.port} ({e}). "
            "Start the daemon on Move and tunnel the port."
        )
    yield b
    b.close()


@pytest.fixture
def fresh_move(bus):
    """Restart Move's firmware before this test (~3 s).

    Triggers ``restart-move.sh`` via the shim's restart_move flag —
    SIGTERMs+SIGKILLs the whole Move chain and relaunches fresh. Move
    reloads the same set on startup (currentSongIndex unchanged); this
    fixture resets transient state (active overtake, held modifiers,
    edit-mode position, etc.) but NOT the song content.

    Use this when:
      - your test needs Move's UI in a known reset state
      - you don't care which set is loaded (or you'll position the
        UI yourself after)

    For a fixture that ALSO swaps to a known empty template set, use
    ``pristine_set``.

    Skip ``fresh_move`` for fast in-set tests where undoing your own
    actions suffices (3 s reset × N tests adds up in CI).
    """
    _do_restart(bus)
    yield


@pytest.fixture(scope="session")
def device_files():
    """Persistent SSH connection (ControlMaster) to the Move device.

    One open per session, reused by every fixture / test that needs
    file ops. Avoids ~200 ms SSH handshake on every call — after
    the first ``open()`` subsequent commands round-trip in ~30 ms.

    Skips collected tests if SSH itself is unreachable, so test
    runs on machines that can't ssh to the device degrade
    gracefully rather than fail with a cryptic stack trace.
    """
    dev = DeviceFiles()
    try:
        dev.open()
    except DeviceFilesError as e:
        pytest.skip(
            f"DeviceFiles SSH to {dev.host} unreachable ({e}). "
            "Ensure the device is up and SSH keys are configured."
        )
    yield dev
    dev.close()


def _get_xattr(device_files, path: str, name: str) -> str | None:
    """Read a Linux extended file attribute via SSH. Returns the value
    string (xattrs on the device are stored as ASCII ints/colors), or
    None if the attribute doesn't exist or path is missing.
    """
    res = device_files.run(
        f"getfattr -n {shlex.quote(name)} --only-values {shlex.quote(path)} 2>/dev/null",
        check=False,
    )
    if res.returncode != 0:
        return None
    return res.stdout


def _set_xattr(device_files, path: str, name: str, value: str) -> None:
    """Write a Linux extended file attribute via SSH."""
    device_files.run(
        f"setfattr -n {shlex.quote(name)} -v {shlex.quote(value)} {shlex.quote(path)}"
    )


def _find_uuid_at_index(device_files, target_index: str) -> str | None:
    """Find which set's directory has user.song-index = ``target_index``.

    Move resolves ``currentSongIndex`` (in Settings.json) by
    linear-scanning all set directories and picking the one whose
    ``user.song-index`` xattr matches. Returns that set's UUID, or
    None if no set currently claims that index (which would make Move
    fall back to a default song on the next load).

    Raises if MULTIPLE sets claim the same index — that's corrupt
    state (we don't know which Move would actually pick) and silently
    operating on one of them would leave a duplicate after teardown.
    """
    cmd = (
        f'for d in {SETS_ROOT_ON_DEVICE}/*/; do '
        f'  uuid=$(basename "$d"); '
        f'  idx=$(getfattr -n user.song-index --only-values "$d" 2>/dev/null); '
        f'  if [ "$idx" = {shlex.quote(target_index)} ]; then echo "$uuid"; fi; '
        f'done'
    )
    res = device_files.run(cmd)
    lines = [l for l in res.stdout.strip().split('\n') if l]
    if len(lines) > 1:
        raise RuntimeError(
            f"multiple sets claim user.song-index={target_index!r}: {lines}. "
            "Device xattr state is corrupt — fix manually before running tests."
        )
    return lines[0] if lines else None


def _all_song_indices(device_files) -> set[int]:
    """Return the set of currently-used user.song-index values across
    all set directories. Used to pick a safe sentinel value that's
    guaranteed not to collide with any existing set.
    """
    cmd = (
        f'for d in {SETS_ROOT_ON_DEVICE}/*/; do '
        f'  getfattr -n user.song-index --only-values "$d" 2>/dev/null; '
        f'  echo; '
        f'done'
    )
    res = device_files.run(cmd)
    indices: set[int] = set()
    for line in res.stdout.split('\n'):
        line = line.strip()
        if line.isdigit():
            indices.add(int(line))
    return indices


def _pick_sentinel_index(device_files) -> str:
    """Pick a user.song-index value not currently in use by any set.

    Used when the template has no xattr at all and we need to give
    the displaced "holder" set a non-conflicting temporary value.
    Returns one past the max — avoids collisions with any current
    set, even ones at high indices (we saw 30 on real hardware).
    """
    used = _all_song_indices(device_files)
    return str((max(used) + 1) if used else 1000)


def _find_template_uuid(device_files) -> str | None:
    """Find our dedicated test set by its display name. Returns the
    set's UUID, or None if no such set exists on the device.

    A Move set lives at ``Sets/<uuid>/<display name>/Song.abl``; we
    match the ``<display name>`` directory and read the UUID one level
    up. ``TEMPLATE_DISPLAY_NAME`` is ours and won't collide with the
    user's sets, so a name match identifies it unambiguously. Raises if
    two sets somehow share the name (ambiguous which one Move would
    load).
    """
    cmd = (
        f"find {shlex.quote(SETS_ROOT_ON_DEVICE)} -mindepth 2 -maxdepth 2 "
        f"-type d -name {shlex.quote(TEMPLATE_DISPLAY_NAME)}"
    )
    res = device_files.run(cmd, check=False)
    uuids: list[str] = []
    for line in res.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("/")
        # .../Sets/<uuid>/<display name>  -> uuid is second-to-last
        if len(parts) >= 2:
            uuids.append(parts[-2])
    uuids = sorted(set(uuids))
    if len(uuids) > 1:
        raise DeviceFilesError(
            f"multiple sets named {TEMPLATE_DISPLAY_NAME!r} on device: "
            f"{uuids}. Delete the extras in Move's set list and re-run."
        )
    return uuids[0] if uuids else None


def _create_template_set(device_files) -> str:
    """Mint a fresh test set from the staged canonical Song.abl.

    Generates a new UUID, lays down ``Sets/<uuid>/<display name>/
    Song.abl`` (copied from the on-device staging path — no scp to a
    spaced remote path), and stamps the set-metadata xattrs Move
    expects so the set shows up as a normal entry in its list. The
    ``user.song-index`` xattr is seeded with a free sentinel; the
    caller's swap assigns the real index. Returns the new UUID.

    Assumes the canonical Song.abl is already staged at
    ``DEVICE_STAGING_PATH`` (``_template_staged`` does that first).
    """
    new_uuid = str(uuid.uuid4())
    set_dir = f"{SETS_ROOT_ON_DEVICE}/{new_uuid}"
    inner = f"{set_dir}/{TEMPLATE_DISPLAY_NAME}"
    device_files.run(f"mkdir -p {shlex.quote(inner)}")
    device_files.run(
        f"cp {shlex.quote(DEVICE_STAGING_PATH)} "
        f"{shlex.quote(inner + '/Song.abl')}"
    )
    # Metadata xattrs so Move treats it as an ordinary set (matching
    # the shape Move writes on every set dir). song-index starts at a
    # free sentinel; the swap in _template_staged sets the real value.
    ts = device_files.run("date -u +%Y-%m-%dT%H:%M:%SZ").stdout.strip()
    _set_xattr(device_files, set_dir, "user.song-index",
               _pick_sentinel_index(device_files))
    _set_xattr(device_files, set_dir, "user.last-modified-time", ts)
    _set_xattr(device_files, set_dir, "user.local-cloud-state", "notSynced")
    _set_xattr(device_files, set_dir, "user.song-color", "3")
    _set_xattr(device_files, set_dir, "user.was-externally-modified", "false")
    return new_uuid


@pytest.fixture(scope="session")
def _template_staged(device_files):
    """Stage the repo's canonical empty_song.abl onto Move once
    per session AND swap the user.song-index xattrs so Move's current
    ``currentSongIndex`` (in Settings.json) resolves to our template.

    Why the xattr swap (the part that makes pristine_set actually work):
      Move's song-list is built by scanning ``Sets/*/`` and reading
      each directory's ``user.song-index`` extended attribute (ext4
      xattr). It then matches that against ``currentSongIndex`` from
      Settings.json. Without the swap, our template (with whatever
      xattr it happened to be assigned at creation) is invisible to
      Move at the index Settings.json points to — Move just loads
      whatever set already had the matching xattr.

      We perform a SWAP: our template gets the xattr value Move is
      looking for; the set that previously had that value gets our
      template's old value. Other sets are untouched. Settings.json
      is untouched. On teardown the swap reverses and the user's
      device is byte-identical to its pre-session state.

    Crash recovery: the swap state is written to
    ``/data/UserData/schwung/_pristine_xattr_recovery.json``. If the
    session crashes and leaves the swap in place, the next session
    detects the recovery file and restores it BEFORE applying the
    new swap. So an interrupted session is auto-healed on next run.
    """
    if not REPO_TEMPLATE_PATH.is_file():
        pytest.skip(
            f"repo template missing: {REPO_TEMPLATE_PATH}. "
            "Re-capture an empty Move set into tests/fixtures/."
        )

    # Stage the canonical Song.abl once (per-test cp from this path is
    # local on device, ~30 ms). Staged before find-or-create so a newly
    # minted set can cp from here instead of scp'ing to a spaced path.
    device_files.put_file(REPO_TEMPLATE_PATH, DEVICE_STAGING_PATH)

    # Find our dedicated test set by display name, or create it from the
    # staged template; the UUID is resolved at runtime.
    template_uuid = _find_template_uuid(device_files)
    if template_uuid is None:
        template_uuid = _create_template_set(device_files)
    template = _Template(
        uuid=template_uuid,
        dir=f"{SETS_ROOT_ON_DEVICE}/{template_uuid}",
        song_path=(f"{SETS_ROOT_ON_DEVICE}/{template_uuid}/"
                   f"{TEMPLATE_DISPLAY_NAME}/Song.abl"),
    )

    # --- Recover from a previous crashed session, if any. ---
    if device_files.file_exists(RECOVERY_FILE):
        prev = None
        try:
            prev = json.loads(device_files.read_text(RECOVERY_FILE))
        except (json.JSONDecodeError, ValueError):
            # Corrupt recovery file (truncated write, partial flush
            # during crash). Without delete, every subsequent session
            # would fail the same way. Drop it and proceed fresh.
            prev = None

        if prev and prev.get("template_uuid") == template.uuid:
            try:
                orig_template_xattr = prev.get("template_xattr_orig") or ""
                if orig_template_xattr:
                    _set_xattr(device_files, template.dir,
                               "user.song-index", orig_template_xattr)
                else:
                    device_files.run(
                        f"setfattr -x user.song-index "
                        f"{shlex.quote(template.dir)} 2>/dev/null",
                        check=False,
                    )
                holder_uuid = prev.get("holder_uuid")
                if holder_uuid and holder_uuid != template.uuid:
                    holder_dir = f"{SETS_ROOT_ON_DEVICE}/{holder_uuid}"
                    if device_files.file_exists(holder_dir):
                        _set_xattr(device_files, holder_dir,
                                   "user.song-index",
                                   prev.get("holder_xattr_orig", ""))
            except Exception:
                # Best-effort. If something stale (holder no longer
                # exists, etc.) we still want to clean up the
                # recovery file so we don't loop forever.
                pass

        device_files.run(f"rm -f {shlex.quote(RECOVERY_FILE)}")

    # --- Read current state. ---
    settings = json.loads(device_files.read_text(SETTINGS_ON_DEVICE))
    current_index = str(settings["currentSongIndex"])
    template_xattr_orig = _get_xattr(device_files, template.dir,
                                     "user.song-index") or ""

    # If template already has the right xattr, no swap needed. The
    # session yields without writing a recovery file or running a
    # restore — there's nothing to restore. Assumes
    # ``currentSongIndex`` doesn't change mid-session (no other
    # process editing Settings.json). If it does, subsequent
    # pristine_set calls keep restarting Move at the new index,
    # which won't point to our template anymore. That's an external
    # mutation we don't try to defend against.
    if template_xattr_orig == current_index:
        yield template
        return

    holder_uuid = _find_uuid_at_index(device_files, current_index)
    holder_xattr_orig = current_index if holder_uuid else None
    holder_dir = (f"{SETS_ROOT_ON_DEVICE}/{holder_uuid}"
                  if holder_uuid and holder_uuid != template.uuid else None)

    # --- Persist recovery state BEFORE mutating. ---
    # If we crash between this write and the swap completion, next
    # session reads this file and restores. printf+stdin redirect
    # avoids heredoc quoting ambiguity and works regardless of how
    # device_files.run dispatches to the remote shell.
    recovery = {
        "template_uuid":        template.uuid,
        "template_xattr_orig":  template_xattr_orig,
        "holder_uuid":          holder_uuid,
        "holder_xattr_orig":    holder_xattr_orig,
        "current_index":        current_index,
    }
    recovery_json = json.dumps(recovery, indent=2)
    device_files.run(
        f"printf '%s' {shlex.quote(recovery_json)} > {shlex.quote(RECOVERY_FILE)}"
    )

    # --- Swap: holder gets template's old value, template gets index. ---
    if holder_dir:
        # If template_xattr_orig is empty (template never had the
        # xattr), pick a sentinel guaranteed not to collide with any
        # other set's existing xattr. Naive value like "999" could
        # collide with a real set; _pick_sentinel_index scans all
        # current values and returns one past the max.
        new_holder_value = template_xattr_orig or _pick_sentinel_index(device_files)
        _set_xattr(device_files, holder_dir, "user.song-index", new_holder_value)
    _set_xattr(device_files, template.dir, "user.song-index", current_index)

    try:
        yield template
    finally:
        # --- Restore. ---
        try:
            if holder_dir:
                _set_xattr(device_files, holder_dir,
                           "user.song-index", current_index)
            if template_xattr_orig:
                _set_xattr(device_files, template.dir,
                           "user.song-index", template_xattr_orig)
            else:
                device_files.run(
                    f"setfattr -x user.song-index "
                    f"{shlex.quote(template.dir)} 2>/dev/null",
                    check=False,
                )
            device_files.run(f"rm -f {shlex.quote(RECOVERY_FILE)}")
        except Exception:
            # Teardown must never raise; the recovery file stays in
            # place for next session to pick up.
            pass


def _apply_template_and_restart(bus, device_files, template) -> None:
    """Body shared by pristine_set (function-scoped) and
    pristine_set_class (class-scoped). One place that knows the cp
    + restart protocol; if a future change wants different ordering
    or extra verification, edit here once.

    ``template`` is the ``_Template`` yielded by ``_template_staged``.
    Both paths are quoted: the staging path is fixed, but the target
    Song.abl lives under the display-name dir, which contains a space.
    """
    device_files.run(
        f"cp {shlex.quote(DEVICE_STAGING_PATH)} {shlex.quote(template.song_path)}"
    )
    _do_restart(bus)


@pytest.fixture
def pristine_set(bus, device_files, _template_staged):
    """Reset Move to the canonical empty test template before this test.

    Per test:
      1. ssh cp staging-path → template Song.abl (local on device, ~30 ms)
      2. restart_move() → shim picks up the flag, kills + relaunches Move
      3. wait_for_shim_ready() → blocks through the freeze + thaw cycle

    Total cost: ~3 s (dominated by the restart-move cycle; the cp itself
    is negligible).

    Scope coupling: function-scoped, depends on ``bus`` (session),
    ``device_files`` (session), ``_template_staged`` (session). If
    ``bus`` is ever narrowed to function scope, the session-scoped
    SSH connection in ``device_files`` keeps working — but be careful
    that ``_template_staged`` still runs before the first per-test
    use of ``pristine_set``.

    No teardown — the next test that needs a pristine set will
    overwrite the file again anyway. Tests that want strict cleanup
    should undo their own actions explicitly. If
    ``restart_move()`` raises mid-fixture (network hiccup, daemon
    crash), the device is left with the template Song.abl already in
    place; the next pristine_set re-`cp`s and re-restarts, so the
    contamination is at worst one test's ordering artifact, not a
    hard break.

    Use this when your test would otherwise depend on whatever pattern
    / instruments the user happened to leave loaded — e.g. step LED
    tests that need known dim baseline, pad LED tests that need
    notes-off starting state. For fast tests where undoing your own
    actions suffices, prefer that; pristine_set adds ~3 s per test.

    For grouped tests that can share a pristine starting state and
    just need to run in order, use ``pristine_set_class`` instead.

    How Move's reload reaches our template (since 2026-05-18): the
    session-scoped ``_template_staged`` fixture also swaps the
    ``user.song-index`` xattr on Move's filesystem so the value of
    ``currentSongIndex`` in Settings.json resolves to our template
    UUID — see ``_template_staged`` docs.
    """
    _apply_template_and_restart(bus, device_files, _template_staged)
    yield


@pytest.fixture(scope="class")
def pristine_set_class(bus, device_files, _template_staged):
    """Class-scoped variant of ``pristine_set``: one cp + restart per
    test class instead of per test.

    Use when you have several short tests that all want a common
    pristine starting point and don't mutually interfere — paying 3 s
    once instead of N × 3 s.

    Tests in the class run in **declaration order** (pytest preserves
    file order within a class). Tests may rely on state set up by
    earlier tests in the same class — that's the explicit deal here.
    If two tests in a class need independent pristine state, they
    belong in two separate classes.

    Example::

        class TestStepPatternBuildup:
            def test_initially_empty(self, bus, pristine_set_class):
                # snapshot is the dim baseline
                ...

            def test_step_1_toggles_on(self, bus, pristine_set_class):
                # ... select track 1, toggle step 1, assert change ...

            def test_step_5_toggles_on_alongside_step_1(self, bus,
                                                       pristine_set_class):
                # step 1 is still toggled on from the previous test;
                # this test toggles step 5 ON TOP of that state.
                ...

    Scope coupling: class-scoped, shares the session-scoped ``bus``,
    ``device_files``, ``_template_staged``. The cp + restart runs at
    the start of the first test in the class; no teardown between
    tests in the class. Move's state evolves freely until the next
    class's ``pristine_set_class`` fires.
    """
    _apply_template_and_restart(bus, device_files, _template_staged)
    yield


@pytest.fixture
def midi_out_capture(bus) -> MidiOutSession:
    """Subscribe to MIDI_OUT events for the duration of one test.

    Yields a MidiOutSession. Call ``session.drain()`` (or the equivalent
    shorter ``session()``) to read events captured since the last drain
    (or since subscribe). The fixture handles unsubscribe on teardown,
    so failing tests don't leak the subscription into the next test.

    Typical use::

        def test_no_stuck_notes(bus, midi_out_capture):
            bus.press_pad(84); bus.wait_frame(8)
            bus.release_pad(84); bus.wait_frame(8)
            cap = midi_out_capture.drain()
            assert len(cap.filter(kind="note_off")) >= len(cap.filter(kind="note_on"))
    """
    bus.subscribe_midi_out()
    try:
        yield MidiOutSession(bus)
    finally:
        try:
            bus.unsubscribe_midi_out()
        except Exception:
            pass
