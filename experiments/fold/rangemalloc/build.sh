#!/bin/sh
# Build the four shims: {jemalloc, mimalloc} backing x {glibc, backing} home.
# Sources: jemalloc 5.4.0 (prefix rj_, static) and mimalloc v3.5.3 (single object, no override),
# cloned into jemalloc-src/ and mimalloc-src/ (gitignored). See README.md.
set -eu
cd "$(dirname "$0")"
mkdir -p build
[ -d jemalloc-src ] || git clone -q --depth 1 --branch 5.4.0 https://github.com/jemalloc/jemalloc.git jemalloc-src
[ -d mimalloc-src ] || git clone -q --depth 1 --branch v3.5.3 https://github.com/microsoft/mimalloc.git mimalloc-src
if [ ! -f jemalloc-src/lib/libjemalloc_pic.a ]; then
  (cd jemalloc-src && ./autogen.sh && ./configure --with-jemalloc-prefix=rj_ --with-private-namespace=rjp_ \
     --disable-shared --enable-static --disable-cxx --disable-doc --disable-stats --disable-initial-exec-tls && make -j"$(nproc)" build_lib_static) > build/jemalloc.log 2>&1
fi
[ -f build/mimalloc.o ] || gcc -O2 -g -fPIC -DNDEBUG -DMI_SECURE=0 -Imimalloc-src/include -c mimalloc-src/src/static.c -o build/mimalloc.o
CF="-shared -fPIC -O2 -g -Wall -Wno-unused-function"
EXPORT='-Wl,--version-script=exports.map'
for home in GLIBC BACKING; do
  h=$(echo $home | tr A-Z a-z)
  gcc $CF -DBACKING_JEMALLOC -DHOME_$home -Ijemalloc-src/include rangemalloc.c jemalloc-src/lib/libjemalloc_pic.a -lpthread -ldl -lm $EXPORT -o build/librangemalloc-je-$h.so
  gcc $CF -DBACKING_MIMALLOC -DHOME_$home -Imimalloc-src/include rangemalloc.c build/mimalloc.o -lpthread -ldl -latomic $EXPORT -o build/librangemalloc-mi-$h.so
done
ls -la build/*.so
