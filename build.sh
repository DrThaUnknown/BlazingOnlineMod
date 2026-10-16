#!/bin/sh
# Builds build/BlazingOnline.dylib: JS host bundle on the host machine, then the C part in Docker.
set -e
cd "$(dirname "$0")"
[ -d node_modules ] || npm install
node host/build.mjs
docker build -q -t blazing-online-build . >/dev/null
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/mod" blazing-online-build make
ls -la build/BlazingOnline.dylib
