"""TCP client for schwung-testd, the on-device test-bus daemon.

Wraps the line protocol (PING / INJECT_MIDI / WAIT_FRAME /
SNAPSHOT_PAD_LEDS / SUBSCRIBE_MIDI_OUT / DUMP midi_out /
UNSUBSCRIBE_MIDI_OUT / QUIT) and exposes both raw primitives and
semantic helpers (press_pad, release_pad, capture_midi_out). The daemon
listens on TCP loopback by default; reach it from a dev machine via
`ssh -L 47777:localhost:47777`.

Sequential, single-connection — matches the Phase 1 daemon, which
accepts one client at a time. Threading and async are out of scope.
"""

from __future__ import annotations

import os
import socket
import time
from dataclasses import dataclass, field
from typing import Optional, List, Tuple

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 47777


class SchwungBusError(RuntimeError):
    """Raised when the daemon returns an ERR response or the protocol breaks."""


@dataclass
class WaitFrameResult:
    counter: int


@dataclass
class BusState:
    """Snapshot of selected ``shadow_control_t`` fields.

    Used as the precondition source for Command-pattern UI tests
    (Phase 3) — commands inspect this to decide whether the system
    is in a valid state for the action they're about to take.

    Fields are coarse on purpose: we extend the daemon's STATE
    response (and the shim's enum, if needed) one field at a time
    as commands need more granularity. Display-diff is never used
    for preconditions — too fragile.
    """
    move_ui_mode: int       # 0=unknown, 1=session, 2=note, 3=set_overview
    overtake_mode: int      # 0=normal, 1=menu, 2=module (schwung overlay)
    shift_held: int         # 0/1 — modifier
    selected_slot: int      # 0..3 — schwung slot focus
    ui_slot: int            # 0..3 — schwung slot for knob routing
    shim_counter: int       # SPI tick counter at moment of probe
    transport_playing: int  # 0=stopped, 1=playing (from overlay, set by MIDI Start/Stop)
    # Fields below default to 0 so older test code (or tests that
    # manually construct a fake state for monkey-patching) keeps
    # working without listing every field. Add new fields here, not
    # above; older daemons answering STATE without these keys produce
    # 0 via the `.get(..., 0)` in `state()`.
    speaker_active: int = 0     # 1=built-in speaker live (no headphones); 0=jack plugged
    line_in_connected: int = 0  # 1=line-in cable plugged; 0=using internal mic
    display_mode: int = 0       # 0=Move-native UI; 1=shadow UI displayed

    # Enum mirrors for readability in tests.
    MOVE_UI_UNKNOWN      = 0
    MOVE_UI_SESSION      = 1
    MOVE_UI_NOTE         = 2
    MOVE_UI_SET_OVERVIEW = 3

    OVERTAKE_NORMAL = 0
    OVERTAKE_MENU   = 1
    OVERTAKE_MODULE = 2

    def in_move_native(self) -> bool:
        """True if Move's own UI is active (not schwung overlay)."""
        return self.overtake_mode == self.OVERTAKE_NORMAL


# Move button -> CC# mapping for bus.tap(). Add new names sparingly;
# this is a stable surface that tests rely on.
_BUTTON_CCS = {
    "jog_click": 3,
    "shift":     49,
    "menu":      50,
    "back":      51,
}


@dataclass
class MidiOutEvent:
    """One USB-MIDI packet observed in Move's MIDI_OUT by the shim."""
    frame: int       # shim_counter at capture time
    cable: int       # 0=internal, 2=external USB
    cin: int         # USB-MIDI Code Index Number (0x9=note-on, 0x8=note-off, 0xB=CC...)
    status: int      # MIDI status byte (0x90, 0x80, 0xB0, ...)
    data1: int       # note / CC#
    data2: int       # velocity / value

    @property
    def channel(self) -> int:
        return self.status & 0x0F

    @property
    def kind(self) -> str:
        """Human-readable event kind. Matches the `status=...` filter values."""
        hi = self.status & 0xF0
        return {
            0x80: "note_off",
            0x90: "note_on",
            0xA0: "aftertouch",
            0xB0: "cc",
            0xC0: "program_change",
            0xD0: "channel_pressure",
            0xE0: "pitch_bend",
            0xF0: "system",
        }.get(hi, "unknown")


@dataclass
class MidiOutCapture:
    """A snapshot of MIDI_OUT events drained from the daemon.

    Returned by `bus.dump_midi_out()`. Supports simple filtering helpers
    so tests read naturally:

        cap = bus.dump_midi_out()
        ons  = cap.filter(kind="note_on")
        offs = cap.filter(kind="note_off", note=60)
        assert len(offs) >= len(ons), "stuck notes"
    """
    events: List[MidiOutEvent] = field(default_factory=list)
    dropped: int = 0  # events that overflowed the ring before we drained

    def __len__(self) -> int:
        return len(self.events)

    def __iter__(self):
        return iter(self.events)

    def filter(
        self,
        kind: Optional[str] = None,
        cable: Optional[int] = None,
        channel: Optional[int] = None,
        note: Optional[int] = None,
        cc: Optional[int] = None,
    ) -> "MidiOutCapture":
        """Return a new capture with only events matching all given keys.

        `note` filters data1 for note-on/off/aftertouch events.
        `cc` filters data1 for control-change events.
        """
        def keep(e: MidiOutEvent) -> bool:
            if kind is not None and e.kind != kind: return False
            if cable is not None and e.cable != cable: return False
            if channel is not None and e.channel != channel: return False
            if note is not None and e.data1 != note: return False
            if cc is not None and e.data1 != cc: return False
            return True
        return MidiOutCapture(events=[e for e in self.events if keep(e)], dropped=0)


class SchwungBus:
    """Synchronous client for one schwung-testd instance.

    Use as a context manager or call ``connect()`` / ``close()`` explicitly.
    """

    def __init__(
        self,
        host: Optional[str] = None,
        port: Optional[int] = None,
        connect_timeout: float = 2.0,
        recv_timeout: float = 35.0,  # > daemon's 30s WAIT_FRAME cap
    ) -> None:
        self.host = host or os.environ.get("SCHWUNG_TEST_HOST", DEFAULT_HOST)
        env_port = os.environ.get("SCHWUNG_TEST_PORT")
        self.port = port if port is not None else (int(env_port) if env_port else DEFAULT_PORT)
        self._connect_timeout = connect_timeout
        self._recv_timeout = recv_timeout
        self._sock: Optional[socket.socket] = None
        self._buf = bytearray()

    def connect(self) -> None:
        if self._sock is not None:
            return
        s = socket.create_connection((self.host, self.port), timeout=self._connect_timeout)
        s.settimeout(self._recv_timeout)
        self._sock = s

    def close(self) -> None:
        if self._sock is None:
            return
        try:
            self._send_line("QUIT")
            self._read_line()  # consume "OK bye"
        except Exception:
            pass
        try:
            self._sock.close()
        finally:
            self._sock = None
            self._buf.clear()

    def __enter__(self) -> "SchwungBus":
        self.connect()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ----- raw protocol primitives ------------------------------------------

    def ping(self) -> str:
        """Return the daemon's identity string (e.g. 'schwung-testd 0.1.0')."""
        return self._request("PING")

    def inject_midi(self, packet: bytes) -> None:
        """Inject one 4-byte USB-MIDI packet into Move's MIDI_IN buffer.

        Packet format: [CIN+cable, status, data1, data2]. Cable nibble is
        the high nibble of byte 0; CIN the low nibble. Cable 0 = internal
        hardware (pads/buttons), cable 2 = external USB.

        Delivery is frame-synchronous: the daemon writes the packet into the
        /schwung-midi-inject ring and the shim drains it on the next SPI
        frame. Sequence ordering across injects is the caller's job — use
        :meth:`wait_frame` between injects that must land in distinct frames.
        """
        if len(packet) != 4:
            raise ValueError(f"INJECT_MIDI expects exactly 4 bytes, got {len(packet)}")
        self._request("INJECT_MIDI " + packet.hex())

    def wait_frame(self, n: int = 1) -> WaitFrameResult:
        """Block until the shim has ticked at least N more SPI frames."""
        if n < 1:
            raise ValueError("wait_frame N must be >= 1")
        line = self._request(f"WAIT_FRAME {n}")
        # Reply is "frame=<counter>" after the OK prefix is stripped
        if not line.startswith("frame="):
            raise SchwungBusError(f"unexpected WAIT_FRAME reply: {line!r}")
        try:
            counter = int(line.split("=", 1)[1])
        except ValueError as e:
            raise SchwungBusError(f"unparseable WAIT_FRAME counter: {line!r}") from e
        if counter < 0:
            raise SchwungBusError(f"negative frame counter: {counter}")
        return WaitFrameResult(counter=counter)

    def snapshot_pad_leds(self) -> bytes:
        """Return the current 32-byte pad LED color snapshot.

        Index 0 = note 68 (track 4 pad A), ..., index 31 = note 99 (track 1
        pad H). Each byte is a Move LED color code (0 = off).
        """
        line = self._request("SNAPSHOT_PAD_LEDS")
        try:
            data = bytes.fromhex(line)
        except ValueError as e:
            raise SchwungBusError(f"bad SNAPSHOT_PAD_LEDS hex: {line!r}") from e
        if len(data) != 32:
            raise SchwungBusError(f"expected 32 LED bytes, got {len(data)}")
        return data

    # snapshot_step_leds() is intentionally absent: the daemon's
    # SNAPSHOT_STEP_LEDS reads a step-LED buffer added by a separate
    # shim feature not yet mainline. It returns with that feature.

    # ----- stream subscriptions -----------------------------------

    def subscribe(self, channel: str) -> None:
        """Start capturing events on the given test-bus channel.

        Channels currently supported: ``midi_out``. Phase 3 will add
        ``midi_in`` and a log-tail channel. Resets the per-subscriber
        baseline so the next ``dump(channel)`` returns events from now.
        """
        self._request(f"SUBSCRIBE {channel}")

    def unsubscribe(self, channel: str) -> None:
        """Stop capturing events on the given channel."""
        self._request(f"UNSUBSCRIBE {channel}")

    def dump(self, channel: str) -> MidiOutCapture:
        """Drain events for ``channel`` since the last subscribe/dump.

        Currently only ``midi_out`` is wired through MidiOutEvent
        parsing; future channels will return their own event type.
        Use the convenience wrapper ``dump_midi_out()`` for clarity.
        """
        if channel != "midi_out":
            raise SchwungBusError(
                f"dump({channel!r}) — only 'midi_out' is implemented in Phase 2; "
                "Phase 3+ will add more channels."
            )
        return self._dump_midi_out_impl()

    def subscribe_midi_out(self) -> None:
        """Convenience: ``subscribe('midi_out')``."""
        self.subscribe("midi_out")

    def unsubscribe_midi_out(self) -> None:
        """Convenience: ``unsubscribe('midi_out')``."""
        self.unsubscribe("midi_out")

    def dump_midi_out(self) -> MidiOutCapture:
        """Convenience: ``dump('midi_out')``."""
        return self._dump_midi_out_impl()

    def _dump_midi_out_impl(self) -> MidiOutCapture:
        if self._sock is None:
            raise SchwungBusError("bus not connected")
        self._send_line("DUMP midi_out")
        header = self._read_line()
        if not (header == "OK" or header.startswith("OK ")):
            if header.startswith("ERR"):
                raise SchwungBusError(header[3:].lstrip() or "ERR (no message)")
            raise SchwungBusError(f"unexpected DUMP reply: {header!r}")

        # Parse "OK count=N dropped=D" — count is required.
        rest = header[2:].lstrip()
        count: Optional[int] = None
        dropped = 0
        for tok in rest.split():
            if tok.startswith("count="):
                try:
                    count = int(tok[6:])
                except ValueError as e:
                    raise SchwungBusError(f"DUMP midi_out bad count token: {tok!r}") from e
            elif tok.startswith("dropped="):
                try:
                    dropped = int(tok[8:])
                except ValueError as e:
                    raise SchwungBusError(f"DUMP midi_out bad dropped token: {tok!r}") from e
        if count is None:
            raise SchwungBusError(f"DUMP midi_out header missing count=: {header!r}")
        if count < 0 or dropped < 0:
            raise SchwungBusError(f"DUMP midi_out negative count/dropped: {header!r}")

        events: List[MidiOutEvent] = []
        for _ in range(count):
            line = self._read_line()
            if not line.startswith("EV "):
                raise SchwungBusError(f"expected EV line, got {line!r}")
            parts = line.split()
            if len(parts) != 3:
                raise SchwungBusError(
                    f"malformed EV line (expected 'EV <frame> <pkt>'): {line!r}"
                )
            _, frame_hex, pkt_hex = parts
            try:
                frame = int(frame_hex, 16)
                pkt = bytes.fromhex(pkt_hex)
            except ValueError as e:
                raise SchwungBusError(f"bad EV hex in {line!r}: {e}") from e
            if len(pkt) != 4:
                raise SchwungBusError(f"EV packet must be 4 bytes, got {len(pkt)}")
            events.append(MidiOutEvent(
                frame=frame,
                cable=(pkt[0] >> 4) & 0x0F,
                cin=pkt[0] & 0x0F,
                status=pkt[1],
                data1=pkt[2],
                data2=pkt[3],
            ))
        end = self._read_line()
        if end != "END":
            raise SchwungBusError(f"expected END, got {end!r}")
        return MidiOutCapture(events=events, dropped=dropped)

    def capture_midi_out(self) -> "MidiOutCaptureContext":
        """Context manager: subscribe on enter, dump on exit.

        Usage::

            with bus.capture_midi_out() as cap:
                bus.press_pad(84)
                bus.wait_frame(8)
            # cap.events populated, subscription closed
        """
        return MidiOutCaptureContext(self)

    # ----- semantic helpers --------------------------------------------------

    def press_pad(self, note: int, velocity: int = 100) -> None:
        """Inject a note-on for a pad (notes 68..99) on cable 0, channel 0."""
        _check_pad_note(note)
        if not 1 <= velocity <= 127:
            raise ValueError("velocity must be 1..127 for note-on")
        # Cable=0 (high nibble), CIN=9 (note-on, low nibble) -> 0x09
        self.inject_midi(bytes([0x09, 0x90, note, velocity]))

    def release_pad(self, note: int, velocity: int = 0x40) -> None:
        """Inject a note-off for a pad on cable 0, channel 0.

        ``velocity`` is the release velocity (0..127). Default 0x40 matches
        the standard "no release-velocity sensor" value; pass a real
        velocity if the test exercises a release-velocity-aware module.
        """
        _check_pad_note(note)
        if not 0 <= velocity <= 127:
            raise ValueError("release velocity must be 0..127")
        # CIN=8 (note-off), status 0x80
        self.inject_midi(bytes([0x08, 0x80, note, velocity]))

    def pad_index(self, note: int) -> int:
        """Convert a pad note (68..99) to its pad_led_colors index (0..31)."""
        _check_pad_note(note)
        return note - 68

    def press_step(self, note: int, velocity: int = 100) -> None:
        """Inject a note-on for a sequencer step pad (notes 16..31)
        on cable 0, channel 0. Use in note-edit mode to toggle steps.
        """
        _check_step_note(note)
        if not 1 <= velocity <= 127:
            raise ValueError("velocity must be 1..127 for note-on")
        self.inject_midi(bytes([0x09, 0x90, note, velocity]))

    def release_step(self, note: int, velocity: int = 0x40) -> None:
        """Inject a note-off for a sequencer step pad on cable 0, channel 0."""
        _check_step_note(note)
        if not 0 <= velocity <= 127:
            raise ValueError("release velocity must be 0..127")
        self.inject_midi(bytes([0x08, 0x80, note, velocity]))

    def step_index(self, note: int) -> int:
        """Convert a step note (16..31) to its step_led_colors index (0..15)."""
        _check_step_note(note)
        return note - 16

    # ----- state probe (Phase 3, used as command preconditions) -------------

    def state(self) -> BusState:
        """One-shot snapshot of shim/control state for preconditions.

        Read-only, no side effects. Extend the daemon's STATE response
        (and this parser) one field at a time as new commands need
        more precondition granularity. Fields added after v0.1 default
        to 0 if the daemon is older — tests that depend on them should
        skip when the field is missing rather than crash.
        """
        line = self._request("STATE")
        fields: dict[str, int] = {}
        for tok in line.split():
            if "=" not in tok:
                raise SchwungBusError(f"STATE: malformed token {tok!r}")
            k, v = tok.split("=", 1)
            try:
                fields[k] = int(v)
            except ValueError as e:
                raise SchwungBusError(f"STATE: non-integer {k}={v!r}") from e
        try:
            return BusState(
                move_ui_mode=fields["move_ui_mode"],
                overtake_mode=fields["overtake_mode"],
                shift_held=fields["shift_held"],
                selected_slot=fields["selected_slot"],
                ui_slot=fields["ui_slot"],
                shim_counter=fields["shim_counter"],
                transport_playing=fields["transport_playing"],
                # Defaults so an older daemon still produces a valid
                # BusState; tests gating on these fields should also
                # check ``state.X is not None`` if we ever switch to
                # Optional[int]. For now zeros are safe sentinels —
                # invariant tests assert state transitions, so a stuck-
                # zero from an old daemon fails loudly with "didn't flip".
                speaker_active=fields.get("speaker_active", 0),
                line_in_connected=fields.get("line_in_connected", 0),
                display_mode=fields.get("display_mode", 0),
            )
        except KeyError as e:
            raise SchwungBusError(f"STATE: missing field {e}") from e

    # ----- restart-move (L2 fast reset) -------------------------------------

    def restart_move(self) -> None:
        """Trigger restart-move.sh via the shim's `restart_move` flag.

        Returns as soon as the daemon has written the flag — the shim
        picks it up on the next SPI frame and invokes the restart
        script. The Move stack goes down then back up over ~4 seconds.
        Pair with :meth:`wait_for_shim_ready` to block until recovery.

        The daemon and its SHM segments survive the restart (kernel
        keeps them mapped). The shim re-attaches to existing segments
        on init, so frame counter continuity is preserved.
        """
        self._request("RESTART_MOVE")

    def set_open_tool(self, module_id: str) -> None:
        """Ask shadow_ui to load the tool module with the given id.

        Mirrors what schwung-manager (the web UI on port 7700) does
        when the user clicks "Open in tool" from the browser. The
        daemon writes ``/data/UserData/schwung/open_tool_cmd.json``
        with ``{"tool_id": module_id, "file_path": "__test_bus__"}``
        (the non-empty placeholder is required — shadow_ui's
        ``if (cmd.file_path && cmd.tool_id)`` check treats an empty
        string as falsy and silently skips the load) and raises
        the ``shadow_control.open_tool_cmd`` SHM flag. shadow_ui's
        tick polls the flag, reads the JSON, locates the module by
        id, and calls ``startInteractiveTool`` — putting Move into
        ``OVERTAKE_MODULE`` (state.overtake_mode == 2).

        Returns as soon as the daemon has written the file and set
        the flag. The actual module load completes asynchronously
        (~500 ms-2 s depending on module size). Caller should poll
        ``state().overtake_mode == 2`` to wait for the load — use
        the ``ion_loaded`` / module-loaded fixtures rather than
        calling this directly when you can.

        ``module_id`` must match ``[a-zA-Z0-9_-]+``, max 64 chars
        — daemon enforces this and rejects anything else.

        Raises :class:`SchwungBusError` on validation failure.
        """
        if not module_id:
            raise ValueError("set_open_tool requires a non-empty module_id")
        # Brief client-side validation to fail fast with a Python
        # exception rather than waiting for the daemon's ERR reply.
        import re
        if not re.fullmatch(r"[a-zA-Z0-9_-]+", module_id):
            raise ValueError(
                f"module_id must match [a-zA-Z0-9_-]+, got {module_id!r}"
            )
        if len(module_id) > 64:
            raise ValueError(f"module_id too long ({len(module_id)} > 64)")
        self._request(f"SET_OPEN_TOOL {module_id}")

    # ----- param get/set (routes through /schwung-param) -------------------

    # Number of automatic retries on transient param-SHM contention
    # errors ("param SHM busy", "param SET timeout", "param GET timeout").
    # The /schwung-param SHM has two producers — shadow_ui and the
    # daemon — and the wait_idle handshake can briefly fail when
    # shadow_ui is mid-call (e.g. processing user input on its tick).
    # 3 retries with 100ms backoff covers ~99% of intermittent races
    # without masking real failures (a peer that's actually down
    # times out forever, not intermittently).
    PARAM_RETRY_COUNT = 3
    PARAM_RETRY_DELAY_S = 0.1

    _PARAM_TRANSIENT_ERRORS = (
        "param SHM busy",
        "param SET timeout",
        "param GET timeout",
        "DUMP_PARAM_FILE: GET timeout",
    )

    def _param_request_with_retry(self, line: str) -> str:
        """_request wrapper that retries on transient param-SHM contention.

        Non-transient errors (bad key, file not found, ...) propagate
        immediately. There is deliberately no empty-response retry: that
        was a workaround for a host release-store race on response_ready,
        fixed upstream — the daemon now reads the response with acquire
        ordering, so an empty reply means an empty value, not a stale read.
        """
        for attempt in range(self.PARAM_RETRY_COUNT + 1):
            try:
                return self._request(line)
            except SchwungBusError as e:
                if any(t in str(e) for t in self._PARAM_TRANSIENT_ERRORS) and \
                        attempt < self.PARAM_RETRY_COUNT:
                    time.sleep(self.PARAM_RETRY_DELAY_S)
                    continue
                raise
        # Unreachable — loop exits via return or re-raise. Keep type checkers happy.
        raise SchwungBusError(f"param request exhausted retries: {line!r}")

    def set_param(self, key: str, value: str, overtake: bool = True) -> None:
        """SET a chain / overtake-module param via the daemon.

        ``key`` is the param key as the module sees it. For overtake
        modules the shim's setupModuleParamShims prepends ``overtake_dsp:``
        — when ``overtake=True`` (the default) this wrapper does the same so
        test code can just call ``bus.set_param("track.0.channel", "5")`` and
        have it land on the module's set_param("track.0.channel", "5") handler.

        For master-FX / jack / passthrough / chain-slot keys (which
        already have their own prefix), pass ``overtake=False`` to skip
        the auto-prefix.

        Wire-format constraint: value must fit in TESTD_LINE_MAX (4 KiB)
        AFTER prepending "OK " and adding the newline. For larger
        values (e.g. project.json), use :meth:`set_param_file` which
        reads a Move-side file. The daemon rejects oversized values
        with an ERR reply.

        Value must not contain newlines or NUL bytes — the line-based
        protocol can't represent them. Validate client-side to fail
        fast with a clear Python exception.
        """
        if "\n" in value or "\r" in value or "\0" in value:
            raise ValueError(
                "set_param value may not contain newlines or NUL; "
                "use set_param_file for multi-line payloads"
            )
        full_key = f"overtake_dsp:{key}" if overtake else key
        if " " in full_key:
            raise ValueError(f"param key may not contain spaces: {full_key!r}")
        self._param_request_with_retry(f"SET_PARAM {full_key} {value}")

    def get_param(self, key: str, overtake: bool = True) -> str:
        """GET a chain / overtake-module param via the daemon.

        Mirror of :meth:`set_param` on the read side. Same prefixing
        rules. Returns the param's current value as a string (caller
        parses to int/float/JSON as appropriate).

        Wire-format constraint: response must fit in TESTD_LINE_MAX.
        For ``project.json`` and other large GETs, use
        :meth:`dump_param_to_file` + an SCP round-trip.

        Raises :class:`SchwungBusError` on timeout or peer error.
        """
        full_key = f"overtake_dsp:{key}" if overtake else key
        if " " in full_key:
            raise ValueError(f"param key may not contain spaces: {full_key!r}")
        return self._param_request_with_retry(f"GET_PARAM {full_key}")

    def set_param_from_file(self, key: str, move_path: str,
                            overtake: bool = True) -> None:
        """SET a param from a file already present on Move's filesystem.

        For large JSON payloads (project.json ~5-50 KB) that exceed
        TESTD_LINE_MAX. Caller must SCP the file to Move first — this
        method only tells the daemon to read+SET.

        Typical use through the higher-level :meth:`load_ion_project_json`
        helper which handles the SCP itself.
        """
        full_key = f"overtake_dsp:{key}" if overtake else key
        # Reject any whitespace, not just space — the daemon's args
        # parser only splits on space, so a tab in path would be
        # treated as part of the key, producing misleading errors.
        import re
        if re.search(r'\s', full_key) or re.search(r'\s', move_path):
            raise ValueError("param key / path may not contain whitespace")
        self._param_request_with_retry(f"SET_PARAM_FILE {full_key} {move_path}")

    def dump_param_to_file(self, key: str, move_path: str,
                           overtake: bool = True) -> int:
        """GET a param, write its value to a Move-side file. Returns
        the byte count written.

        Mirror of :meth:`set_param_from_file` on the read side. Useful
        for dumping in-memory state (``project.json``) to inspect via
        :meth:`load_ion_project_json` -- which uses this internally.
        """
        full_key = f"overtake_dsp:{key}" if overtake else key
        # Reject any whitespace, not just space — the daemon's args
        # parser only splits on space, so a tab in path would be
        # treated as part of the key, producing misleading errors.
        import re
        if re.search(r'\s', full_key) or re.search(r'\s', move_path):
            raise ValueError("param key / path may not contain whitespace")
        # Reply format: "OK bytes=<N>"
        resp = self._param_request_with_retry(f"DUMP_PARAM_FILE {full_key} {move_path}")
        for tok in resp.split():
            if tok.startswith("bytes="):
                try:
                    return int(tok[6:])
                except ValueError:
                    pass
        raise SchwungBusError(f"DUMP_PARAM_FILE: bad response {resp!r}")

    def wait_for_shim_ready(
        self,
        timeout: float = 15.0,
        freeze_confirm: float = 0.4,
        poll_interval: float = 0.1,
    ) -> int:
        """Wait through a restart-move.sh cycle: counter ticking →
        frozen (shim dead) → ticking again (shim alive).

        Two-phase detection because just "counter advanced" returns too
        early — restart-move.sh has a ~1s preamble (sleep 1 + kill) during
        which the old shim is still ticking. The reliable signal is the
        freeze-then-thaw transition.

        Phase 1: detect freeze. Counter must hold a single value for at
        least ``freeze_confirm`` seconds (default 0.4s, ≈140 SPI frames
        of stillness — clearly not normal jitter).

        Phase 2: detect thaw. Counter advances past the frozen value.

        Returns the counter at thaw. Raises SchwungBusError if either
        phase doesn't complete within ``timeout``.
        """
        deadline = time.monotonic() + timeout

        # Phase 1: freeze detection.
        prev: Optional[int] = None
        prev_at: float = time.monotonic()
        frozen_at_value: Optional[int] = None
        while time.monotonic() < deadline:
            try:
                cur = self.state().shim_counter
                now = time.monotonic()
                if prev is None or cur != prev:
                    prev = cur
                    prev_at = now
                elif now - prev_at >= freeze_confirm:
                    frozen_at_value = cur
                    break
            except SchwungBusError:
                # Brief blip during restart is fine — keep polling.
                pass
            time.sleep(poll_interval)
        if frozen_at_value is None:
            raise SchwungBusError(
                f"wait_for_shim_ready: counter never froze within {timeout}s "
                f"(restart-move.sh may not have fired; last counter={prev})"
            )

        # Phase 2: thaw detection. Use masked delta so a 32-bit wrap
        # during the restart window (Move runs continuously; ~14 day
        # uptime hits 0xFFFFFFFF) doesn't make `cur > frozen` falsely
        # stay False forever. The delta is the forward distance modulo
        # 2^32, so any nonzero positive advance is a thaw — regardless
        # of whether the absolute counter wrapped or not.
        while time.monotonic() < deadline:
            try:
                cur = self.state().shim_counter
                if ((cur - frozen_at_value) & 0xFFFFFFFF) > 0:
                    return cur
            except SchwungBusError:
                pass
            time.sleep(poll_interval)
        raise SchwungBusError(
            f"wait_for_shim_ready: shim never recovered after freeze at "
            f"counter={frozen_at_value} within {timeout}s"
        )

    # ----- button helpers ---------------------------------------------------

    def tap(self, button: str, hold_frames: int = 2) -> None:
        """Press + release a Move button via CC injection on cable 0.

        Known buttons: jog_click, shift, menu, back. Add more in
        ``_BUTTON_CCS`` as commands need them — keep the surface
        explicit so a typo fails loudly.
        """
        cc = _BUTTON_CCS.get(button)
        if cc is None:
            raise ValueError(
                f"unknown button {button!r}; known: {sorted(_BUTTON_CCS)}"
            )
        self.inject_midi(bytes([0x0B, 0xB0, cc, 127]))
        self.wait_frame(hold_frames)
        self.inject_midi(bytes([0x0B, 0xB0, cc, 0]))

    # ----- internals ---------------------------------------------------------

    def _request(self, command: str) -> str:
        """Send one command line and return the OK response payload (no prefix).

        Raises ``SchwungBusError`` on ERR responses, malformed lines, or
        connection problems.
        """
        if self._sock is None:
            raise SchwungBusError("bus not connected")
        self._send_line(command)
        line = self._read_line()
        # Token-match (not prefix-match) so a hypothetical "OKAY foo" or
        # "ERROR bad" reply doesn't get silently mis-routed as success/error.
        if line == "OK" or line.startswith("OK "):
            return line[2:].lstrip()
        if line == "ERR" or line.startswith("ERR "):
            return self._raise_err(line[3:].lstrip())
        raise SchwungBusError(f"protocol error: unexpected reply {line!r}")

    @staticmethod
    def _raise_err(msg: str) -> str:
        raise SchwungBusError(msg or "ERR (no message)")

    def _send_line(self, line: str) -> None:
        assert self._sock is not None
        self._sock.sendall((line + "\n").encode("ascii"))

    def _read_line(self) -> str:
        assert self._sock is not None
        # Drain any buffered bytes first
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = bytes(self._buf[:nl]).decode("ascii", errors="replace").rstrip("\r")
                del self._buf[: nl + 1]
                return line
            chunk = self._sock.recv(4096)
            if not chunk:
                raise SchwungBusError("connection closed by daemon")
            self._buf.extend(chunk)


def _check_pad_note(note: int) -> None:
    if not 68 <= note <= 99:
        raise ValueError(f"pad note must be 68..99, got {note}")


def _check_step_note(note: int) -> None:
    if not 16 <= note <= 31:
        raise ValueError(f"step note must be 16..31, got {note}")


class MidiOutSession:
    """Live MIDI_OUT subscription handle — `drain()` returns events
    captured since the last drain (or since the session started).

    Used by the `midi_out_capture` pytest fixture. The fixture
    subscribes on setup, yields the session, drains-and-unsubscribes on
    teardown. Tests call `session.drain()` (or the shorter `session()`,
    which is equivalent) to assert on captured events mid-test.

    For non-pytest usage prefer `SchwungBus.capture_midi_out()` (a
    context manager).
    """
    def __init__(self, bus: "SchwungBus") -> None:
        self._bus = bus

    def drain(self) -> MidiOutCapture:
        return self._bus.dump_midi_out()

    def __call__(self) -> MidiOutCapture:
        return self.drain()


class MidiOutCaptureContext:
    """Context manager returned by SchwungBus.capture_midi_out().

    Subscribes on enter, drains-and-unsubscribes on exit. The captured
    events are exposed as `.events` (a MidiOutCapture) AFTER the
    `with` block.

    Use this for one-shot scripts and the API examples in the README.
    Inside pytest, prefer the `midi_out_capture` fixture which yields a
    `MidiOutSession` (no implicit __enter__/__exit__ confusion).
    """
    def __init__(self, bus: "SchwungBus") -> None:
        self._bus = bus
        self.events: MidiOutCapture = MidiOutCapture()

    def __enter__(self) -> "MidiOutCaptureContext":
        self._bus.subscribe_midi_out()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        try:
            self.events = self._bus.dump_midi_out()
        finally:
            try:
                self._bus.unsubscribe_midi_out()
            except Exception:
                pass
