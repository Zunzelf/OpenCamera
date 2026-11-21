"""Fake phone for the PhoneLink test (loopback only)."""
import socket, struct, time, sys

def log(*a):
    print("PHONE", *a, flush=True)

# 1. discovery probe
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(2)
u.sendto(b"OPENCAMERA?", ("127.0.0.1", 65501))
try:
    data, _ = u.recvfrom(256)
    log("discovery reply:", data.decode())
except socket.timeout:
    log("discovery: NO REPLY")

def hello(name):
    s = socket.create_connection(("127.0.0.1", 65500), timeout=3)
    s.sendall(struct.pack(">4i", 127, 0, 0, 1) + struct.pack(">i", 25999))
    nb = name.encode()
    s.sendall(struct.pack(">ii", 0x4F43414D, len(nb)) + nb)
    return s

# 2. connect, heartbeat, ask to flip camera, then go silent (watchdog)
s = hello("Test Phone")
log("connected #1")
for i in range(6):
    s.sendall(b"\x01"); time.sleep(0.5)
s.sendall(b"F"); log("sent flip")
for i in range(4):
    s.sendall(b"\x01"); time.sleep(0.5)
log("going silent (expect watchdog after ~6 s)")
time.sleep(8)
s.close()

# 3. old-app style heartbeat (writeChar = 0x00 0x01, no name block), then close
time.sleep(1)
s = socket.create_connection(("127.0.0.1", 65500), timeout=3)
s.sendall(struct.pack(">4i", 127, 0, 0, 1) + struct.pack(">i", 25998))
log("connected #2 (old protocol)")
for i in range(4):
    s.sendall(b"\x00\x01"); time.sleep(0.5)
s.close()
log("closed #2")
time.sleep(1.5)
log("end")
