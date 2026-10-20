import os, sys, time
# a heap object whose address must survive restore; a counter that must continue, not restart
marker = {'k': list(range(1000))}
with open('/mnt/log', 'a') as f:
    f.write(f"start pid={os.getpid()} id={id(marker):#x} py={sys.version.split()[0]}\n"); f.flush()
    for i in range(10**6):
        f.write(f"tick {i} pid={os.getpid()} id={id(marker):#x} sum={sum(marker['k'])}\n"); f.flush()
        time.sleep(0.5)
