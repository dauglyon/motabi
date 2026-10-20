# The base (DESIGN.md 2.7): libc and the system libraries beneath the venv, plus uv. Exported
# to a directory, named by digest, shipped once per release. Everything Python comes from uv
# at each site, from the notebook's lockfile.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh \
    && mkdir -p /mnt /opt /home/dauglyon/work/motabi
