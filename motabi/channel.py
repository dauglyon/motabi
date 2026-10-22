"""The server-kernel channel accepts only the kernel.

marimo's server binds five ZeroMQ sockets on TCP ports and the kernel connects to them. A bound
socket takes any peer: a port scanner that sent an HTTP line was attached as an old-protocol
peer and from then on every other command alternated to it and was lost (2026-10-21), and on a
shared login node a neighbour could do the same or inject commands. ZeroMQ's PLAIN mechanism
closes this: the server's sockets require a name and a secret presented before any pipe is
attached, the kernel presents them, and every other peer is refused by the ZAP handler here.

The secret is made once per session in the server process (install_server, loaded by the
sitecustomize on the server's PYTHONPATH) and reaches the kernel through its environment: the
launcher copies every MOTABI_ variable into the sandbox's spec (mode 0600); the kernel takes it
out of its environment at start. PLAIN sends the secret in clear on the connection: on the
workstation that is loopback, and on a site it is inside the adapter's ssh tunnel. Every server
socket names a ZAP domain and enforces it, so a context without the handler refuses peers
rather than admitting them.
"""
from __future__ import annotations

import os
import secrets
import threading
import weakref
from typing import Any

KEY_ENV = "MOTABI_CHANNEL_KEY"
LOG_ENV = "MOTABI_CHANNEL_LOG"
USER = b"kernel"
ZAP_ADDR = "inproc://zeromq.zap.01"
DOMAIN = b"motabi"
_key: bytes | None = None


def _log(msg: str) -> None:
    path = os.environ.get(LOG_ENV)
    if path:
        with open(path, "a") as f:
            f.write(msg + "\n")


def kernel_options(sock: Any) -> None:
    """Inside the kernel: present the session's credentials on every socket before it connects.
    The kernel's sockets are the five to the server and nothing else; the secret leaves the
    environment on first use and lives in this module for the process's life (restores and
    worker copies keep it, and their sockets keep the options)."""
    import zmq

    global _key
    if _key is None:
        key = os.environ.pop(KEY_ENV, "")
        _key = key.encode() if key else b""
    if _key:
        sock.setsockopt(zmq.PLAIN_USERNAME, USER)
        sock.setsockopt(zmq.PLAIN_PASSWORD, _key)


def install_server() -> None:
    """In the server process: a secret for the session, PLAIN on every socket it binds, and a
    ZAP handler per context that admits the kernel's credentials and nothing else."""
    import zmq

    key = os.environ.get(KEY_ENV)
    if not key:
        key = secrets.token_hex(32)
        os.environ[KEY_ENV] = key  # the launcher, spawned later, inherits it for the sandbox
    expected = key.encode()
    original_socket = zmq.Context.socket
    handled: weakref.WeakSet[Any] = weakref.WeakSet()   # contexts with a handler; a context's id can be reused after it dies

    def zap_loop(zap: Any) -> None:
        while True:
            try:
                frames = zap.recv_multipart()
            except zmq.ZMQError:
                return
            try:
                version, request_id, domain, address, identity, mechanism = frames[:6]
                ok = (domain == DOMAIN and mechanism == b"PLAIN" and len(frames) >= 8 and frames[6] == USER
                      and secrets.compare_digest(frames[7], expected))
            except ValueError:  # a malformed request is refused, never fatal to the handler
                version, request_id, address, mechanism, ok = b"1.0", b"", b"?", b"?", False
            _log(f"zap {address.decode(errors='replace')} {mechanism.decode(errors='replace')} {'ok' if ok else 'refused'}")
            zap.send_multipart([version, request_id, b"200" if ok else b"400", b"OK" if ok else b"refused", USER if ok else b"", b""])

    def socket(self: zmq.Context, socket_type: int, **kwargs: Any) -> zmq.Socket:
        if self not in handled:
            handled.add(self)
            zap = original_socket(self, zmq.REP)
            zap.setsockopt(zmq.LINGER, 0)
            zap.bind(ZAP_ADDR)  # before any socket of this context binds: libzmq looks for it at the handshake
            threading.Thread(target=zap_loop, args=(zap,), name="motabi-zap", daemon=True).start()
        s = original_socket(self, socket_type, **kwargs)
        s.setsockopt(zmq.PLAIN_SERVER, 1)
        s.setsockopt(zmq.ZAP_DOMAIN, DOMAIN)
        s.setsockopt(zmq.ZAP_ENFORCE_DOMAIN, 1)  # no handler for the domain: refuse, never admit
        return s

    zmq.Context.socket = socket  # type: ignore[method-assign]
