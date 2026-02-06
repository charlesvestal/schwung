#!/usr/bin/env bash
# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=/data/UserData/move-anything/lib:$LD_LIBRARY_PATH
exec env LD_PRELOAD=move-anything-shim.so /opt/move/MoveOriginal
