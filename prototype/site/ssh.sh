#!/bin/sh
# ssh/scp/rsync into the guest without touching the user's known_hosts.
exec ssh -p 2222 -i "$(dirname "$0")/id_site" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dauglyon@127.0.0.1 "$@"
