#!/bin/sh
# Build the environment image and export it as prototype/rootfs/ (the sandbox's "/").
set -eu
cd "$(dirname "$0")/.."
cp ../pyproject.toml env/pyproject.toml
docker build -q -t motabi-env env
rm env/pyproject.toml
cid=$(docker create motabi-env)
rm -rf rootfs && mkdir rootfs
docker export "$cid" | tar -x -C rootfs --exclude='dev/*' --no-same-owner --no-same-permissions
docker rm "$cid" >/dev/null
du -sh rootfs
