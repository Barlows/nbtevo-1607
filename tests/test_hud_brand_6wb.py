"""1.2.11 / 1.2.12 behaviours carried into 1.3.0's HUD set-up (src/hud.h), on test_hud_init.py's harness:
  * the picture box follows the coded brand (HMI_BRAND) before the MINI layout's presence (1.2.12: a BMW F20 with HMI 1607 and a
    6WA cluster carries the MINI package and got the MINI box);
  * on a 6WB, hud_text_handles with a 6WA cluster's instances (below 0x17) is not taken (issue #10: a BMW F36 with a 6WB tried the
    F25's "0x0c 0x12 0x0f"), its own instances are, and on a 6WA the F25's set still is;
  * the 6WB plane's picture is the 6WA box mapped by toWide (x 1:1, y 5/4: (70,20)-(170,120) -> (66,18)-(166,143)).
The switch files are read from a test folder (HUD_SWITCHES)."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tests.test_hud_init import HARNESS as INIT_HARNESS

ROOT = Path(__file__).resolve().parents[1]
MAIN = r'''
// stdin, one command a line:
//   choosebox               chooseBox(); then the box
//   use6wb | text6wa        use6wb() / textOverride() on a 6WA; then the handles
//   poly                    the 6WA box's corners as one whole polygon
int main() {
  g_log = logLine;
  S.streamCtor = sCtor; S.streamWrite = sWrite; S.streamSend = sSend; S.streamDtor = sDtor;
  static char cli[0x400];
  g_cli = cli; g_attached = 1; g_registered = 1; g_client = 7; g_self = 0x205e; g_host = 0x0101; g_hostOf[0] = 0x0101; g_hostOf[1] = 0x0102;
  char line[128];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\n")] = 0;
    if (!strcmp(line, "choosebox")) { chooseBox(); boxLine(); }
    else if (!strcmp(line, "use6wb")) { use6wb("test"); printf("HANDLES %u %u %u\n", hStreet, hDist, hUnit); }
    else if (!strcmp(line, "text6wa")) { textOverride(); printf("HANDLES %u %u %u\n", hStreet, hDist, hUnit); }
    else if (!strcmp(line, "poly")) {
      const Pt p[4] = { { 70, 20 }, { 170, 20 }, { 170, 120 }, { 70, 120 } };
      polygon(hBufA, 1, p, 4, true);
    }
    fflush(stdout);
  }
}
'''


class HudBrand6wb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang++'):
            raise unittest.SkipTest('clang++ not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='hud-brand-6wb-')
        d = Path(cls.tmp.name)
        cls.sw = d / 'sw'
        cls.sw.mkdir()
        src = INIT_HARNESS[:INIT_HARNESS.index('int main() {')] + MAIN
        (d / 'h.cpp').write_text(src)
        cls.exe = d / 'h'
        subprocess.run(['clang++', '-std=c++11', '-w', '-DHUD_SWITCHES="%s/"' % cls.sw, '-I', str(ROOT), '-I', str(ROOT / 'src'),
                        '-o', str(cls.exe), str(d / 'h.cpp')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def tearDown(self):
        for f in self.sw.iterdir():
            f.unlink()

    def run_h(self, cmds, brand=None):
        env = {k: v for k, v in os.environ.items() if k != 'HMI_BRAND'}
        if brand is not None:
            env['HMI_BRAND'] = brand
        return subprocess.run([str(self.exe)], input='\n'.join(cmds) + '\n', capture_output=True, text=True, check=True,
                              env=env).stdout

    # ---- the picture box by the coded brand (1.2.12) -----------------------------------------------------------------------------
    def test_bmw_brands_get_the_6wa_box(self):
        for b in ('00', '01', '03', '04', '05'):
            out = self.run_h(['choosebox'], b)
            self.assertIn('BOX 6wa', out, b)
            self.assertIn('HMI_BRAND not MINI, whatever layouts the unit carries', out)

    def test_mini_brand_gets_the_mini_box(self):
        out = self.run_h(['choosebox'], '02')
        self.assertIn('BOX mini', out)
        self.assertIn('HMI_BRAND=02, a MINI', out)

    def test_no_or_odd_brand_falls_back_to_the_layout(self):
        layout = Path('/opt/hmi/data/ro/mini/id5/widget/generated/Spec_State_Merged.pbw').exists()
        for b in (None, '7', '0x', '002'):
            out = self.run_h(['choosebox'], b)
            self.assertNotIn('HMI_BRAND', out, b)
            self.assertIn('BOX mini' if layout else 'BOX 6wa', out)

    # ---- hud_text_handles on a 6WB (issue #10, 1.2.11) -------------------------------------------------------------------------
    def test_6wa_handles_ignored_on_a_6wb(self):
        (self.sw / 'hud_text_handles').write_text('0x0c 0x12 0x0f\n')
        out = self.run_h(['use6wb'])
        self.assertIn('hud_text_handles 12 18 15 ignored: 6WA instances on a 6WB', out)
        self.assertIn('HANDLES 26 27 28', out)                           # the 6WB's own 0x1a 0x1b 0x1c

    def test_6wb_handles_taken_on_a_6wb(self):
        (self.sw / 'hud_text_handles').write_text('0x1a 0x1b 0\n')        # 0: that text off
        out = self.run_h(['use6wb'])
        self.assertNotIn('ignored', out)
        self.assertIn('texts on street 26, distance 27, unit 0 (switch hud_text_handles)', out)
        self.assertIn('HANDLES 26 27 0', out)

    def test_f25_handles_still_taken_on_a_6wa(self):
        (self.sw / 'hud_text_handles').write_text('0x0c 0x12 0x0f\n')
        out = self.run_h(['text6wa'])
        self.assertNotIn('ignored', out)
        self.assertIn('HANDLES 12 18 15', out)

    # ---- the 6WB plane's picture (toWide) ------------------------------------------------------------------------------------
    def test_6wb_picture_mapped_by_toWide(self):
        out = self.run_h(['use6wb', 'poly'])
        polys = [l.split()[2:] for l in out.splitlines() if l.startswith('00 21d ')]
        self.assertTrue(polys, out)
        d = [int(x, 16) for x in polys[0]]
        k = d[5]
        pts = sorted((d[6 + 4 * i] << 8 | d[7 + 4 * i], d[8 + 4 * i] << 8 | d[9 + 4 * i]) for i in range(k))
        self.assertEqual(pts, [(66, 18), (66, 143), (166, 18), (166, 143)])


if __name__ == '__main__':
    unittest.main()
