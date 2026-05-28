"""Make abl_io importable from tests/print_stems/ for tests in the same dir."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
