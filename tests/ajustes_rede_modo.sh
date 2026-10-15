#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
sources=()
for source in src/*.c src/dts/*.c; do
  case "$source" in src/main.c|src/ajustes.c) continue;; esac
  sources+=("$source")
done
flags=(-O1 -g -Wno-deprecated-declarations -Wno-macro-redefined)
libs=(-lSDL2 -lSDL2_image -lSDL2_ttf -lz)
if [ "$(uname -s)" = Darwin ]; then
  flags+=(-I/opt/homebrew/include -I/opt/homebrew/include/SDL2)
  libs+=(-L/opt/homebrew/lib -framework OpenGL)
else
  read -r -a sdl_flags <<< "$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
  read -r -a sdl_libs <<< "$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"
  flags+=("${sdl_flags[@]}")
  libs+=("${sdl_libs[@]}" -lGL -lm -lpthread)
fi
dir=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-aj-rede.XXXXXX")
trap 'rm -rf "$dir"' EXIT
cc "${sources[@]}" tests/ajustes_rede_modo.c -Isrc "${flags[@]}" "${libs[@]}" -o "$dir/teste"
"$dir/teste"
