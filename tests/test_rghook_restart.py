"""1.4.4: "Restart CarPlay" inside mm-ipod (src/rghook.c restart_watch): the menu's request "CPR1 <ms>" ends this mm-ipod only when it was
made after this mm-ipod started and less than 10 s ago (CLOCK_MONOTONIC ms, wrapping like a u32). Built for this computer with
test_rghook_lanes' way of including rghook.c."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = r'''
#define read rg_read_hook
#define write rg_write_hook
#include "rghook.c"
#undef read
#undef write
extern int printf(const char *, ...);
int main(void) {
  struct { const char *c; u32 start, now; } t[] = {
    {"CPR1 5000\n", 1000, 5200}, {"CPR1 5000\n", 6000, 6100}, {"CPR1 5000\n", 1000, 16000}, {"CPR1 5000\n", 1000, 4000},
    {"XPR1 5000\n", 1000, 5200}, {"CPR1 \n", 1000, 5200}, {"", 1000, 5200}, {"CPR1 3\n", 4294967000u, 200},
  };
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) printf("%d", rg_restart_due(t[i].c, t[i].start, t[i].now));
  printf("\n");
  return 0;
}
'''


class RestartWatch(unittest.TestCase):
    def test_due(self):
        if not shutil.which('clang'):
            self.skipTest('clang not found')
        with tempfile.TemporaryDirectory() as d:
            Path(d, 'm.c').write_text(MAIN)
            subprocess.run(['clang', '-w', '-I', str(ROOT / 'src'), '-o', str(Path(d, 'm')), str(Path(d, 'm.c'))], check=True)
            out = subprocess.run([str(Path(d, 'm'))], capture_output=True, text=True, check=True).stdout.strip()
        # fresh after the start; made before this mm-ipod started; 11 s old; in the future; not ours; no number; empty; across the wrap
        self.assertEqual(out, '10000001')

    def test_watch_started_with_the_link_and_ends_itself(self):
        s = (ROOT / 'src' / 'rghook.c').read_text()
        w = s[s.index('static void *restart_watch(void *a) {'):]
        w = w[:w.index('\n}\n')]
        self.assertIn('k(gp(), 15);', w)                                       # its own pid: always permitted
        self.assertIn('rg_lower_priority(10);', w)
        self.assertIn('#define RG_RESTART_REQ "/dev/shmem/carplaymenu-cprestart"', s)
        tr = s[s.index('snprintf(b, sizeof b, "transport fd %d\\n", fd);'):][:200]
        self.assertIn('restart_watch_once();', tr)
        h = (ROOT / 'src' / 'cprestart.h').read_text()
        self.assertIn('#define CPR_REQ "/dev/shmem/carplaymenu-cprestart"', h)  # the same file on both sides


if __name__ == '__main__':
    unittest.main()
