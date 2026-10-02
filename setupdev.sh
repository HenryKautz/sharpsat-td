#!/bin/bash
set -e
CMAKE_ARGS=()
if [ "$1" = "static" ]; then
	CMAKE_ARGS+=(-DSTATIC=ON)
fi
# On macOS, /usr/bin/g++ is a clang shim, and a bare cmake picks Apple clang.
# Build with Homebrew GCC instead: the newest g++-N, unless CC/CXX are set.
if [ "$(uname -s)" = "Darwin" ] && [ -z "$CXX" ]; then
	GXX=""
	for dir in /opt/homebrew/bin /usr/local/bin; do
		for g in "$dir"/g++-[0-9]*; do
			[ -x "$g" ] || continue
			if [ -z "$GXX" ] || [ "${g##*-}" -gt "${GXX##*-}" ]; then
				GXX="$g"
			fi
		done
		[ -n "$GXX" ] && break
	done
	if [ -z "$GXX" ]; then
		echo "setupdev.sh: Homebrew GCC not found; run 'brew install gcc' (or set CC and CXX)" >&2
		exit 1
	fi
	CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="$GXX" -DCMAKE_C_COMPILER="${GXX%/*}/gcc-${GXX##*-}")
fi
rm -rf build/
mkdir build
cd build
cmake ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"} ..
make
cd ..
cp build/sharpSAT bin/sharpSAT
cp build/flow_cutter_pace17 bin/flow_cutter_pace17
