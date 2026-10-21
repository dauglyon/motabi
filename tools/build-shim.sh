#!/bin/sh
# Build motabi/librangemalloc.so, the preloaded libc-allocator shim (motabi/rangemalloc.c), against a
# static jemalloc 5.4.0 with the symbol prefix rj_ so it never collides with a library's bundled copy.
# The jemalloc checkout lives in build/jemalloc-src (gitignored) unless JEMALLOC_SRC names another.
set -eu
cd "$(dirname "$0")/.."
SRC=${JEMALLOC_SRC:-$PWD/build/jemalloc-src}
mkdir -p build
[ -d "$SRC" ] || git clone -q --depth 1 --branch 5.4.0 https://github.com/jemalloc/jemalloc.git "$SRC"
if [ ! -f "$SRC/lib/libjemalloc_pic.a" ]; then
  (cd "$SRC" && ./autogen.sh && ./configure --with-jemalloc-prefix=rj_ --with-private-namespace=rjp_ \
     --disable-shared --enable-static --disable-cxx --disable-doc --disable-stats --disable-initial-exec-tls \
     && make -j"$(nproc)" build_lib_static) > build/jemalloc.log 2>&1
fi
gcc -shared -fPIC -O2 -g -Wall -Wno-unused-function -I"$SRC/include" motabi/rangemalloc.c "$SRC/lib/libjemalloc_pic.a" \
    -lpthread -ldl -lm -Wl,--version-script=motabi/rangemalloc.map -o motabi/librangemalloc.so
echo "built motabi/librangemalloc.so"
