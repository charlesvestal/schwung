#!/usr/bin/env bash
set -x
./clean.sh    
mkdir -p ./build/
echo "Building..."
"${CROSS_PREFIX}gcc" -g -O3 src/control_surface_move.c -o build/control_surface_move -Ilibs/quickjs/quickjs-2025-04-26 -Llibs/quickjs/quickjs-2025-04-26 -lquickjs -lm
"${CROSS_PREFIX}gcc" -g3 -shared -fPIC -o build/control_surface_move_shim.so src/control_surface_move_shim.c -ldl
cp ./src/*.js ./src/*.mjs ./src/*.sh ./src/*.py font.* ./build/
