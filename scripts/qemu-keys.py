#!/usr/bin/env python3
"""Drives a running QEMU through its monitor socket: types text, sends key combos, takes screenshots.

    qemu-system-x86_64 ... -monitor unix:mon.sock,server,nowait &
    scripts/qemu-keys.py mon.sock session.txt

session.txt, one command per line:
    wait 2.5              seconds
    type help\\n           text (\\n = Enter; ASCII letters, digits, space and common punctuation)
    key alt-shift         a QEMU sendkey combo
    shot out.ppm          screendump (PPM)
    quit                  stop QEMU (also done at the end)
"""
import socket
import sys
import time

NAMES = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma', '/': 'slash',
         ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent',
         '\\': 'backslash'}
SHIFTED = {'?': 'slash', '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', ':': 'semicolon', '"': 'apostrophe', '<': 'comma',
           '>': 'dot', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash', '~': 'grave_accent'}


def main():
    sock_path, script = sys.argv[1], sys.argv[2]
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    s.settimeout(0.2)
    time.sleep(0.3)

    def cmd(c):
        s.send((c + '\n').encode())
        time.sleep(0.06)
        try:
            s.recv(65536)
        except (BlockingIOError, socket.timeout):
            pass

    def typ(text):
        for ch in text:
            if ch.isupper():
                cmd(f'sendkey shift-{ch.lower()}')
            elif ch in SHIFTED:
                cmd(f'sendkey shift-{SHIFTED[ch]}')
            else:
                cmd(f'sendkey {NAMES.get(ch, ch)}')

    for line in open(script, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line:
            continue
        op, _, arg = line.partition(' ')
        if op == 'type':
            typ(arg.replace('\\n', '\n'))
        elif op == 'key':
            cmd(f'sendkey {arg}')
        elif op == 'wait':
            time.sleep(float(arg))
        elif op == 'shot':
            cmd(f'screendump {arg}')
            time.sleep(0.5)
        elif op == 'quit':
            break
    cmd('quit')


if __name__ == '__main__':
    main()
