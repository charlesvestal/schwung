"""Constants for the ``pristine_set`` fixture.

The fixture resets Move to a known-empty set before a test. It keeps a
dedicated test set on the device, identified by its **display name**:

  * if a set with ``TEMPLATE_DISPLAY_NAME`` already exists, it is reused;
  * otherwise the fixture creates one from ``REPO_TEMPLATE_PATH`` (the
    canonical empty Song.abl committed in this repo), minting a fresh
    UUID for it.

The set's UUID is discovered (or generated) at runtime — see
``_find_template_uuid`` / ``_create_template_set`` in pytest_plugin.py.
"""

from __future__ import annotations

from pathlib import Path


# Display name of the dedicated test set on the device. The fixture
# finds the set by this name under ``Sets/<uuid>/<name>/Song.abl`` and
# reads the UUID one level up. It shows up under this name in Move's set
# list; deleting it there is harmless — the fixture recreates it on the
# next run.
TEMPLATE_DISPLAY_NAME = "Schwung Test Template"

# Canonical empty Song.abl shipped in the repo, captured from a default
# (4-track, no clips) Move set. Resolved relative to this file so tests
# run from any cwd.
#   tools/pytest-schwung/src/schwung_bus/pristine_constants.py
#     .parents[4] -> repo root
REPO_TEMPLATE_PATH = (
    Path(__file__).resolve().parents[4] / "tests" / "fixtures" / "empty_song.abl"
)

# Where the fixture stages the canonical Song.abl on the device so each
# per-test reset is a local ``cp`` (~30 ms) rather than a network scp.
# Under /data/UserData because root FS is tiny and usually full; we own
# the schwung namespace there.
DEVICE_STAGING_PATH = "/data/UserData/schwung/_test_template_song.abl"
