# On the marimo server's PYTHONPATH (prototype/run.sh): the kernel channel admits only the kernel.
import os

if os.environ.get("MOTABI_CHANNEL", "1") == "1":
    import motabi.channel

    motabi.channel.install_server()
