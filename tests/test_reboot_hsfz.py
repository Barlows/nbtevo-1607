"""1.4.2: libcpreboot.so's ECU reset (CPREBOOT_HSFZ=1), the restart of the phone app CarPlay Advanced Installer 1.6.7: one UDS
ECUReset 0x11 0x01 to ECU 0x63 from tester 0xF4 in an HSFZ frame on TCP 6801, after ReadDataByIdentifier 0xF187 / 0xF190, 0x51 0x01
the acknowledgement. src/reboot.c built for this computer (CPREBOOT_HOST) against a fake HSFZ server; sysmgr_reboot is a stand-in
that prints SYSMGR_REBOOT."""
import os, socket, struct, subprocess, tempfile, threading, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def frame(ctl, payload):
    return struct.pack('>IH', len(payload), ctl) + payload


class FakeHsfz(threading.Thread):
    """answers per UDS request: a function(uds) -> list of UDS answers from 0x63 (each after the gateway's 0x0002 echo)"""
    def __init__(self, answer):
        super().__init__(daemon=True)
        self.answer, self.requests, self.headers, self.connections = answer, [], [], 0
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', 0))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]

    def run(self):
        self.srv.settimeout(10)
        try:
            while True:
                c, _ = self.srv.accept()
                self.connections += 1
                c.settimeout(10)
                try:
                    while True:
                        h = self.recvn(c, 6)
                        if not h:
                            break
                        n, ctl = struct.unpack('>IH', h)
                        p = self.recvn(c, n)
                        self.headers.append((ctl, p[0], p[1]))
                        uds = p[2:]
                        self.requests.append(uds)
                        c.sendall(frame(2, p[:2] + uds[:4]))                       # the gateway's echo
                        for a in self.answer(uds):
                            c.sendall(frame(1, bytes([0x63, 0xF4]) + a))
                except OSError:
                    pass
                c.close()
        except OSError:
            pass

    @staticmethod
    def recvn(c, n):
        b = b''
        while len(b) < n:
            x = c.recv(n - len(b))
            if not x:
                return b''
            b += x
        return b


class RebootHsfz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name, 'reboot')
        main = Path(cls.tmp.name, 'main.c')
        main.write_text('int main(void) { return 0; }\n')
        subprocess.run(['cc', '-DCPREBOOT_HOST', '-o', str(cls.exe), str(ROOT / 'src' / 'reboot.c'), str(main), '-ldl'],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_reboot(self, port, hsfz=True):
        env = dict(os.environ, CPREBOOT_HSFZ_HOST='127.0.0.1', CPREBOOT_HSFZ_PORT=str(port), CPREBOOT_WAIT='0')
        if hsfz:
            env['CPREBOOT_HSFZ'] = '1'
        r = subprocess.run([str(self.exe)], env=env, capture_output=True, text=True, timeout=60)
        return r.stderr

    def serve(self, answer):
        s = FakeHsfz(answer)
        s.start()
        return s

    def test_acknowledged(self):
        def ans(u):
            if u[:1] == b'\x22':
                return [b'\x62' + u[1:3] + b'0123456']
            if u == b'\x11\x01':
                return [b'\x7f\x11\x78', b'\x51\x01']                              # pending, then the acknowledgement
            return []
        s = self.serve(ans)
        err = self.run_reboot(s.port)
        self.assertEqual(s.requests, [b'\x22\xf1\x87', b'\x11\x01'])
        self.assertEqual({h for h in s.headers}, {(1, 0xF4, 0x63)})               # control 1, tester 0xF4, ECU 0x63
        self.assertIn('ECU 0x63 answered DID 0xF187', err)
        self.assertIn('acknowledged by the head unit (UDS 0x51 0x01)', err)
        self.assertIn('waiting 0 s', err)
        self.assertIn('SYSMGR_REBOOT', err)                                         # still up: the old reboot after the wait

    def test_second_did(self):
        def ans(u):
            if u == b'\x22\xf1\x87':
                return [b'\x7f\x22\x31']
            if u == b'\x22\xf1\x90':
                return [b'\x62\xf1\x90WBA00000000000000']
            if u == b'\x11\x01':
                return [b'\x51\x01']
            return []
        s = self.serve(ans)
        err = self.run_reboot(s.port)
        self.assertEqual(s.requests, [b'\x22\xf1\x87', b'\x22\xf1\x90', b'\x11\x01'])
        self.assertIn('acknowledged', err)

    def test_no_identity_no_reset(self):
        s = self.serve(lambda u: [b'\x7f' + u[:1] + b'\x31'])
        err = self.run_reboot(s.port)
        self.assertNotIn(b'\x11\x01', s.requests)
        self.assertIn('did not identify itself', err)
        self.assertNotIn('waiting', err)
        self.assertIn('SYSMGR_REBOOT', err)

    def test_refused(self):
        s = self.serve(lambda u: [b'\x62' + u[1:3] + b'x'] if u[:1] == b'\x22' else [b'\x7f\x11\x22'])
        err = self.run_reboot(s.port)
        self.assertEqual(s.requests.count(b'\x11\x01'), 1)
        self.assertIn('refused by ECU 0x63 (NRC 0x22)', err)
        self.assertNotIn('waiting', err)
        self.assertIn('SYSMGR_REBOOT', err)

    def test_sent_without_answer_is_not_repeated(self):
        s = self.serve(lambda u: [b'\x62' + u[1:3] + b'x'] if u[:1] == b'\x22' else [])
        err = self.run_reboot(s.port)
        self.assertEqual(s.requests.count(b'\x11\x01'), 1)
        self.assertIn('sent, no acknowledgement', err)
        self.assertIn('waiting 0 s', err)

    def test_no_port(self):
        p = socket.socket(); p.bind(('127.0.0.1', 0)); port = p.getsockname()[1]; p.close()
        err = self.run_reboot(port)
        self.assertIn('no HSFZ port at 127.0.0.1', err)
        self.assertIn('SYSMGR_REBOOT', err)

    def test_without_the_switch_as_before(self):
        s = self.serve(lambda u: [])
        err = self.run_reboot(s.port, hsfz=False)
        self.assertEqual(s.connections, 0)
        self.assertNotIn('ECU reset', err)
        self.assertIn('SYSMGR_REBOOT', err)


if __name__ == '__main__':
    unittest.main()
