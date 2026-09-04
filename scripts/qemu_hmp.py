#!/usr/bin/env python3
import argparse, socket, sys, time

PROFILES = {
    "normal": (30, .10),
    "stress": (30, .20),
    "sync": (30, .25),
}
MAX_HOLD_MS = 1000

def recv_prompt(sock, timeout):
    sock.settimeout(timeout); data=b""
    while b"(qemu)" not in data:
        part=sock.recv(4096)
        if not part: break
        data += part
    return data.decode(errors="replace")

def command(path, cmd, timeout=3.0, expect=True):
    if not __import__('os').path.exists(path): raise RuntimeError(f"HMP socket does not exist: {path}")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout); s.connect(path); recv_prompt(s, timeout)
        s.sendall((cmd+"\n").encode())
        if not expect: time.sleep(0.2)
        return recv_prompt(s, timeout) if expect else ""

def keyname(ch):
    if 'a' <= ch <= 'z' or '0' <= ch <= '9': return ch
    if ch == ' ': return 'spc'
    if ch == '-': return 'minus'
    if ch == '+': return 'shift-equal'
    raise ValueError(f"unsupported text character: {ch!r}")

def timing(profile, hold_ms, delay):
    base_hold, base_delay = PROFILES[profile]
    hold_ms = base_hold if hold_ms is None else hold_ms
    delay = base_delay if delay is None else delay
    if hold_ms <= 0 or hold_ms > MAX_HOLD_MS:
        raise ValueError(f"hold-ms must be 1..{MAX_HOLD_MS}")
    if delay <= 0 or delay * 1000 <= hold_ms:
        raise ValueError("delay must be positive and greater than hold-ms")
    if delay * 1000 < hold_ms + 20:
        raise ValueError("delay must be at least hold-ms + 20 ms")
    return hold_ms, delay

def add_timing_options(parser, default_profile="normal"):
    parser.add_argument('--profile', choices=PROFILES, default=default_profile)
    parser.add_argument('--hold-ms', type=int)
    parser.add_argument('--delay', type=float)

def main():
    p=argparse.ArgumentParser(); p.add_argument('--selftest-keymap',action='store_true'); p.add_argument('--selftest-timing',action='store_true'); p.add_argument('--socket'); p.add_argument('--timeout',type=float,default=3)
    sub=p.add_subparsers(dest='op')
    c=sub.add_parser('command'); c.add_argument('hmp')
    k=sub.add_parser('key'); k.add_argument('key'); add_timing_options(k)
    t=sub.add_parser('text'); t.add_argument('text'); t.add_argument('--enter',action='store_true'); add_timing_options(t)
    sub.add_parser('quit'); a=p.parse_args();
    if a.selftest_keymap:
        assert keyname('-')=='minus' and keyname('+')=='shift-equal'
        print("'-' -> minus"); print("'+' -> shift-equal"); return 0
    if a.selftest_timing:
        for name, (hold_ms, delay) in PROFILES.items():
            timing(name, hold_ms, delay)
            assert hold_ms < delay * 1000 and delay * 1000 >= hold_ms + 20
            print(f"{name}: hold_ms={hold_ms} delay={delay:.2f}s PASS")
        return 0
    if not a.socket or not a.op: p.error('--socket and an operation are required')
    try:
        if a.op=='command': print(command(a.socket,a.hmp,a.timeout))
        elif a.op=='key':
            hold_ms, _ = timing(a.profile, a.hold_ms, a.delay)
            print(command(a.socket,f'sendkey {a.key} {hold_ms}',a.timeout))
        elif a.op=='quit': command(a.socket,'quit',a.timeout,False)
        else:
            hold_ms, delay = timing(a.profile, a.hold_ms, a.delay)
            keys=[keyname(ch) for ch in a.text]
            for key in keys:
                command(a.socket,f'sendkey {key} {hold_ms}',a.timeout); time.sleep(delay)
            if a.enter: command(a.socket,f'sendkey ret {hold_ms}',a.timeout)
    except (OSError,RuntimeError,ValueError,socket.timeout) as e:
        print(f"qemu_hmp.py: {e}",file=sys.stderr); return 1
    return 0
if __name__=='__main__': raise SystemExit(main())
