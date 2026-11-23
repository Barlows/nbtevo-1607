"""src/kombi.h, the Ethernet KOMBI output for CarPlay directions, built for the Mac (KOMBI_HOST: the Mac's own socket
calls) and run against a fake KOMBI on 127.0.0.1: the SOME/IP requests (service 0xB006, our client id, whitelisted methods only), the
route graph / in-place update / distance / lanes + position / cancel sequence, a rejection stopping the output, magic cookies skipped,
a lost link retried with the whole graph, the probe; and the iAP2 -> KOMBI direction, lane arrow and unit mapping.
The graph bytes themselves were compared with NBT-AAD's j29_graph.hpp (identical for the same inputs) when the port was made."""
import os
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'
HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "kombi.h"
static void out(const char *s) { printf("LOG %s\n", s); fflush(stdout); }
// stdin, one command per line:
//   tick <have> <dir> <unit> <metres> <road>|<dist>|<lanes "kind:arrow,...">   at time <now>
//   now <ms> | probe <port> | state | map <type> <angle> <side> | lane <angle> | unit <iap2> <text>
int main(int argc, char **argv) {
  kombi::g_log = out;
  static kombi::Output o; o.init();
  unsigned char ip[4] = {127, 0, 0, 1};
  o.c.init(ip, (unsigned short)atoi(argv[1]), 2000);
  unsigned now = 1000; char line[512];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "now ", 4)) now = (unsigned)atoi(line + 4);
    else if (!strncmp(line, "tick ", 5)) {
      static kombi::Man m; memset(&m, 0, sizeof m);
      int have, n = 0; sscanf(line + 5, "%d %d %d %u %n", &have, &m.dir, &m.unit, &m.metres, &n);
      m.hasStep = true;
      char *rest = line + 5 + n, *bar1 = strchr(rest, '|'), *bar2 = bar1 ? strchr(bar1 + 1, '|') : 0;
      if (!strncmp(rest, "fb:", 3)) { m.roadFb = true; rest += 3; }   // the street is the car's current road
      if (bar1) *bar1 = 0; if (bar2) *bar2 = 0;
      snprintf(m.road, sizeof m.road, "%s", rest); if (bar1) snprintf(m.dist, sizeof m.dist, "%s", bar1 + 1);
      if (bar2) for (char *q = bar2 + 1; *q && m.lanes.n < 8;) {
        m.lanes.kind[m.lanes.n] = (unsigned char)strtol(q, &q, 10); q++;
        m.lanes.arrow[m.lanes.n++] = (unsigned char)strtol(q, &q, 10); if (*q == ',') q++;
      }
      o.tick(have != 0, m, now);
      printf("STATE shown=%d failed=%d inplace=%d\n", o.shown, o.failed, o.inplaceOk); fflush(stdout);
    } else if (!strncmp(line, "probe ", 6)) {
      kombi::Client c; c.init(ip, (unsigned short)atoi(line + 6), 500); printf("PROBE %d\n", c.probe(500)); fflush(stdout);
    } else if (!strncmp(line, "map ", 4)) {
      unsigned t; int a; unsigned s; sscanf(line + 4, "%u %d %u", &t, &a, &s); printf("MAP %d\n", kombi::direction(t, a, s)); fflush(stdout);
    } else if (!strncmp(line, "lane ", 5)) { printf("LANE %d\n", kombi::laneArrow(atoi(line + 5))); fflush(stdout); }
    else if (!strncmp(line, "man ", 4)) {   // man [x:<exit number>] [fb] type angle side metres units <dist text>|<road>|<status/hl/a,a;...>
      static rg_view v; memset(&v, 0, sizeof v); int n = 0; char *s = line + 4;
      if (!strncmp(s, "x:", 2)) { int k = 0; s += 2; while (*s && *s != ' ' && k < 15) v.exitno[k++] = *s++; while (*s == ' ') s++; }
      if (!strncmp(s, "fb ", 3)) { v.roadfb = 1; s += 3; }
      sscanf(s, "%u %d %u %u %u %n", &v.type, &v.angle, &v.side, &v.dist, &v.units, &n);
      char *rest = s + n, *b1 = strchr(rest, '|'), *b2 = b1 ? strchr(b1 + 1, '|') : 0;
      if (b1) *b1 = 0; if (b2) *b2 = 0;
      snprintf(v.dist_s, sizeof v.dist_s, "%s", rest); if (b1) snprintf(v.road, sizeof v.road, "%s", b1 + 1);
      if (b2) for (char *q = b2 + 1; *q && v.nl < 8;) {
        rg_lane &L = v.lanes[v.nl++]; L.status = (unsigned)strtol(q, &q, 10); q++; L.hl = (int)strtol(q, &q, 10); q++;
        while (*q && *q != ';') { L.ang[L.na++] = (int)strtol(q, &q, 10); if (*q == ',') q++; }
        if (*q == ';') q++;
      }
      static kombi::Man m; kombi::manOf(v, m);
      printf("MAN %d %d [%s] [%s] %u", m.dir, m.unit, m.dist, m.road, m.lanes.n);
      for (unsigned i = 0; i < m.lanes.n; i++) printf(" %d:%d", m.lanes.kind[i], m.lanes.arrow[i]);
      printf("\n"); fflush(stdout);
    }
    else if (!strncmp(line, "roadmax ", 8)) { kombi::g_roadDigits = (unsigned)atoi(line + 8); printf("ROADMAX ok\n"); fflush(stdout); }
    else if (!strncmp(line, "ruler ", 6)) { kombi::g_ruler = atoi(line + 6) != 0; printf("RULER ok\n"); fflush(stdout); }
    else if (!strncmp(line, "width ", 6)) { printf("WIDTH %u %u\n", kombi::textW(line + 6, (unsigned)strlen(line + 6)), kombi::roadWidth()); fflush(stdout); }
    else if (!strncmp(line, "fit ", 4)) {   // fit <width>|<street>: kombi::fitRoad into <width> (1/1000 em)
      char *bar = strchr(line + 4, '|'), r[96];
      if (bar) { *bar = 0; kombi::fitRoad(bar + 1, r, sizeof r, (unsigned)atoi(line + 4)); printf("FIT [%s]\n", r); fflush(stdout); }
    }
    else if (!strncmp(line, "unit ", 5)) {
      unsigned u; char t[8] = ""; sscanf(line + 5, "%u %7s", &u, t); printf("UNIT %d %d\n", kombi::unitOf(u), kombi::unitOfText(t)); fflush(stdout);
    }
  }
}
'''


class FakeKombi(object):
    """TCP server: records every SOME/IP request, answers E_OK (or an error for methods in reject), optionally a magic cookie first."""
    def __init__(self):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', 0))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.requests, self.connections, self.reject, self.cookie_first, self.drop_after = [], 0, set(), False, None
        self.lock = threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            with self.lock:
                self.connections += 1
            threading.Thread(target=self.client, args=(c,), daemon=True).start()

    @staticmethod
    def recvn(c, n):
        b = b''
        while len(b) < n:
            k = c.recv(n - len(b))
            if not k:
                raise EOFError
            b += k
        return b

    def client(self, c):
        try:
            while True:
                h = self.recvn(c, 16)
                length = struct.unpack('>I', h[4:8])[0]
                body = self.recvn(c, length - 8)
                svc, method, client, session = struct.unpack('>HHHH', h[0:4] + h[8:12])
                with self.lock:
                    self.requests.append(dict(svc=svc, method=method, client=client, session=session, proto=h[12:16], body=body))
                    n = len(self.requests)
                if self.drop_after is not None and n >= self.drop_after:
                    self.drop_after = None
                    c.close()
                    return
                if self.cookie_first:
                    c.sendall(b'\xff\xff\x80\x00' + struct.pack('>I', 8) + b'\xde\xad\xbe\xef\x01\x01\x02\x00')
                code = 0x01 if method in self.reject else 0x00
                c.sendall(h[0:4] + struct.pack('>I', 8) + h[8:12] + bytes([1, 1, 0x80, code]))
        except (EOFError, OSError):
            pass
        finally:
            c.close()

    def take(self):
        with self.lock:
            r, self.requests = self.requests, []
        return r

    def close(self):
        try:                    # Linux: a close() alone leaves the socket listening while serve() waits in accept()
            self.srv.shutdown(socket.SHUT_RDWR)
        except OSError:         # the Mac: no shutdown on a listening socket (its close() ends the accept)
            pass
        self.srv.close()


class Harness(object):
    """the harness program (HARNESS) against a fake KOMBI"""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / 'h.cpp'
        src.write_text(HARNESS)
        cls.exe = Path(cls.tmp.name) / 'h'
        subprocess.run(['c++', '-std=c++11', '-DKOMBI_HOST', '-I', str(SRC), '-o', str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.k = FakeKombi()
        self.addCleanup(self.k.close)
        self.p = subprocess.Popen([str(self.exe), str(self.k.port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)

    def stop(self):
        self.p.kill()
        self.p.wait()
        self.p.stdin.close()
        self.p.stdout.close()

    def cmd(self, line, until):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        logs = []
        while True:
            l = self.p.stdout.readline()
            self.assertTrue(l, 'harness ended')
            if l.startswith(until):
                return l.split(' ', 1)[1].strip(), logs
            logs.append(l.strip())

    def tick(self, have=1, d=6, unit=3, metres=450, road='Main Street', dist='450', lanes='', now=None):
        if now is not None:
            self.p.stdin.write('now %d\n' % now)
        st, logs = self.cmd('tick %d %d %d %d %s|%s|%s' % (have, d, unit, metres, road, dist, lanes), 'STATE')
        return dict(kv.split('=') for kv in st.split()), logs



class KombiTest(Harness, unittest.TestCase):
    @staticmethod
    def fields(r):
        b = r['body']
        return dict(type=b[8] if len(b) > 8 else 0, index=int.from_bytes(b[5:8], 'big') if len(b) >= 8 else 0, body=b)

    @staticmethod
    def utf16_at_bom(b):
        i = b.index(b'\xfe\xff', 18)
        n = b[i - 1]
        return b[i + 2:i + n - 2].decode('utf-16-be'), i + n

    def check_requests(self, reqs):
        sessions = [r['session'] for r in reqs]
        for r in reqs:
            self.assertEqual(r['svc'], 0xB006)
            self.assertEqual(r['client'], 0x7E01)
            self.assertEqual(r['proto'], bytes([1, 1, 0, 0]))
            self.assertIn(r['method'], (0x2, 0x3, 0x4, 0x9, 0xA, 0xB, 0xC, 0xE, 0x10))
            self.assertEqual(r['body'][0:4], b'\x00\x00\x00\xc8')        # the factory instance handle
        return sessions

    def maneuver0(self, reqs):
        return [self.fields(r) for r in reqs if r['method'] == 0xA and self.fields(r)['type'] == 15 and self.fields(r)['index'] == 0]

    def test_route_graph_then_distance_then_in_place_then_clear(self):
        st, _ = self.tick(d=6, road='Main Street', dist='450', unit=3)
        reqs = self.k.take()
        self.assertEqual(len(reqs), 97)
        self.assertEqual(st['shown'], '1')
        sessions = self.check_requests(reqs)
        self.assertEqual(sessions, list(range(sessions[0], sessions[0] + 97)))
        self.assertEqual(self.k.connections, 1)
        for f in self.maneuver0(reqs):
            self.assertEqual(f['body'][33], 6)
        dist0 = [self.fields(r) for r in reqs if r['method'] == 0xA and self.fields(r)['type'] == 29 and self.fields(r)['index'] == 0]
        text, end = self.utf16_at_bom(dist0[-1]['body'])
        self.assertEqual((text, dist0[-1]['body'][end], dist0[-1]['body'][22]), ('450', 3, 255))   # text, unit m, bar hidden
        roads = [self.utf16_at_bom(r['body'])[0] for r in reqs if r['method'] == 0x3 and self.fields(r)['type'] == 3 and self.fields(r)['index'] == 0 and len(r['body']) > 9]
        self.assertTrue(roads and all(x == 'Main Street' for x in roads), roads)

        self.tick(d=6, road='Main Street', dist='400')            # same maneuver: one distance frame
        reqs = self.k.take()
        self.assertEqual([r['method'] for r in reqs], [0xA])
        self.assertEqual(self.utf16_at_bom(reqs[0]['body'])[0], '400')

        self.tick(d=6, road='Main Street', dist='400')            # nothing changed: nothing sent
        self.assertEqual(self.k.take(), [])

        self.tick(d=7, road='Oak Road', dist='1.2', unit=0)       # next maneuver: the factory's in-place update block
        reqs = self.k.take()
        self.assertEqual(len(reqs), 9)
        self.assertEqual(reqs[-1]['method'], 0x9)
        self.assertEqual(self.fields(reqs[-1])['type'], 33)
        self.assertEqual([f['body'][33] for f in self.maneuver0(reqs)], [7])
        self.assertEqual(self.k.connections, 1)                   # still the one persistent connection

        st, logs = self.tick(have=0)                              # guidance over: the factory cancel on a fresh connection
        reqs = self.k.take()
        self.assertEqual(len(reqs), 6)
        self.check_requests(reqs)
        self.assertEqual([r['session'] for r in reqs], list(range(400, 406)))
        self.assertEqual(self.k.connections, 2)
        self.assertEqual(st['shown'], '0')
        self.assertIn('LOG CarPlayMenu: KOMBI: cleared', logs)
        self.tick(have=0)
        self.assertEqual(self.k.take(), [])

    def test_street_change_redraws_only_the_maneuvers_own(self):
        self.tick(d=6, road='Main Street', dist='450', now=1000)
        self.assertEqual(len(self.k.take()), 97)
        self.tick(d=6, road='fb:Elm Road', dist='400', now=6000)      # the car's current road changed: the distance only
        self.assertEqual([r['method'] for r in self.k.take()], [0xA])
        self.tick(d=6, road='Oak Road', dist='350', now=7000)         # the maneuver's own street: the in-place update
        self.assertEqual(len(self.k.take()), 9)
        self.tick(d=6, road='Pine Road', dist='300', now=8000)        # again within 4 s of it: the distance only, for now
        self.assertEqual([r['method'] for r in self.k.take()], [0xA])
        self.tick(d=6, road='Pine Road', dist='300', now=11000)       # 4 s on: drawn
        self.assertEqual(len(self.k.take()), 9)

    def test_lanes_and_position_feed(self):
        self.tick(d=2, lanes='2:6,1:1,1:1,2:7', metres=300, now=1000)
        reqs = self.k.take()
        self.assertEqual(len(reqs), 93 + 1)                       # graph with our lane model, then one 0x11 position update
        lane = [r for r in reqs if r['method'] == 0x10]
        self.assertEqual(len(lane), 2)                            # one before each of maneuver 0's two 0x0F nodes (as NBT-AAD)
        self.assertEqual(lane[0]['body'], lane[1]['body'])
        b = lane[0]['body']
        self.assertEqual(b[4:9], b'\x1b\x00\x00\x00\x1b')
        n = struct.unpack('>I', b[22:26])[0]
        lanes = b[26:26 + n]
        self.assertEqual([(lanes[i + 1], lanes[i + 7]) for i in range(0, n, 16)], [(2, 6), (1, 1), (1, 1), (2, 7)])
        pos = reqs[-1]
        self.assertEqual((pos['method'], self.fields(pos)['type']), (0x9, 17))
        p0 = struct.unpack('>d', pos['body'][22:30])[0]

        self.tick(d=2, lanes='2:6,1:1,1:1,2:7', metres=200, dist='200', now=1500)   # < 1 s: distance only
        self.assertEqual([r['method'] for r in self.k.take()], [0xA])
        self.tick(d=2, lanes='2:6,1:1,1:1,2:7', metres=150, dist='200', now=2600)   # > 1 s and moved: a position update
        reqs = self.k.take()
        self.assertEqual([r['method'] for r in reqs], [0x9])
        self.assertLess(struct.unpack('>d', reqs[0]['body'][22:30])[0], p0)

        self.tick(d=2, lanes='1:6,2:1,2:1,2:7', metres=150, dist='200', now=3000)   # lanes changed within 3 s: not yet
        self.assertEqual(self.k.take(), [])
        self.tick(d=2, lanes='1:6,2:1,2:1,2:7', metres=150, dist='200', now=4100)   # after 3 s: in-place update with the new row
        reqs = self.k.take()
        self.assertIn(0x10, [r['method'] for r in reqs])
        self.assertLess(len(reqs), 20)

    def test_lane_row_steady(self):
        """1.3.1: a lane row that drops out for a moment stays, and one seen on a single tick is not sent"""
        lanes = '2:6,1:1,1:1'
        self.tick(d=2, lanes=lanes, metres=300, now=1000)
        self.assertIn(0x10, [r['method'] for r in self.k.take()])
        self.tick(d=2, lanes='', metres=300, now=5000)                   # gone (the lane index moved on before its lane set)
        self.tick(d=2, lanes='', metres=300, now=6000)
        self.tick(d=2, lanes=lanes, metres=300, now=6500)                # back within 2.5 s: nothing was sent
        self.assertEqual(self.k.take(), [])
        self.tick(d=2, lanes='1:6,2:1,2:1', metres=300, now=10000)       # another row on one tick only
        self.tick(d=2, lanes=lanes, metres=300, now=10500)
        self.assertEqual(self.k.take(), [])
        self.tick(d=2, lanes='', metres=300, now=11000)                  # gone for good: taken away after 2.5 s
        self.tick(d=2, lanes='', metres=300, now=12000)
        self.assertEqual(self.k.take(), [])
        self.tick(d=2, lanes='', metres=300, now=13600)
        reqs = self.k.take()
        self.assertTrue(reqs)
        self.assertLess(len(reqs), 20)                                   # the in-place update, not the whole graph
        self.tick(d=2, lanes=lanes, metres=300, now=20000)               # a new row: sent on its second tick
        self.assertEqual(self.k.take(), [])
        self.tick(d=2, lanes=lanes, metres=300, now=20500)
        self.assertIn(0x10, [r['method'] for r in self.k.take()])
        self.tick(d=7, lanes='', metres=300, now=21000)                  # a new maneuver takes no row from the last one
        self.assertNotIn(0x10, [r['method'] for r in self.k.take() if self.fields(r)['index'] == 0])

    def test_rejection_stops_the_output(self):
        self.k.reject.add(0x3)
        st, logs = self.tick()
        self.assertEqual(st['failed'], '1')
        self.assertTrue(any('rejected the route graph' in l for l in logs), logs)
        self.k.take()
        self.tick(d=7)
        self.tick(have=0)
        self.assertEqual(self.k.take(), [])

    def test_in_place_rejected_falls_back_to_whole_graph(self):
        self.tick()
        self.k.take()
        self.k.reject.add(0xC)                                    # the in-place block's list frame
        st, logs = self.tick(d=7)
        self.assertTrue(any('in-place update rejected' in l for l in logs), logs)
        self.assertEqual(st['failed'], '1')                       # the whole graph has the same frame: rejected too, off
        self.k.reject.clear()

    def test_magic_cookies_are_skipped(self):
        self.k.cookie_first = True
        st, _ = self.tick()
        self.assertEqual((st['shown'], st['failed']), ('1', '0'))
        self.assertEqual(len(self.k.take()), 97)

    def test_lost_link_reconnects(self):
        self.k.drop_after = 5                                     # the KOMBI drops the connection on the 5th request
        st, _ = self.tick()
        self.assertEqual((st['shown'], st['failed']), ('1', '0'))
        self.assertEqual(len(self.k.take()), 98)                  # that frame again on a second connection
        self.assertEqual(self.k.connections, 2)

    def test_no_kombi(self):
        port = self.k.port
        self.k.close()
        st, logs = self.tick(now=1000)
        self.assertEqual((st['shown'], st['failed']), ('0', '0'))
        self.assertTrue(any('FAILED' in l for l in logs), logs)
        self.assertEqual(self.cmd('probe %d' % port, 'PROBE')[0], '0')

    def test_probe(self):
        self.assertEqual(self.cmd('probe %d' % self.k.port, 'PROBE')[0], '1')
        self.assertEqual(self.k.take(), [])                       # nothing sent

    def test_mapping(self):
        cases = {(1, 0, 0): 6, (2, 0, 0): 7, (3, 0, 0): 1, (4, 0, 0): 8, (4, 0, 1): 9, (6, 0, 0): 15, (6, 0, 1): 14, (30, 0, 0): 15,
                 (8, 45, 0): 29, (8, -45, 0): 28, (8, 0, 1): 28, (9, 0, 0): 26, (10, 0, 0): 10, (13, 0, 0): 2, (50, 0, 0): 3,
                 (47, 0, 0): 4, (48, 0, 0): 5, (22, 0, 0): 28, (23, 0, 0): 29, (52, 0, 0): 32, (53, 0, 0): 33, (255, 0, 0): 1}
        for (t, a, s), want in cases.items():
            self.assertEqual(self.cmd('map %d %d %d' % (t, a, s), 'MAP')[0], str(want), (t, a, s))
        for a, want in {0: 1, -15: 1, 30: 3, -45: 2, 90: 7, -90: 6, 135: 5, -150: 4, 180: 9, -180: 8}.items():
            self.assertEqual(self.cmd('lane %d' % a, 'LANE')[0], str(want), a)
        self.assertEqual(self.cmd('unit 2 m', 'UNIT')[0], '3 3')
        self.assertEqual(self.cmd('unit 4 ft', 'UNIT')[0], '5 5')
        self.assertEqual(self.cmd('unit 0 km', 'UNIT')[0], '0 0')
        self.assertEqual(self.cmd('unit 9 xx', 'UNIT')[0], '-1 -1')


class ManTest(Harness, unittest.TestCase):
    """kombi::manOf: the iPhone's guidance (rgview.h, as librghook hands it over) -> the KOMBI's maneuver"""
    def man(self, line):
        return self.cmd('man ' + line, 'MAN')[0]

    def test_man_of(self):
        self.assertEqual(self.man('1 0 0 450 2 450 m|Main Street|'), '6 3 [450] [Main Street] 0')
        self.assertEqual(self.man('2 0 0 1200 0 1.2 km|Oak Road|'), '7 0 [1.2] [Oak Road] 0')
        self.assertEqual(self.man('3 0 0 800 4 0.5 mi||'), '1 1 [0.5] [ ] 0')                 # no street: a space, never ""
        self.assertEqual(self.man('3 0 0 300 4 1000||'), '1 5 [1000] [ ] 0')                 # text without unit: the iPhone's units (ft)
        self.assertEqual(self.man('3 0 0 75 2 ||'), '1 3 [75] [ ] 0')                        # no text: whole metres
        self.assertEqual(self.man('3 0 0 0 2 ||'), '1 3 [0] [ ] 0')                          # nothing at all: "0 m" (not "-m")
        self.assertEqual(self.man('10 0 0 0 2 0 m||'), '10 3 [0] [ ] 0')
        self.assertEqual(self.man('2 0 0 300 2 300 m|A1|0/1000/-90;1/1000/0;2/90/0,90'), '7 3 [300] [A1] 3 2:6 1:1 1:7')
        self.assertEqual(self.man('2 0 0 300 2 300 m|A1|1/1000/'), '7 3 [300] [A1] 0')       # a lane with no direction: no row

    def width(self, text):
        w, field = self.cmd('width ' + text, 'WIDTH')[0].split()
        return int(w), int(field)

    def fit(self, width, street):
        return self.cmd('fit %d|%s' % (width, street), 'FIT')[0][1:-1]

    def test_width_model(self):
        # Helvetica's advance widths (1/1000 em): digits 556, "i" 222, "W" 944, other letters 600; the field 14 digits less half a digit
        self.assertEqual(self.width('1234567890'), (5560, 14 * 556 - 278))
        self.assertEqual(self.width('iiii')[0], 888)
        self.assertEqual(self.width('WWWW')[0], 3776)
        self.assertEqual(self.width('Main Street')[0], 5113)
        self.assertEqual(self.width('Großherzog')[0], 5324)
        self.assertEqual(self.width('Exit 104  Main Street')[0], 9282)                  # 16.7 digits: 1.3.3 sent it, the HUD cut it

    def test_street_fits(self):
        # by width, not by count: a name that fits stays as the iPhone wrote it (21 narrow characters), a wider one (20 capitals) does not
        self.assertEqual(self.man('1 0 0 450 2 450 m|Main Street|'), '6 3 [450] [Main Street] 0')
        self.assertEqual(self.man('1 0 0 450 2 450 m|Little Illinois Trail|'), '6 3 [450] [Little Illinois Trail] 0')
        self.assertEqual(self.man('1 0 0 450 2 450 m|US HIGHWAY 101 NORTH|'), '6 3 [450] [US HWY 101 N] 0')
        # USPS: the suffix and the directions; then Mount / Fort / Point / Saint; then trailing words; then one word cut (UTF-8)
        self.assertEqual(self.man('1 0 0 450 2 450 m|Fifth Avenue South Westbound|'), '6 3 [450] [Fifth AVE S WB] 0')
        self.assertEqual(self.man('1 0 0 450 2 450 m|Jefferson Davis Highway|'), '6 3 [450] [Jefferson Davis] 0')
        self.assertEqual(self.man('1 0 0 450 2 450 m|North Martin Luther King Junior Boulevard|'), '6 3 [450] [N Martin Luther] 0')
        self.assertEqual(self.man('1 0 0 450 2 450 m|Großherzog-Friedrich-Straße Nord|'), '6 3 [450] [Großherzog-Frie] 0')
        self.assertEqual(self.fit(6000, 'Highway 101'), 'Highway 101')                    # fits (5780)
        self.assertEqual(self.fit(4500, 'Highway 101'), 'HWY 101')                        # the suffix before a number
        self.assertEqual(self.fit(3000, 'I-95 North'), 'I-95 N')                          # a direction after a route
        self.assertEqual(self.fit(5000, 'North Avenue'), 'North AVE')                     # the direction is the name: it stays...
        self.assertEqual(self.fit(3700, 'North Avenue'), 'N AVE')                         # ...unless nothing else fits
        self.assertEqual(self.fit(6000, 'Mount Vernon Highway'), 'MT Vernon')
        self.assertEqual(self.fit(9500, 'Saint Charles Avenue South'), 'Saint Charles AVE S')   # stage 1 is enough
        self.assertEqual(self.fit(8500, 'Saint Charles Avenue South'), 'St Charles AVE S')
        self.assertEqual(self.fit(4500, 'North Kalanianaole Highway'), 'N Kalania')       # never just "N"
        self.assertEqual(self.fit(9000, 'Spring Garden Street'), 'Spring Garden ST')     # name words stay whole
        self.cmd('roadmax 45', 'ROADMAX')                                                      # kombi_road_max: 45 digits
        self.assertEqual(self.man('1 0 0 450 2 450 m|North Martin Luther King Junior Boulevard|'),
                         '6 3 [450] [North Martin Luther King Junior Boulevard] 0')
        self.cmd('roadmax 0', 'ROADMAX')

    # The list ghackt sent for 1.3.5 (USPS Publication 28), as sent: duplicates included
    USPS = ('Avenue=AVE Boulevard=BLVD Circle=CIR Court=CT Drive=DR Lane=LN Place=PL Road=RD Square=SQ Street=ST Terrace=TER Way=WAY '
            'Alley=ALY Beach=BCH Bend=BND Bluff=BLF Bottom=BTM Bridge=BRG Brook=BRK Burg=BG Bypass=BYP Causeway=CSWY Center=CTR '
            'Canyon=CYN Cape=CPE Crossing=XING Crest=CRST Creek=CRK Crescent=CRES Dale=DL Dam=DM Divide=DV Estate=EST '
            'Expressway=EXPY Extension=EXT Falls=FLS Ferry=FRY Field=FLD Fort=FT Freeway=FWY Garden=GDN Gateway=GTWY Glen=GLN '
            'Grove=GRV Harbor=HBR Heights=HTS Highway=HWY Hill=HL Hollow=HOLW Inlet=INLT Island=IS Junction=JCT Key=KY Knoll=KNL '
            'Lake=LK Landing=LNDG Light=LGT Lock=LCK Loop=LOOP Mall=MALL Manor=MNR Meadow=MDW Mill=ML Mission=MSN Mount=MT '
            'Mountain=MTN Neck=NCK Orchard=ORCH Oval=OVAL Park=PARK Parkway=PKWY Point=PT Port=PRT Prairie=PR Ranch=RNCH '
            'Rapids=RPDS Rest=RST Ridge=RDG River=RIV Road=RD Row=ROW Run=RUN Shoal=SHL Shore=SHR Spring=SPG Springs=SPGS Spur=SPUR '
            'Square=SQ Station=STA Stravenue=STRA Stream=STRM Street=ST Summit=SMT Terrace=TER Trace=TRCE Trail=TRL Trailer=TRLR '
            'Tunnel=TNL')

    def test_usps_suffixes(self):
        # every pair as a street's suffix, in a field just wide enough for its short form: the short form, unless it is no narrower
        # than the word in the HUD's font (PRT, CRST, INLT, TRCE and HOLW; WAY, LOOP, ... are the word itself): then the word stays
        kept = []
        for pair in self.USPS.split():
            full, short = pair.split('=')
            for word in (full, full.upper()):                                                # any case
                narrower = self.width(short)[0] < self.width(word)[0]
                got = self.fit(self.width('Oak ' + short)[0], 'Oak ' + word)
                self.assertEqual(got, 'Oak ' + (short if narrower else word), word)
            if not self.width(short)[0] < self.width(full)[0]:
                kept.append(full)
        self.assertEqual(kept, ['Way', 'Crest', 'Hollow', 'Inlet', 'Loop', 'Mall', 'Oval', 'Park', 'Port', 'Row', 'Run', 'Spur',
                                'Trace'])
        self.assertEqual(self.fit(3000, 'Oak Port'), 'Oak')                               # a word that stays is dropped, not widened
        self.assertEqual(self.fit(5000, 'Bay Port North'), 'Bay Port N')                  # "Bay PRT N" would be 5001 wide

    def test_exit_number(self):
        # an exit: its number in front of the road, whole; the road fitted into the width left; other maneuvers ignore the number
        self.assertEqual(self.man('x:23B 23 0 0 800 2 800 m|I-95 North|'), '29 3 [800] [Exit 23B  I-95 N] 0')
        self.assertEqual(self.man('x:23B 8 -30 0 800 2 800 m|I-95|'), '28 3 [800] [Exit 23B  I-95] 0')
        self.assertEqual(self.man('x:23B fb 23 0 0 800 2 800 m|Interstate 95|'), '29 3 [800] [Exit 23B] 0')  # the current road
        self.assertEqual(self.man('x:23B 23 0 0 800 2 800 m||'), '29 3 [800] [Exit 23B] 0')
        self.assertEqual(self.man('x:12 1 0 0 800 2 800 m|Main Street|'), '6 3 [800] [Main Street] 0')
        # 1.3.3's cut-off lines (ghackt, J29): now within the field, the exit number kept
        self.assertEqual(self.man('x:104 23 0 0 800 2 800 m|Main Street|'), '29 3 [800] [Exit 104  Main] 0')
        self.assertEqual(self.man('x:104 23 0 0 800 2 800 m|Jefferson Davis Highway|'), '29 3 [800] [Exit 104  Jeffers] 0')
        for line in ('x:104 23 0 0 800 2 800 m|Jefferson Davis Highway|', 'x:104 23 0 0 800 2 800 m|Main Street|'):
            road = self.man(line).split('[')[2].split(']')[0]
            self.assertLessEqual(self.width(road)[0], self.width(road)[1], road)
        # exits off other roads: keep right (14), slight left (49), a highway change (53) with an exit number
        self.assertEqual(self.man('x:104 14 0 0 800 2 800 m|I-95 North|'), '3 3 [800] [Exit 104  I-95 N] 0')
        self.assertEqual(self.man('x:5 49 0 0 800 2 800 m|Route 9|'), '2 3 [800] [Exit 5  Route 9] 0')
        self.assertEqual(self.man('x:23B 53 0 0 800 2 800 m|I-95 North|'), '33 3 [800] [Exit 23B  I-95 N] 0')
        self.assertEqual(self.man('x:2 6 0 0 800 2 800 m|Main Street|'), '15 3 [800] [Main Street] 0')   # a roundabout: no
        # a wider field (kombi_road_max 16): the road's suffix shortened after the number
        self.cmd('roadmax 16', 'ROADMAX')
        self.assertEqual(self.man('x:104 23 0 0 800 2 800 m|Main Street|'), '29 3 [800] [Exit 104  Main ST] 0')
        self.cmd('roadmax 0', 'ROADMAX')

    def test_ruler(self):
        # kombi_road_ruler: 40 digits instead of every street, exits too, to count the ones the HUD shows whole
        self.cmd('ruler 1', 'RULER')
        row = '1234567890' * 4
        self.assertEqual(self.man('1 0 0 450 2 450 m|Main Street|'), '6 3 [450] [%s] 0' % row)
        self.assertEqual(self.man('x:104 23 0 0 800 2 800 m|Main Street|'), '29 3 [800] [%s] 0' % row)
        self.cmd('ruler 0', 'RULER')
        self.assertEqual(self.man('1 0 0 450 2 450 m|Main Street|'), '6 3 [450] [Main Street] 0')


class ProbeTest(unittest.TestCase):
    """libcpprobe.so (src/probe.cpp) built for the Mac as a program: its constructor runs before main, as it does under
    LD_PRELOAD of /bin/cat on the head unit. Detection line; and the watcher's clear of a left-over arrow, only for its own pid."""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        main = Path(cls.tmp.name) / 'main.cpp'
        main.write_text('int main() { return 3; }\n')
        cls.exe = Path(cls.tmp.name) / 'probe'
        subprocess.run(['c++', '-std=c++11', '-w', '-DKOMBI_HOST', '-I', str(SRC), '-o', str(cls.exe), str(SRC / 'probe.cpp'),
                        str(main)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.k = FakeKombi()
        self.addCleanup(self.k.close)
        self.marker = Path(self.tmp.name) / 'marker'

    def run_probe(self, **env):
        e = dict(os.environ, CPM_KOMBI_ADDR='127.0.0.1:%d' % self.k.port, CPM_KOMBI_MARKER=str(self.marker), **env)
        r = subprocess.run([str(self.exe)], env=e, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0)                         # the constructor exits; main never runs
        return r.stdout

    def test_detection(self):
        self.assertEqual(self.run_probe(), 'kombi=1\n')
        self.assertEqual(self.k.take(), [])
        port = self.k.port
        self.k.close()
        out = subprocess.run([str(self.exe)], env=dict(os.environ, CPM_KOMBI_ADDR='127.0.0.1:%d' % port), capture_output=True,
                             text=True, timeout=30).stdout
        self.assertTrue(out.startswith('kombi=0 '), out)

    def test_clear_only_its_own_arrow(self):
        self.marker.write_text('5555\n')
        self.run_probe(CPM_CLEAR='4444')                     # a newer session's marker: left alone
        self.assertEqual(self.k.take(), [])
        self.assertTrue(self.marker.exists())
        self.run_probe(CPM_CLEAR='5555')
        self.assertEqual([r['session'] for r in self.k.take()], list(range(400, 406)))
        self.assertFalse(self.marker.exists())
        self.run_probe(CPM_CLEAR='5555')                     # no marker: nothing
        self.assertEqual(self.k.take(), [])


if __name__ == '__main__':
    unittest.main()
