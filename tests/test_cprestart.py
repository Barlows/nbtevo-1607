"""1.4.3 / 1.4.4: "Restart CarPlay" (src/cprestart.h): NBT-AAD's process restart (nbt::mmr::Launcher::stop / findProcess / exePath, read
from its nbt-aad binary) aimed at CarPlay's session process mm-ipod. Built for this computer (CPR_HOST) with CPR_PROC naming a fake
/proc whose <pid>/exefile files name real processes started here (or whose <pid>/as file stands for the DCMD_PROC_MAPDEBUG_BASE
answer: 8 bytes, then the path); the waits and signals are real."""
import os, signal, subprocess, sys, tempfile, threading, time, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = r'''
#include "cprestart.h"
static void lg(const char *m) { puts(m); fflush(stdout); }
int main(int argc, char **argv) {
  cpr::g_log = lg;
  if (argc > 1) {  /* "find": the pids found */
    int p[8], n = cpr::findMmIpod(p, 8);
    for (int i = 0; i < n; i++) printf("FOUND %d\n", p[i]);
    return 0;
  }
  cpr::restartThread(0);
  return 0;
}
'''


class CarPlayRestart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.proc = Path(cls.tmp.name, 'proc')
        cls.proc.mkdir()
        src = Path(cls.tmp.name, 'main.cpp')
        src.write_text(MAIN)
        cls.exe = Path(cls.tmp.name, 'cpr')
        cls.req = Path(cls.tmp.name, 'cprestart')
        subprocess.run(['c++', '-DCPR_HOST', '-DCPR_WAIT_S=4', '-DCPR_PROC="%s"' % cls.proc, '-DCPR_REQ="%s"' % cls.req, '-I',
                        str(ROOT / 'src'), '-o', str(cls.exe), str(src)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        if self.req.exists():
            self.req.unlink()
        for d in self.proc.iterdir():
            for f in d.iterdir():
                f.unlink()
            d.rmdir()
        self.kids = []
        for name in ('self', 'boot', '1x'):                                    # not process entries: skipped
            (self.proc / name).mkdir()

    def tearDown(self):
        for p in self.kids:
            if p.poll() is None:
                p.kill()
                p.wait()

    def start(self, exe, ignore_term=False, as_only=False, self_exit=False):
        # self_exit: as librghook's restart_watch in mm-ipod: ends itself when the menu's request file appears
        body = ('import os, time\nfor i in range(600):\n    time.sleep(0.1)\n'
                '    if os.path.exists(%r) and open(%r).read().startswith("CPR1 "):\n        os._exit(0)\n' % (str(self.req), str(self.req))
                if self_exit else 'time.sleep(60)')
        cmd = [sys.executable, '-c', 'import signal, time\n' +
               ('signal.signal(signal.SIGTERM, signal.SIG_IGN)\n' if ignore_term else '') + body]
        p = subprocess.Popen(cmd)
        threading.Thread(target=p.wait, daemon=True).start()                    # reaped at once, as on the unit
        self.kids.append(p)
        d = self.proc / str(p.pid)
        d.mkdir()
        if as_only:                                                             # no exefile: the path from /proc/<pid>/as
            (d / 'as').write_bytes(b'\0' * 8 + exe.encode() + b'\0')
        else:
            (d / 'exefile').write_text(exe + '\n')
        return p

    def run_cpr(self, *args, timeout=30, env=None):
        e = dict(os.environ, **(env or {}))
        return subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=timeout, env=e).stdout

    def test_finds_mm_ipod_only(self):
        a = self.start('/fs/sda0/opt/conn/bin/carplay/mm-ipod')                 # the system's
        b = self.start('/fs/sda1/opt/hmi/data/ID5/carplaymenu/bin/mm-ipod')     # the wrapper's copy (same name)
        self.start('/fs/sda0/opt/conn/bin/Connectivity')                         # never
        self.start('/fs/sda0/opt/conn/bin/carplay/mm-ipod-helper')               # another name
        time.sleep(0.3)
        found = sorted(int(l.split()[1]) for l in self.run_cpr('find').splitlines())
        self.assertEqual(found, sorted([a.pid, b.pid]))

    def test_path_from_as_when_there_is_no_exefile(self):
        # nbt-aad's exePath: /proc/<pid>/exefile, else /proc/<pid>/as with DCMD_PROC_MAPDEBUG_BASE (the path at +8)
        a = self.start('/fs/sda0/opt/conn/bin/carplay/mm-ipod', as_only=True)
        self.start('/fs/sda0/opt/conn/bin/Connectivity', as_only=True)
        b = self.start('/opt/conn/bin/carplay/mm-ipod')
        time.sleep(0.3)
        found = sorted(int(l.split()[1]) for l in self.run_cpr('find').splitlines())
        self.assertEqual(found, sorted([a.pid, b.pid]))
        out = self.run_cpr()
        self.assertIn('found mm-ipod pid %d: /fs/sda0/opt/conn/bin/carplay/mm-ipod (as)' % a.pid, out)
        self.assertIn('found mm-ipod pid %d: /opt/conn/bin/carplay/mm-ipod (exefile)' % b.pid, out)
        self.assertIn('mm-ipod pid %d stopped' % a.pid, out)
        self.assertEqual(a.returncode, -signal.SIGTERM)

    def test_devctl_command_is_nbt_aads(self):
        h = (ROOT / 'src' / 'cprestart.h').read_text()
        self.assertIn('const int kMapDebugBase = 0x40100803;', h)               # nbt-aad: devctl(fd, 0x40100803, buf, 1040, 0)
        self.assertIn('const unsigned long kMapDebugSize = 16 + 1024;', h)
        self.assertIn('map[8 + k]', h)                                          # its path at +8 (after the u64 vaddr)
        for name in ('nbt-aad', 'mm-renderer', 'acpd', 'mdnsd', 'Connectivity'):  # only mm-ipod is matched
            self.assertNotIn('"/%s"' % name, h)
        self.assertIn('static const char kEnd[] = "/mm-ipod";', h)

    def test_sigterm_then_new_session(self):
        a = self.start('/opt/conn/bin/carplay/mm-ipod')
        conn = self.start('/opt/conn/bin/Connectivity')
        time.sleep(0.3)
        new = []
        def connectivity():                                                     # Connectivity's next session, a second later
            time.sleep(1.2)
            new.append(self.start('/opt/conn/bin/carplay/mm-ipod'))
        threading.Thread(target=connectivity, daemon=True).start()
        out = self.run_cpr()
        self.assertIn('stopping mm-ipod pid %d (SIGTERM)' % a.pid, out)
        self.assertIn('mm-ipod pid %d stopped' % a.pid, out)
        self.assertNotIn('SIGKILL', out)
        self.assertEqual(a.returncode, -signal.SIGTERM)
        self.assertIn('CarPlay session started again (mm-ipod pid %d, after ' % new[0].pid, out)
        self.assertIsNone(conn.poll())                                          # Connectivity untouched

    def test_sigkill_when_sigterm_is_ignored(self):
        a = self.start('/opt/conn/bin/carplay/mm-ipod', ignore_term=True)
        time.sleep(0.5)
        t0 = time.time()
        out = self.run_cpr()
        self.assertIn('mm-ipod pid %d ignored SIGTERM: SIGKILL' % a.pid, out)
        self.assertIn('mm-ipod pid %d stopped' % a.pid, out)
        self.assertEqual(a.returncode, -signal.SIGKILL)
        self.assertIn('no new CarPlay session within 4 s', out)                 # nobody started one
        self.assertLess(time.time() - t0, 10)

    def test_hmi_may_not_signal_it_mm_ipod_ends_itself(self):
        # 1.4.4: EPERM from the HMI (in the car the button restarted nothing): the request file, librghook in mm-ipod ends it
        a = self.start('/opt/conn/bin/carplay/mm-ipod', self_exit=True)
        time.sleep(0.3)
        new = []
        def connectivity():
            while a.poll() is None:
                time.sleep(0.05)
            time.sleep(0.5)
            new.append(self.start('/opt/conn/bin/carplay/mm-ipod'))
        threading.Thread(target=connectivity, daemon=True).start()
        out = self.run_cpr(env={'CPR_FAKE_EPERM': '1'})
        self.assertIn('SIGTERM to mm-ipod pid %d refused (errno 1, EPERM' % a.pid, out)
        self.assertIn('asked mm-ipod to end itself', out)
        self.assertIn('mm-ipod ended itself', out)
        self.assertTrue(self.req.read_text().startswith('CPR1 '))
        self.assertIn('CarPlay session started again (mm-ipod pid %d, after ' % new[0].pid, out)

    def test_hmi_may_not_signal_it_and_no_librghook(self):
        a = self.start('/opt/conn/bin/carplay/mm-ipod')
        time.sleep(0.3)
        out = self.run_cpr(env={'CPR_FAKE_EPERM': '1'})
        self.assertIn('mm-ipod still running: librghook not in it', out)
        self.assertIn('CarPlay not restarted', out)
        self.assertIsNone(a.poll())

    def test_none_visible_asks_anyway(self):
        out = self.run_cpr()
        self.assertIn('no mm-ipod visible from the HMI: asked a running mm-ipod to end itself', out)
        self.assertTrue(self.req.read_text().startswith('CPR1 '))

    def test_no_session(self):
        self.start('/opt/conn/bin/Connectivity')
        self.req.parent.chmod(0o555)                                            # no request file possible: plain "no session"
        try:
            out = self.run_cpr()
        finally:
            self.req.parent.chmod(0o755)
        self.assertIn('no mm-ipod', out)


if __name__ == '__main__':
    unittest.main()
