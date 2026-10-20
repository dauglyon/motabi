# The sandbox environment: what the kernel process sees as "/" on every site.
# Exported to prototype/rootfs/ and copied verbatim to each site. Versions pinned to the
# server's venv on canopy (marimo protocol and pickle formats must match).
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh
ENV UV_PYTHON_INSTALL_DIR=/opt/python
RUN uv python install 3.12.10 && uv venv /opt/venv --python 3.12.10
RUN uv pip install --python /opt/venv/bin/python "marimo==0.25.2" "numpy==2.5.4" "pyzmq==27.2.0"
# An editable install of the checkout at its canopy path registers the executor entry point;
# the checkout itself is bind-mounted there at run time (its built C extensions included).
COPY pyproject.toml /home/dauglyon/work/motabi/pyproject.toml
RUN mkdir -p /mnt /home/dauglyon/work/motabi/motabi && touch /home/dauglyon/work/motabi/motabi/__init__.py \
    && uv pip install --python /opt/venv/bin/python --no-deps -e /home/dauglyon/work/motabi \
    && rm -rf /home/dauglyon/work/motabi/motabi
