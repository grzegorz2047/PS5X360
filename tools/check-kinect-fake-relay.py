"""Host check of the fake Kinect relay test payload (fix 44):

    python3 -I tools/check-kinect-fake-relay.py

Builds tools/source-1.0.2/kinect-fake-relay/kinect_fake_relay.c with the host's C compiler
(FAKE_RELAY_HOST: log and stop file in a temporary folder, a short run time) and checks that it
serves what kinect-tracker.elf's relay serves and the emulator's client accepts
(usb_kinect.hpp): "KNCT" headers, depth (stream 1, 422400 bytes of 11-bit values) and colour
(stream 2, 307200 bytes), frames that move, about 30 per second, a reconnect, a second copy
leaving the port to the first, and a stop through the stop file.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / 'tools/source-1.0.2/kinect-fake-relay/kinect_fake_relay.c'
PORT = 9301
DEPTH, COLOUR = 640 * 480 * 11 // 8, 640 * 480
failures = []


def check(condition, what):
    print(('ok   ' if condition else 'FAIL ') + what)
    if not condition:
        failures.append(what)


def receive(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError('closed')
        data += chunk
    return bytes(data)


def frame(sock):
    header = receive(sock, 12)
    magic, stream, size = header[:4], header[4], struct.unpack_from('<I', header, 8)[0]
    return magic, stream, header[5:8], size, receive(sock, size)


def unpack_depth(packed):
    values, bits, count = [], 0, 0
    for byte in packed:
        bits = (bits << 8 | byte) & 0xFFFFFF
        count += 8
        if count >= 11:
            count -= 11
            values.append((bits >> count) & 2047)
    return values


def connect(limit=3.0):
    end = time.monotonic() + limit
    while True:
        try:
            return socket.create_connection(('127.0.0.1', PORT), timeout=2)
        except OSError:
            if time.monotonic() > end:
                raise
            time.sleep(0.05)


def main():
    compiler = os.environ.get('CC_HOST') or shutil.which('clang-18') or shutil.which('cc')
    if not compiler:
        sys.exit('no C compiler')
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        exe = folder / 'kinect-fake-relay'
        build = subprocess.run([compiler, '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-Wall', '-Wextra',
                                '-DFAKE_RELAY_HOST', str(SOURCE), '-o', str(exe)],
                               capture_output=True, text=True)
        print(build.stdout + build.stderr, end='')
        check(build.returncode == 0, 'builds with the host compiler')
        check('warning' not in build.stderr, 'no compiler warnings')
        if build.returncode:
            sys.exit(1)
        env = dict(os.environ, FAKE_RELAY_LOG=str(folder / 'relay.log'),
                   FAKE_RELAY_STOP=str(folder / 'relay.stop'), FAKE_RELAY_SECONDS='20')
        relay = subprocess.Popen([str(exe)], env=env)
        try:
            sock = connect()
            started = time.monotonic()
            frames = [frame(sock) for _ in range(6)]
            took = time.monotonic() - started
            for n, (magic, stream, pad, size, payload) in enumerate(frames):
                expected = (1, DEPTH) if n % 2 == 0 else (2, COLOUR)
                check(magic == b'KNCT' and pad == b'\0\0\0' and (stream, size) == expected,
                      f'frame {n}: KNCT, stream {expected[0]}, {expected[1]} bytes')
            depth = unpack_depth(frames[0][4])
            check(len(depth) == 640 * 480, 'depth unpacks to 640x480 values')
            check(set(depth) == {880, 1000, 2047}, 'depth: disc, wall and no-reading values')
            check(depth[240 * 640 + 120] == 880 and depth[10 * 640 + 600] == 1000
                  and depth[470 * 640] == 2047, 'depth: the disc, the wall and the floor edge')
            check(set(frames[1][4]) == {40, 200}, 'colour: two-tone bars')
            check(frames[0][4] != frames[2][4] and frames[1][4] != frames[3][4], 'frames move')
            check(0.03 < took < 1.0, f'about 30 frame pairs per second (3 pairs in {took:.2f} s)')
            sock.close()
            # A second copy finds the port taken and leaves.
            second_log = folder / 'second.log'
            second = subprocess.run([str(exe)], env=dict(env, FAKE_RELAY_LOG=str(second_log),
                                                         FAKE_RELAY_STOP=str(folder / 'second.stop')),
                                    timeout=10)
            check(second.returncode == 1 and 'is taken' in second_log.read_text(),
                  'a second copy leaves the port to the first')
            # The emulator connects again.
            sock = connect()
            magic, stream, _, size, _ = frame(sock)
            check(magic == b'KNCT' and stream == 1 and size == DEPTH, 'a reconnect gets frames again')
            # A client that stops reading is dropped after the send timeout, and the stop file
            # ends the payload.
            time.sleep(3)
            sock.close()
            (folder / 'relay.stop').write_text('')
            code = relay.wait(timeout=5)
            check(code == 0, 'the stop file ends it with status 0')
        finally:
            if relay.poll() is None:
                relay.kill()
        log = (folder / 'relay.log').read_text()
        print(log, end='')
        for line in ('listening on 127.0.0.1:9301', 'emulator connected', 'emulator disconnected after',
                     'stopped after', 'notification: PS5X360 test: fake Kinect relay running'):
            check(line in log, f'log: {line}')
    if failures:
        sys.exit(f'FAIL: {len(failures)} check(s)')
    print('PASS: the fake Kinect relay serves KNCT depth and colour frames as kinect-tracker.elf does')


if __name__ == '__main__':
    main()
