# A host for the stand-in site: QEMU with KVM, driving an Ubuntu cloud-image guest.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-x86 qemu-utils cloud-image-utils openssh-client rsync ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
