"""1.3.2: src/hud.h on a BMW 6WB cluster that the plugin could not tell from a 6WA (ghackt/Carplay-advanced-installer issue #2:
BMW F11, 6WB with HUD, HUD-only install, nothing in the cluster or HUD), built for the computer with the DSI stream stubbed and a
fake cluster answering the calls:
  * the size question (ImageGetSize) also goes with transmission type 4, as nbt-android-auto asked a 6WB; a 6WB answering only that
    is found;
  * unanswered, the 6WA profile is a guess: asked again every 10 s, and given up for the 6WB layout when the cluster refuses the
    picture calls on the 6WA handles;
  * on a 6WB the plane shown is also set visible (and the other hidden) with SetVisibility, as nbt-android-auto's car-verified
    sender does;
  * the BMW navigation counts as drawing only after a burst of 4 picture calls in 3 s (nbt-android-auto: single stray calls made
    its sender yield wrongly); while it waits, any drawing call keeps the wait;
  * our own calls, coming back with the head unit's address, are recognised among the last 256 (a 6WB picture is more than 32)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
int open(const char *, int, ...);
long read(int, void *, unsigned long);
long write(int, const void *, unsigned long);
int close(int);
int unlink(const char *);
int usleep(unsigned) { return 0; }
}
#include "src/hud.h"
using namespace hud;
static unsigned g_id;
static int g_model;   // 0 silent; 1 a 6WB answering sizes only with transmission 4 in its [h][01] form; 2 the same with 0;
                      // 3 (1.3.8) a BMW F10 6WB: answers the [h] form for 0x1F (221 x 158) and for 5 (126 x 126) too
static int g_refuse;  // the cluster refuses AddPolygon / RemoveAll / SwitchVisibility on handles below 0x17
static int g_refuseText;  // 1.3.9: the cluster refuses SetText in coding 00 (bit 1) / 02 (bit 2)
static unsigned g_hud2W, g_hud2H;  // 1.3.9: 82/02 answers ImageGetSize for 0x1F with this size ([h] form), 0: no answer
static void inject(unsigned char fb, unsigned char inst, unsigned short src, unsigned short dst, unsigned short fkt, unsigned char op,
                   const unsigned char *d, unsigned n) {
  static unsigned char m[400];
  memset(m, 0, sizeof m);
  m[4] = 2;
  m[12] = (unsigned char)dst; m[13] = (unsigned char)(dst >> 8);
  m[14] = (unsigned char)src; m[15] = (unsigned char)(src >> 8);
  m[16] = fb; m[17] = inst; m[18] = (unsigned char)fkt; m[19] = (unsigned char)(fkt >> 8); m[20] = op;
  m[28] = (unsigned char)n; m[29] = (unsigned char)(n >> 8);
  memcpy(m + 32, d, n);
  static char is[0x50];
  memset(is, 0, sizeof is);
  *(unsigned *)(is + 0x48) = kInfoMessage;
  *(unsigned *)(is + 0x08) = 32 + n;
  *(const unsigned char **)(is + 0x10) = m;
  onData(0, is);
}
static void sCtor(void *, void *, void *, int, unsigned, unsigned, unsigned id, const void *, const void *, int) { g_id = id; }
static void sWrite(void *, const void *pv, unsigned n) {
  const unsigned char *p = (const unsigned char *)pv;
  if (g_id != kReqProcessMessage || n < 36) return;
  unsigned tx = p[4] | p[5] << 8;
  unsigned char inst = p[21];
  unsigned short fkt = p[22] | p[23] << 8;
  unsigned dn = p[32] | p[33] << 8 | p[34] << 16 | (unsigned)p[35] << 24;
  const unsigned char *d = p + 36;
  printf("%u %02x %03x", tx, inst, fkt);
  for (unsigned i = 0; i < dn && 36 + i < n; i++) printf(" %02x", d[i]);
  printf("\n");
  unsigned h = dn >= 2 ? (unsigned)(d[0] << 8 | d[1]) : 0;
  // the call itself comes back to every subscriber with the head unit's address (0x2000), as in the car
  inject(0x82, inst, 0x2000, g_host, fkt, 6, d, dn);
  if (fkt == 0xC04 && g_model && dn == 3 && (g_model == 2 || tx == 4)) {
    const unsigned w = h == 0x1f ? 221 : 0, ht = h == 0x1f ? 158 : 0;
    const unsigned char a[7] = { d[0], d[1], 1, (unsigned char)(w >> 8), (unsigned char)w, (unsigned char)(ht >> 8), (unsigned char)ht };
    inject(0x82, inst, g_host, g_self, fkt, 0xD, a, 7);
  }
  if (fkt == 0xC04 && g_model == 3 && dn == 2 && (h == 0x1f || h == 5)) {
    const unsigned w = h == 0x1f ? 221 : 126, ht = h == 0x1f ? 158 : 126;
    const unsigned char a[6] = { d[0], d[1], (unsigned char)(w >> 8), (unsigned char)w, (unsigned char)(ht >> 8), (unsigned char)ht };
    inject(0x82, inst, g_host, g_self, fkt, 0xD, a, 6);
  }
  if (fkt == 0xC04 && inst == 2 && g_hud2W && dn == 2 && h == 0x1f) {
    const unsigned char a[6] = { d[0], d[1], (unsigned char)(g_hud2W >> 8), (unsigned char)g_hud2W, (unsigned char)(g_hud2H >> 8),
                                 (unsigned char)g_hud2H };
    inject(0x82, 2, 0x0102, g_self, fkt, 0xD, a, 6);
  }
  bool hangulText = false;  // 1.3.11: bit 4 refuses UTF-16 texts with Hangul (a font without it)
  if (fkt == 0x210 && dn >= 3 && d[2] == 0)
    for (unsigned q = 3; q + 1 < dn; q += 2)
      if (d[q] >= 0xac && d[q] <= 0xd7)
        hangulText = true;
  if (fkt == 0x210 && dn >= 3 && ((d[2] == 0 && (g_refuseText & 1)) || (d[2] == 2 && (g_refuseText & 2)) ||
                                  (hangulText && (g_refuseText & 4)))) {
    const unsigned char e[2] = { 6, 1 };
    inject(0x82, inst, g_host, g_self, fkt, 0xF, e, 2);
  }
  if (fkt == 0x21D || fkt == 0x220 || fkt == 0x20E) {
    if (g_refuse && h < 0x17) {
      const unsigned char e[2] = { 6, 1 };
      inject(0x82, inst, g_host, g_self, fkt, 0xF, e, 2);
    } else if (g_model)
      inject(0x82, inst, g_host, g_self, fkt, 0xD, d, dn < 8 ? dn : 8);
  }
}
static int sSend(void *, int) { return 0; }
static void sDtor(void *) {}
static void logLine(const char *m) { printf("# %s\n", m); }
static void layoutLine() {
  printf("LAYOUT %s GUESS %d HANDLES %u %u %u %u %u %u\n", g_6wb ? "6wb" : "6wa", g_layoutGuess, hFrame, hStreet, hDist, hUnit,
         hBufA, hBufB);
}
int main() {
  g_log = logLine;
  S.streamCtor = sCtor; S.streamWrite = sWrite; S.streamSend = sSend; S.streamDtor = sDtor;
  static char cli[0x400];
  g_cli = cli; g_attached = 1; g_registered = 1; g_client = 7; g_self = 0x205e; g_host = 0x0101; g_hostOf[0] = 0x0101;
  g_concave = true;
  char line[256];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\n")] = 0;
    char cmd[16] = ""; unsigned a = 0;
    sscanf(line, "%15s %x", cmd, &a);
    if (!strcmp(cmd, "clock")) g_fakeMs = (unsigned)strtoul(line + 6, 0, 10);
    else if (!strcmp(cmd, "model")) g_model = (int)a;
    else if (!strcmp(cmd, "refuse")) g_refuse = (int)a;
    else if (!strcmp(cmd, "two")) g_hostOf[1] = 0x0102;          // 82/02 in the registry too (BMW F10: both on the cluster)
    else if (!strcmp(cmd, "refusetext")) g_refuseText = (int)a;
    else if (!strcmp(cmd, "street")) { setText(hStreet, line + 7); printf("REDO %d STEP %d\n", g_textRedo, g_encStep); g_textRedo = 0; }
    else if (!strcmp(cmd, "readenc")) readTextEnc();
    else if (!strcmp(cmd, "hud2")) { unsigned w = 0, h = 0; sscanf(line + 5, "%u %u", &w, &h); g_hud2W = w; g_hud2H = h; }
    else if (!strcmp(cmd, "askm")) askMirrorSize();
    else if (!strcmp(cmd, "readbox2")) readBox2();
    else if (!strcmp(cmd, "switches")) chooseBox();
    else if (!strcmp(cmd, "guide")) { Guide g; bool ok = readGuide(g); printf("GUIDE %d FROM %u ROAD %s\n", ok, g.roadFrom, g.road); }
    else if (!strcmp(cmd, "fixed")) { g_mirrorFixed = 1; g_mirror = 0; }   // hud_inst "0"
    else if (!strcmp(cmd, "layout6wb")) { use6wb("switch file hud_layout"); layoutLine(); }
    else if (!strcmp(cmd, "detect")) { detectLayout(); layoutLine(); }
    else if (!strcmp(cmd, "retry")) { printf("SWITCHED %d\n", layoutRetry(nowMs())); layoutLine(); }
    else if (!strcmp(cmd, "refused")) { printf("SWITCHED %d\n", refusedSwitch()); layoutLine(); }
    else if (!strcmp(cmd, "draw")) {
      static Guide g; memset(&g, 0, sizeof g); g.active = true; g.type = 3; g.nl = 3;
      for (unsigned i = 0; i < 3; i++) { g.lanes[i].status = i == 1 ? 2 : 0; g.lanes[i].hl = 1000; g.lanes[i].na = 1; g.lanes[i].ang[0] = 0; }
      copyS(g.road, sizeof g.road, "Main St");
      drawManeuver(g, "1.5", "km");
      printf("SENT %u\n", g_sent);
    } else if (!strcmp(cmd, "foreign")) {
      unsigned char d[6] = { 0, 0x1f, 0, 0x81, 3, 0 };
      d[3] = (unsigned char)(g_fakeMs / 7);  // not one of ours
      inject(0x82, 0, 0x2000, 0x0101, (unsigned short)a, 6, d, sizeof d);
      printf("NAV %d\n", g_navMs && nowMs() - g_navMs < 5000);
    } else if (!strcmp(cmd, "nav")) printf("NAV %d\n", g_navMs && nowMs() - g_navMs < 5000);
    else if (!strcmp(cmd, "shown")) g_shown = a != 0;
    else if (!strcmp(cmd, "cover")) printf("COVER %d SHOWN %d\n", coverStep(nowMs()), g_shown);
    fflush(stdout);
  }
}
'''


class Hud6wbPath(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang++'):
            raise unittest.SkipTest('clang++ not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='hud-6wb-')
        d = Path(cls.tmp.name)
        (d / 'h.cpp').write_text(HARNESS)
        cls.exe = d / 'h'
        subprocess.run(['clang++', '-std=c++11', '-w', '-DHUD_FAKE_CLOCK', '-DHUD_SWITCHES="%s/"' % d, '-DHUD_PPS="%s/routeguidance"' % d, '-I', str(ROOT),
                        '-I', str(ROOT / 'src'), '-o', str(cls.exe), str(d / 'h.cpp')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_h(self, *cmds):
        r = subprocess.run([str(self.exe)], input='\n'.join(cmds) + '\n', capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.splitlines()

    @staticmethod
    def calls(out):
        """the calls sent: (transmission, instance, function, bytes)"""
        res = []
        for l in out:
            p = l.split()
            if len(p) >= 3 and p[0].isdigit() and len(p[1]) == 2 and len(p[2]) == 3:
                res.append((int(p[0]), int(p[1], 16), int(p[2], 16), bytes(int(x, 16) for x in p[3:])))
        return res

    @staticmethod
    def line(out, head):
        return [l for l in out if l.startswith(head)]

    def test_size_question_with_transmission_4_finds_the_6wb(self):
        out = self.run_h('model 1', 'detect')
        c04 = [(tx, d) for tx, _, f, d in self.calls(out) if f == 0xC04]
        self.assertEqual(sorted(set(tx for tx, _ in c04)), [0, 4])
        self.assertIn((4, bytes([0, 0x1f, 1])), c04)
        self.assertEqual(self.line(out, 'LAYOUT'), ['LAYOUT 6wb GUESS 0 HANDLES 23 26 27 28 31 32'])

    def test_unanswered_is_a_guess_asked_again(self):
        out = self.run_h('model 0', 'detect', 'clock 2000', 'retry', 'clock 12000', 'retry', 'model 1', 'clock 22100', 'retry',
                         'retry')
        layouts = self.line(out, 'LAYOUT')
        self.assertTrue(layouts[0].startswith('LAYOUT 6wa GUESS 1 HANDLES 1 2 3 4 5 6'), layouts)
        self.assertEqual(self.line(out, 'SWITCHED'), ['SWITCHED 0', 'SWITCHED 0', 'SWITCHED 0', 'SWITCHED 1'])
        self.assertTrue(layouts[-1].startswith('LAYOUT 6wb GUESS 0'), layouts)
        self.assertEqual(sum(1 for _, _, f, _ in self.calls(out) if f == 0xC04), 8 * 3)     # the first question, then twice again

    def test_refused_6wa_handles_switch_to_the_6wb(self):
        out = self.run_h('model 0', 'refuse 1', 'detect', 'draw', 'refused', 'draw')
        self.assertEqual(self.line(out, 'SWITCHED'), ['SWITCHED 1'])
        self.assertTrue(self.line(out, 'LAYOUT')[-1].startswith('LAYOUT 6wb GUESS 0 HANDLES 23 26 27 28 31 32'))
        cut = [i for i, l in enumerate(out) if l.startswith('SWITCHED')][0]
        before = {d[:2] for _, _, f, d in self.calls(out[:cut]) if f == 0x21D}
        after = {d[:2] for _, _, f, d in self.calls(out[cut:]) if f == 0x21D}
        self.assertEqual(before, {bytes([0, 5])})                                          # the 6WA picture buffer
        self.assertEqual(after, {bytes([0, 0x1f])})                                        # then the 6WB plane

    def test_answered_6wa_is_not_switched(self):
        out = self.run_h('model 0', 'refuse 0', 'detect', 'draw', 'refused')
        self.assertEqual(self.line(out, 'SWITCHED'), ['SWITCHED 0'])     # nothing refused (a silent cluster: the summary line says so)

    def test_6wb_plane_shown_with_set_visibility(self):
        out = self.run_h('model 2', 'detect', 'draw')
        c = [(f, d) for _, _, f, d in self.calls(out) if f in (0x20E, 0x204)]
        i = [k for k, (f, d) in enumerate(c) if f == 0x20E][0]
        self.assertEqual(c[i], (0x20E, bytes([0, 0x20, 0, 0x1f])))                         # swap: 0x20 hidden, 0x1F shown
        self.assertEqual(c[i + 1:i + 3], [(0x204, bytes([0, 0x1f, 1])), (0x204, bytes([0, 0x20, 0]))])

    def test_f10_6wb_answering_both_is_the_6wb(self):
        # 1.3.8, issue #2: 0x1F answered 221 x 158 and 5 answered 126 x 126; 1.3.6 took it for a 6WA and drew unseen on 1..6
        out = self.run_h('model 3', 'detect', 'draw')
        self.assertEqual(self.line(out, 'LAYOUT'), ['LAYOUT 6wb GUESS 0 HANDLES 23 26 27 28 31 32'])
        self.assertTrue([l for l in out if '0x1f is 221 x 158' in l])
        self.assertEqual({d[:2] for _, _, f, d in self.calls(out) if f == 0x21D}, {bytes([0, 0x1f])})

    def test_6wb_copies_to_82_02_when_registered(self):
        out = self.run_h('model 3', 'two', 'detect', 'draw')
        pics = [(i, d[:2]) for _, i, f, d in self.calls(out) if f == 0x21D]
        self.assertEqual({i for i, _ in pics}, {0, 2})                                      # drawn on 82/00 and copied to 82/02
        self.assertEqual({h for i, h in pics if i == 2}, {bytes([0, 0x1f])})                # with the same 6WB handles
        self.assertTrue([l for l in out if 'copies to HUDTerminal 82/02 at device 0102 with the same 6WB handles' in l])
        self.assertFalse([1 for _, i, f, _ in self.calls(out) if f == 0xC04 and i == 2])  # the size question stays on 82/00

    def test_6wb_switch_file_starts_copies_too(self):
        out = self.run_h('two', 'layout6wb', 'draw')
        self.assertIn(2, {i for _, i, f, _ in self.calls(out) if f == 0x21D})

    def test_no_copies_without_82_02_or_with_hud_inst(self):
        for cmds in (('model 3', 'detect', 'draw'), ('model 3', 'two', 'fixed', 'detect', 'draw')):
            out = self.run_h(*cmds)
            self.assertEqual({i for _, i, _, _ in self.calls(out)}, {0}, cmds)

    def test_6wa_never_copies(self):
        out = self.run_h('model 0', 'two', 'detect', 'draw')                               # a 6WA guess: 82/02 is not drawn on
        self.assertEqual({i for _, i, _, _ in self.calls(out)}, {0})

    # 1.3.9, issue #26: Chinese street names showed as "?????" on a 6WB (all its texts went as ISO-8859-15)
    def texts(self, out):
        return [d for _, i, f, d in self.calls(out) if f == 0x210 and i == 0]

    def test_6wb_latin_street_keeps_iso_8859_15(self):
        out = self.run_h('clock 1000', 'layout6wb', 'street Hauptstraße 5 €')
        self.assertEqual(self.texts(out), [bytes([0, 0x1a, 1]) + 'Hauptstraße 5 '.encode('latin-1') + b'\xa4\x00'])

    def test_6wb_chinese_street_goes_as_utf16(self):
        out = self.run_h('clock 1000', 'layout6wb', 'street 京港澳高速 G4')
        self.assertEqual(self.texts(out), [bytes([0, 0x1a, 0]) + '京港澳高速 G4'.encode('utf-16-be') + b'\x00\x00'])
        self.assertNotIn(b'?', self.texts(out)[0])

    def test_6wb_refused_utf16_then_utf8_then_latin_only(self):
        out = self.run_h('clock 1000', 'refusetext 1', 'layout6wb', 'street 中山路', 'street 中山路', 'refusetext 3',
                         'clock 2000', 'street G4 京港澳高速', 'street G4 京港澳高速')
        t = self.texts(out)
        self.assertEqual(t[0][2], 0)                                                           # UTF-16, refused
        self.assertEqual(t[1], bytes([0, 0x1a, 2]) + '中山路'.encode('utf-8') + b'\x00')        # then UTF-8
        self.assertEqual(t[2][2], 2)                                                           # UTF-8 refused too
        self.assertEqual(t[3], bytes([0, 0x1a, 1]) + b'G4\x00')                               # only what fits, no '?'
        self.assertEqual(self.line(out, 'REDO'), ['REDO 1 STEP 1', 'REDO 0 STEP 1', 'REDO 1 STEP 2', 'REDO 0 STEP 2'])

    def test_6wb_all_chinese_in_latin_only_is_empty(self):
        Path(self.tmp.name, 'hud_text_enc').write_text('latin\n')
        try:
            out = self.run_h('clock 1000', 'readenc', 'layout6wb', 'street 中山路')
        finally:
            Path(self.tmp.name, 'hud_text_enc').unlink()
        self.assertEqual(self.texts(out), [bytes([0, 0x1a, 1, 0])])

    def test_6wb_switch_utf8_is_fixed(self):
        Path(self.tmp.name, 'hud_text_enc').write_text('utf8\n')
        try:
            out = self.run_h('clock 1000', 'readenc', 'refusetext 2', 'layout6wb', 'street 中山路', 'street 中山路')
        finally:
            Path(self.tmp.name, 'hud_text_enc').unlink()
        self.assertEqual([d[2] for d in self.texts(out)], [2, 2])                             # no fallback when fixed
        self.assertTrue([l for l in out if 'UTF-8 (switch hud_text_enc)' in l])

    def test_6wa_street_stays_utf16(self):
        out = self.run_h('clock 1000', 'street 中山路')
        self.assertEqual(self.texts(out), [bytes([0, 2, 0]) + '中山路'.encode('utf-16-be') + b'\x00\x00'])

    # 1.3.9, issue #26: the HUD (82/02) cut the copied arrow off at about y 90 of the plane; the cluster showed it whole
    @staticmethod
    def points(out, inst):
        pts = []
        for _, i, f, d in Hud6wbPath.calls(out):
            if f == 0x21D and i == inst and d[:2] in (bytes([0, 0x1f]), bytes([0, 0x20])):
                k = d[5]
                pts += [(int.from_bytes(d[6 + 4 * j:8 + 4 * j], 'big'), int.from_bytes(d[8 + 4 * j:10 + 4 * j], 'big')) for j in range(k)]
        return pts

    def box(self, pts):
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return min(xs), min(ys), max(xs), max(ys)

    def test_copies_go_into_the_upper_box_by_default(self):
        out = self.run_h('model 3', 'two', 'detect', 'draw')
        x0, y0, x1, y1 = self.box(self.points(out, 0))
        self.assertGreater(y1, 100)                                                         # the cluster: the whole plane box
        c = self.points(out, 2)
        self.assertTrue(c)
        bx = self.box(c)
        self.assertGreaterEqual(bx[0], 88); self.assertGreaterEqual(bx[1], 12)
        self.assertLessEqual(bx[2], 144); self.assertLessEqual(bx[3], 82)                  # above where the HUD cut it
        self.assertEqual(len(c), len(self.points(out, 0)))                                 # every point copied, none dropped

    def test_copies_fitted_to_a_smaller_82_02_buffer(self):
        out = self.run_h('model 3', 'two', 'detect', 'hud2 221 90', 'askm', 'draw')
        self.assertTrue([l for l in out if "82/02's picture buffer is 221 x 90" in l])
        self.assertTrue([l for l in out if 'box (79,5)-(142,84)' in l])
        bx = self.box(self.points(out, 2))
        self.assertTrue(79 <= bx[0] and 5 <= bx[1] and bx[2] <= 142 and bx[3] <= 84, bx)
        self.assertTrue([l for l in out if l.startswith('0 02 c04') or l.startswith('4 02 c04')])  # the question went to 82/02

    def test_full_plane_answer_keeps_the_default_box(self):
        out = self.run_h('model 3', 'two', 'detect', 'hud2 221 158', 'askm', 'draw')
        self.assertLessEqual(self.box(self.points(out, 2))[3], 82)

    def test_switch_hud_box_2(self):
        for word, check in (('full\n', None), ('60 10 160 135\n', (60, 10, 160, 135))):
            Path(self.tmp.name, 'hud_box_2').write_text(word)
            try:
                out = self.run_h('model 3', 'two', 'readbox2', 'detect', 'draw')
            finally:
                Path(self.tmp.name, 'hud_box_2').unlink()
            if check is None:
                self.assertEqual(self.points(out, 2), self.points(out, 0))                  # as on the cluster
            else:
                bx = self.box(self.points(out, 2))
                self.assertTrue(check[0] <= bx[0] and check[1] <= bx[1] and bx[2] <= check[2] and bx[3] <= check[3], bx)
                self.assertGreater(bx[3], 100)

    # 1.3.11, st1ng issue #27 (1903, Korea) and #25: no street on the HUD
    def guide(self, text, *before):
        Path(self.tmp.name, 'routeguidance').write_text(text)
        try:
            out = self.run_h(*before, 'guide')
        finally:
            Path(self.tmp.name, 'routeguidance').unlink()
        return self.line(out, 'GUIDE')[0]

    PPS = ('@routeguidance\nroute_state:n:1\nmaneuver_index:n:2\nmaneuver_type:n:2\ncurrent_road::%s\nmaneuver_road::%s\n'
           'maneuver_description::%s\n')

    def test_street_falls_back_to_the_description(self):
        self.assertEqual(self.guide(self.PPS % ('', 'Oak Avenue', 'Turn right')), 'GUIDE 1 FROM 0 ROAD Oak Avenue')
        self.assertEqual(self.guide(self.PPS % ('Elm Street', '', 'Turn right')), 'GUIDE 1 FROM 1 ROAD Elm Street')
        self.assertEqual(self.guide(self.PPS % ('', '', '우회전')), 'GUIDE 1 FROM 2 ROAD 우회전')
        self.assertEqual(self.guide(self.PPS % ('', '', '')), 'GUIDE 1 FROM 3 ROAD ')

    def test_record_padded_with_nuls_reads_the_same(self):
        # 1.4.4: librghook writes the file in /dev/shmem as one fixed 2048-byte record (no rename there): the text, then NULs, and
        # whatever an older, longer record left after them is overwritten too; hud.h reads up to the first NUL
        text = self.PPS % ('', 'Oak Avenue', 'Turn right')
        rec = text.encode() + b'\0' * (2048 - len(text.encode()))
        Path(self.tmp.name, 'routeguidance').write_bytes(rec)
        try:
            out = self.run_h('guide')
        finally:
            Path(self.tmp.name, 'routeguidance').unlink()
        self.assertEqual(self.line(out, 'GUIDE')[0], 'GUIDE 1 FROM 0 ROAD Oak Avenue')

    def test_switch_hud_street_road_only(self):
        Path(self.tmp.name, 'hud_street').write_text('road\n')
        try:
            self.assertEqual(self.guide(self.PPS % ('', '', 'Turn right'), 'switches'), 'GUIDE 1 FROM 3 ROAD ')
        finally:
            Path(self.tmp.name, 'hud_street').unlink()

    def test_hangul_romanized(self):
        Path(self.tmp.name, 'hud_text_enc').write_text('roman\n')
        try:
            out = self.run_h('clock 1000', 'readenc', 'street 강남대로 123 서울역')
        finally:
            Path(self.tmp.name, 'hud_text_enc').unlink()
        self.assertEqual(self.texts(out), [bytes([0, 2, 0]) + 'Gangnamdaero 123 Seoulyeok'.encode('utf-16-be') + b'\x00\x00'])

    def test_hangul_refused_then_romanized(self):
        out = self.run_h('clock 1000', 'refusetext 4', 'street 세종대로', 'street 세종대로')
        t = self.texts(out)
        self.assertEqual(t[0], bytes([0, 2, 0]) + '세종대로'.encode('utf-16-be') + b'\x00\x00')  # Hangul first (a Korean HUD has it)
        self.assertEqual(t[1], bytes([0, 2, 0]) + 'Sejongdaero'.encode('utf-16-be') + b'\x00\x00')
        self.assertEqual(self.line(out, 'REDO')[0], 'REDO 1 STEP 0')

    def test_hangul_on_6wb_refused_goes_romanized_latin(self):
        out = self.run_h('clock 1000', 'refusetext 1', 'layout6wb', 'street 테헤란로', 'street 테헤란로')
        t = self.texts(out)
        self.assertEqual(t[0][2], 0)
        self.assertEqual(t[1], bytes([0, 0x1a, 1]) + b'Teheranro\x00')                        # ISO-8859-15 once in Latin

    def test_hangul_accepted_stays_hangul(self):
        out = self.run_h('clock 1000', 'street 세종대로', 'street 세종대로')
        self.assertEqual({d for d in self.texts(out)}, {bytes([0, 2, 0]) + '세종대로'.encode('utf-16-be') + b'\x00\x00'})

    # 1.4.0: "Car and HUD type" (switch file hud_car)
    def with_car(self, word, *cmds, inst=None):
        Path(self.tmp.name, 'hud_car').write_text(word + '\n')
        if inst is not None:
            Path(self.tmp.name, 'hud_inst').write_text(inst + '\n')
        try:
            return self.run_h(*cmds)
        finally:
            Path(self.tmp.name, 'hud_car').unlink()
            if inst is not None:
                Path(self.tmp.name, 'hud_inst').unlink()

    def test_car_6wb_without_asking(self):
        out = self.with_car('6wb', 'model 0', 'switches', 'detect', 'draw')            # a silent cluster: 6WB all the same
        self.assertTrue(self.line(out, 'LAYOUT')[-1].startswith('LAYOUT 6wb GUESS 0'))
        self.assertTrue([l for l in out if 'car type BMW 6WB cluster (menu' in l])
        self.assertFalse([1 for _, _, f, _ in self.calls(out) if f == 0xC04])          # no size question

    def test_car_6wb_hud_copies_to_82_02(self):
        out = self.with_car('6wb_hud', 'model 0', 'two', 'switches', 'detect', 'draw')
        self.assertEqual({i for _, i, f, _ in self.calls(out) if f == 0x21D}, {0, 2})
        self.assertTrue([l for l in out if 'copies to 82/02 (car type BMW 6WB cluster + HUD)' in l])

    def test_car_6wb_no_copies_even_with_82_02(self):
        out = self.with_car('6wb', 'model 3', 'two', 'switches', 'detect', 'draw')
        self.assertEqual({i for _, i, _, _ in self.calls(out)}, {0})

    def test_car_6wa_and_mini(self):
        out = self.with_car('6wa', 'model 3', 'switches', 'detect', 'draw')            # a cluster that would answer as a 6WB
        self.assertTrue(self.line(out, 'LAYOUT')[-1].startswith('LAYOUT 6wa'))
        self.assertTrue([l for l in out if 'picture box' in l and 'car type BMW' in l])
        out = self.with_car('MINI', 'model 0', 'switches', 'detect', 'draw')
        self.assertTrue([l for l in out if 'picture box MINI' in l and 'car type MINI' in l])
        self.assertTrue([l for l in out if 'cluster layout 6WA (car type MINI)' in l])

    def test_car_auto_and_unknown_detect(self):
        for w in ('auto', 'tesla'):
            out = self.with_car(w, 'model 3', 'switches', 'detect', 'draw')
            self.assertTrue(self.line(out, 'LAYOUT')[-1].startswith('LAYOUT 6wb'), w)  # found by asking, as without the file
            self.assertTrue([1 for _, _, f, _ in self.calls(out) if f == 0xC04], w)

    def test_hud_inst_decides_over_car(self):
        out = self.with_car('6wb_hud', 'model 0', 'two', 'switches', 'detect', 'draw', inst='0')
        self.assertEqual({i for _, i, _, _ in self.calls(out)}, {0})                    # hud_inst "0": no copies

    def test_burst_of_picture_calls_is_the_bmw_navigation(self):
        out = self.run_h('clock 1000', 'foreign 210', 'foreign 204', 'clock 1500', 'foreign c06', 'clock 2000', 'foreign 21d',
                         'clock 2500', 'foreign 21d', 'clock 3000', 'foreign 220', 'clock 3500', 'foreign 21d',
                         'clock 8000', 'foreign 227', 'clock 12000', 'foreign 210', 'clock 17100', 'nav')
        self.assertEqual(self.line(out, 'NAV'), ['NAV 0', 'NAV 0', 'NAV 0', 'NAV 0', 'NAV 0', 'NAV 0', 'NAV 1', 'NAV 1', 'NAV 1',
                                                 'NAV 0'])
        logged = [l for l in out if 'other client' in l]
        self.assertEqual(len(logged), 8)
        self.assertIn('not counted alone', logged[0])
        self.assertIn('the BMW navigation draws', logged[6])

    def test_single_calls_now_and_then_never_start_a_wait(self):
        cmds = []
        for k in range(30):
            cmds += ['clock %d' % (1000 + k * 2000), 'foreign %s' % ('210', '204', 'c06', '21d', '227')[k % 5]]
        out = self.run_h(*cmds)
        self.assertEqual(set(self.line(out, 'NAV')), {'NAV 0'})

    def test_a_warning_over_the_picture_covers_it(self):
        # 1.4.5, ghackt issue #5 (G30): a speed camera warning, a few calls by another client while the CarPlay picture is up
        out = self.run_h('clock 1000', 'shown 1', 'cover',
                         'foreign 21d', 'foreign c06', 'cover',                       # covered: ours waits, picture forgotten
                         'clock 3000', 'foreign 210', 'cover',                         # the warning still drawing
                         'clock 6500', 'cover',                                        # 3.5 s quiet: still waits
                         'clock 7100', 'cover')                                        # 4 s quiet: draws again (whole)
        self.assertEqual(self.line(out, 'NAV'), ['NAV 0', 'NAV 0', 'NAV 0'])           # not the BMW navigation's wait
        self.assertEqual(self.line(out, 'COVER'), ['COVER 0 SHOWN 1', 'COVER 1 SHOWN 0', 'COVER 1 SHOWN 0', 'COVER 1 SHOWN 0',
                                                   'COVER 0 SHOWN 0'])
        self.assertTrue([l for l in out if 'drew over the CarPlay picture' in l])
        self.assertTrue([l for l in out if 'the other client is done' in l])

    def test_no_cover_without_a_picture_and_a_sender_that_never_stops(self):
        out = self.run_h('clock 1000', 'foreign 21d', 'cover')                         # no CarPlay picture up: nothing to cover
        self.assertEqual(self.line(out, 'COVER'), ['COVER 0 SHOWN 0'])
        cmds = ['clock 1000', 'shown 1']
        for k in range(20):                                                            # a call every second, 20 s long
            cmds += ['clock %d' % (1000 + k * 1000), 'foreign 210', 'cover']
        out = self.run_h(*cmds)
        cov = [l.split()[1] for l in self.line(out, 'COVER')]
        self.assertEqual(cov[:16], ['1'] * 16)
        self.assertEqual(set(cov[17:]), {'0'})                                        # ignored after 15 s, as before 1.4.5
        self.assertTrue([l for l in out if 'keeps drawing: ignored for this session' in l])

    def test_own_calls_coming_back_late_are_ours(self):
        # a 6WB picture with lanes is more than 32 calls; every call came back with 0x2000 (sWrite): none counted as another client
        out = self.run_h('model 2', 'detect', 'draw', 'draw', 'nav')
        sent = int(self.line(out, 'SENT')[-1].split()[1])
        self.assertGreater(sent, 64)
        self.assertEqual(self.line(out, 'NAV'), ['NAV 0'])
        self.assertFalse([l for l in out if 'other client' in l])


if __name__ == '__main__':
    unittest.main()
