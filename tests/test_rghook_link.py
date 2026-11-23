"""librghook.so's iAP2 link layer (src/rghook.c read() / write() hooks), built for this computer: what mm-ipod's driver receives.

An iAP2 byte stream (link packets of the control session, route-guidance messages among them) is written into one end of a socket
pair in uneven chunks, with pauses, as a Bluetooth or Wi-Fi transport delivers it; the hooked read() is called on the other end with
the driver's buffer size. The driver must get exactly the stream it would get without the hook, in order, except that every
route-guidance packet (0x5201 / 0x5202 / 0x5204) is replaced by the 16-byte substitute (an empty PowerUpdate 0xAE01, same header).
Small buffers matter: a route-guidance packet split across reads is held back whole, and the driver's reads on a wireless link can
be smaller than such a packet."""
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define read rg_read_hook
#define write rg_write_hook
#include "rghook.c"
#undef read
#undef write
extern int open(const char *, int, ...); extern int close(int); extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long); extern int unlink(const char *); extern int rename(const char *, const char *);
extern int printf(const char *, ...); extern long strtol(const char *, char **, int); extern int fflush(void *);
#ifdef __APPLE__
#define HOST_WCT 0x601
extern void *__stdoutp;
#define H_OUT __stdoutp
#else
#define HOST_WCT 0x241
extern void *stdout;
#define H_OUT stdout
#endif
static int h_open(const char *p, int f, ...) { return f ? open(p, HOST_WCT, 0644) : open(p, 0); }
static long h_write(int fd, const void *b, size_t n) { return write(fd, b, n); }
static long h_read(int fd, void *b, size_t n) { return read(fd, b, n); }
int main(int argc, char **argv) {   /* argv: transport fd, the driver's buffer size, control session id */
  r_open = h_open; r_write = (long (*)(int, const void *, size_t))h_write; r_read = (long (*)(int, void *, size_t))h_read;
  r_close = close; r_unlink = unlink; inited = 1; logfd = -1;
  transport_fd = (int)strtol(argv[1], 0, 10);
  unsigned n = (unsigned)strtol(argv[2], 0, 10);
  ctl_sess = (int)strtol(argv[3], 0, 10);
  static unsigned char b[70000];
  for (;;) {
    long r = rg_read_hook(transport_fd, b, n);
    if (r <= 0) break;
    if ((unsigned long)r > n) { printf("OVERRUN %ld > %u\n", r, n); return 2; }
    for (long i = 0; i < r; i++) printf("%02x", b[i]);
    printf("\n");
    fflush(H_OUT);
  }
  printf("END %u\n", held_len);
  return 0;
}
'''


def cks(b):
    return (0x100 - (sum(b) & 0xff)) & 0xff


def packet(seq, ack, sess, payload=b'', ctl=0x40):
    """one iAP2 link packet: ff 5a len ctl seq ack session hcs, payload, pcs"""
    ln = 9 + (len(payload) + 1 if payload else 0)
    h = bytes([0xff, 0x5a]) + struct.pack('>H', ln) + bytes([ctl, seq & 0xff, ack & 0xff, sess])
    h += bytes([cks(h)])
    return h + (payload + bytes([cks(payload)]) if payload else b'')


def control(mid, params=b''):
    return b'\x40\x40' + struct.pack('>HH', 6 + len(params), mid) + params


def tlv(pid, v):
    return struct.pack('>HH', len(v) + 4, pid) + v


def substitute(pkt):
    """what the hook hands the driver for a route-guidance packet: same header, 40 40 00 06 ae 01"""
    h = bytearray(pkt[:9])
    h[2:4] = struct.pack('>H', 16)
    h[8] = cks(h[:8])
    pl = b'\x40\x40\x00\x06\xae\x01'
    return bytes(h) + pl + bytes([cks(pl)])


SESS = 1


def stream():
    """(bytes on the wire, bytes the driver must get): control traffic, data on another session, acks, and route guidance with
    long names (a few hundred bytes each, larger than a small read)"""
    wire, want = b'', b''
    seq = 10
    road = b'Avenue of the Long Street Names Which Do Not End Soon ' * 4
    for k in range(12):
        items = [
            packet(seq, 5, SESS, control(0xAE01)),                                       # a message the driver knows
            packet(seq + 1, 5, 2, bytes(range(200)) * 2),                               # another session's data (400 bytes)
            packet(0, 5, SESS),                                                          # ack only
            packet(seq + 2, 5, SESS, control(0x5201, tlv(1, b'\x01') + tlv(3, road + b'\0')
                                             + tlv(13, struct.pack('>H', k)))),          # RouteGuidanceUpdate, ~250 bytes
            packet(seq + 3, 5, SESS, control(0x5202, tlv(1, struct.pack('>H', k)) + tlv(3, b'\x02')
                                             + tlv(4, road + b'\0') + tlv(2, road + road + b'\0'))),   # maneuver, ~700 bytes
        ]
        for p in items:
            wire += p
            mid = struct.unpack('>H', p[13:15])[0] if len(p) > 15 and p[9:11] == b'\x40\x40' and p[7] == SESS else 0
            want += substitute(p) if mid in (0x5201, 0x5202, 0x5204) else p
        seq += 4
    return wire, want


class RgHookLink(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang'):
            raise unittest.SkipTest('clang not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rghook-link-')
        d = Path(cls.tmp.name)
        (d / 'h.c').write_text(HARNESS)
        cls.exe = d / 'h'
        subprocess.run(['clang', '-w', '-I', str(ROOT / 'src'), '-o', str(cls.exe), str(d / 'h.c')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def through(self, wire, n, chunks, pause=0.004):
        """the wire bytes written in the given chunk sizes (cycled), the hooked read() with buffer n: the bytes the driver got"""
        a, b = socket.socketpair()
        env = dict(os.environ, RGHOOK_PPS=str(Path(self.tmp.name) / 'pps'), RGHOOK_LOG=str(Path(self.tmp.name) / 'log'))
        p = subprocess.Popen([str(self.exe), str(a.fileno()), str(n), str(SESS)], stdout=subprocess.PIPE, text=True,
                             pass_fds=[a.fileno()], env=env)
        a.close()

        def feed():
            i = k = 0
            while i < len(wire):
                c = chunks[k % len(chunks)]
                b.sendall(wire[i:i + c])
                i += c
                k += 1
                time.sleep(pause)
            b.close()
        t = threading.Thread(target=feed)
        t.start()
        out, _ = p.communicate(timeout=60)
        t.join()
        lines = out.split('\n')
        self.assertNotIn('OVERRUN', out)
        end = [i for i, l in enumerate(lines) if l.startswith('END ')]
        self.assertEqual(len(end), 1, out[-200:])
        return bytes.fromhex(''.join(lines[:end[0]])), int(lines[end[0]].split()[1])

    def test_large_reads(self):
        wire, want = stream()
        for chunks in ([4096], [1000, 37, 512], [9, 3, 100], [1]):
            got, held = self.through(wire, 65536, chunks, pause=0.001 if chunks == [1] else 0.004)
            self.assertEqual(got, want, chunks)
            self.assertEqual(held, 0)

    def test_small_reads_keep_the_order(self):
        """the driver's buffer smaller than a route-guidance packet that arrives split (a wireless link's small reads)"""
        wire, want = stream()
        for n in (64, 200, 512):
            for chunks in ([300, 50], [700, 10, 400], [128]):
                got, held = self.through(wire, n, chunks)
                self.assertEqual(got, want, (n, chunks))
                self.assertEqual(held, 0, (n, chunks))

    def test_tiny_reads(self):
        """buffers smaller than the 16-byte substitute: what does not fit comes with the next read, still in order"""
        wire, want = stream()
        for n in (12, 9):
            got, held = self.through(wire[:6000], n, [300, 50], pause=0.001)
            self.assertEqual(got, want[:len(got)], n)
            self.assertGreater(len(got), 2000, n)

    def test_route_guidance_longer_than_the_hold_buffer(self):
        """one that cannot be held whole goes on unchanged (the driver gets it as before 1.3.1); the stream stays in order"""
        road = b'Avenue of the Long Street Names Which Do Not End Soon ' * 400          # ~21 KB
        big = packet(40, 5, SESS, control(0x5202, tlv(1, struct.pack('>H', 3)) + tlv(4, road + b'\0')))
        small = packet(41, 5, SESS, control(0x5201, tlv(1, b'\x01')))
        known = packet(42, 5, SESS, control(0xAE01))
        wire = known + big + small + known
        got, held = self.through(wire, 4096, [1500])
        self.assertEqual(got, known + big + substitute(small) + known)
        self.assertEqual(held, 0)


WRITE_HARNESS = r"""
#define read rg_read_hook
#define write rg_write_hook
#include "rghook.c"
#undef read
#undef write
extern int open(const char *, int, ...); extern int close(int); extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long); extern int unlink(const char *); extern int rename(const char *, const char *);
extern int printf(const char *, ...); extern long strtol(const char *, char **, int);
static unsigned cap;
static long h_write(int fd, const void *b, size_t n) { return write(fd, b, cap && n > cap ? cap : n); }  /* a transport taking cap bytes a call */
static int h_open(const char *p, int f, ...) { return -1; }
int main(int argc, char **argv) {   /* argv: transport fd, delta, control session, cap, then one hex string per write() call */
  r_open = h_open; r_write = (long (*)(int, const void *, size_t))h_write; r_close = close; r_unlink = unlink;
  inited = 1; logfd = -1;
  transport_fd = (int)strtol(argv[1], 0, 10);
  delta = (unsigned)strtol(argv[2], 0, 10);
  ctl_sess = (int)strtol(argv[3], 0, 10);
  cap = (unsigned)strtol(argv[4], 0, 10);
  static unsigned char b[70000];
  for (int a = 5; a < argc; a++) {
    unsigned n = 0;
    for (const char *q = argv[a]; q[0] && q[1]; q += 2) {
      char t[3] = { q[0], q[1], 0 };
      b[n++] = (unsigned char)strtol(t, 0, 16);
    }
    printf("%ld\n", rg_write_hook(transport_fd, b, n));
  }
  return 0;
}
"""


def renumbered(pkt, delta):
    """what the phone must get for a driver packet once StartRouteGuidanceUpdates went out: its sequence number + delta"""
    h = bytearray(pkt[:9])
    h[5] = (h[5] + delta) & 0xff
    h[8] = cks(h[:8])
    return bytes(h) + pkt[9:]


class RgHookWrite(unittest.TestCase):
    """the driver's packets on their way out after the injection: renumbered however they are written"""

    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang'):
            raise unittest.SkipTest('clang not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rghook-write-')
        d = Path(cls.tmp.name)
        (d / 'w.c').write_text(WRITE_HARNESS)
        cls.exe = d / 'w'
        subprocess.run(['clang', '-w', '-I', str(ROOT / 'src'), '-o', str(cls.exe), str(d / 'w.c')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def writes(self, pieces, delta=1, cap=0):
        """each piece one write() of the driver; what the phone got, and what each write() returned"""
        a, b = socket.socketpair()
        p = subprocess.Popen([str(self.exe), str(a.fileno()), str(delta), str(SESS), str(cap)] + [x.hex() for x in pieces],
                             pass_fds=[a.fileno()], stdout=subprocess.PIPE, text=True)
        a.close()
        got = []

        def drain():   # read while it writes: many small writes fill a socket's buffer long before their bytes do
            while True:
                c = b.recv(65536)
                if not c:
                    break
                got.append(c)
        t = threading.Thread(target=drain)
        t.start()
        out, _ = p.communicate(timeout=60)
        t.join(10)
        b.close()
        return b''.join(got), [int(x) for x in out.split()]

    def driver_packets(self):
        return [packet(20, 7, SESS, control(0xAE02, tlv(1, b'\x05' * 300))),   # a longer message
                packet(21, 7, SESS, control(0x4E09, tlv(0, b'x' * 40))),
                packet(21, 8, SESS),                                            # ack only
                packet(22, 8, 2, bytes(range(256)) * 3)]                        # another session's data

    def test_packets_written_in_pieces(self):
        pk = self.driver_packets()
        wire = b''.join(pk)
        want = b''.join(renumbered(p, 1) for p in pk)
        for cut in ([9], [4, 100], [9, 1, 2, 500], [333], [1]):                # headers alone or cut, payloads cut
            pieces, i, k = [], 0, 0
            while i < len(wire):
                c = cut[k % len(cut)]
                pieces.append(wire[i:i + c])
                i += c
                k += 1
            got, ret = self.writes(pieces)
            self.assertEqual(ret, [len(x) for x in pieces], cut)
            self.assertEqual(got, want, cut)

    def test_short_transport_writes_are_finished(self):
        pk = self.driver_packets()
        got, ret = self.writes(pk, cap=7)
        self.assertEqual(ret, [len(p) for p in pk])
        self.assertEqual(got, b''.join(renumbered(p, 1) for p in pk))

    def test_without_injection_unchanged(self):
        pk = self.driver_packets()
        got, _ = self.writes([b''.join(pk)[:50], b''.join(pk)[50:]], delta=0)
        self.assertEqual(got, b''.join(pk))


if __name__ == '__main__':
    unittest.main()
