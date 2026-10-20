#!/bin/sh
# Start the stand-in site: an Ubuntu cloud-image guest under QEMU/KVM, inside the sitehost
# container (canopy has no qemu and no root; the container has qemu, /dev/kvm is readable).
# ssh reaches the guest at 127.0.0.1:2222 on canopy (user-mode networking, host port forward);
# from the guest, canopy's loopback is 10.0.2.2. CPU model: $CPU (default host = same features
# as canopy; a narrower model such as Haswell exercises the feature pin).
set -eu
cd "$(dirname "$0")"
CPU=${CPU:-host}
docker rm -f motabi-site >/dev/null 2>&1 || true
docker run -d --name motabi-site --device /dev/kvm --network host -v "$PWD:/site" -w /site motabi-sitehost sh -c "
  [ -f disk.qcow2 ] || qemu-img create -q -f qcow2 -F qcow2 -b noble-server-cloudimg-amd64.img disk.qcow2 20G
  cloud-localds seed.iso user-data meta-data
  exec qemu-system-x86_64 -enable-kvm -cpu $CPU -m 8G -smp 4 -nographic \
    -drive file=disk.qcow2,if=virtio,format=qcow2 -drive file=seed.iso,if=virtio,format=raw \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=n0" >/dev/null
echo "guest starting; ssh -p 2222 -i id_site dauglyon@127.0.0.1"
