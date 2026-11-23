"""1.4.4, ghackt issue #4 (2026-11-21, KakaoNavi, ID5_2211-490 Build 13, HUD-only): the phone showed a roundabout's 9 or 2 o'clock exit, the
HUD a roundabout straight through. The picture turns by JunctionElementExitAngle (0x5202 field 11); without it (or 0) librghook
(src/rghook.c, exit_fallback) now takes the angle from the description's clock direction ("9시 방향", "<n> o'clock") or from roundabout
exit n on the junction's roads. Built for this computer with test_rghook_lanes' harness."""
import os, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

from test_rghook_lanes import HARNESS, ROOT, tlv, current


def man(idx, mtype, desc='', exit_angle=None, angles=(), side=0, jtype=None):
    m = tlv(0, struct.pack('>H', 0x0F0E)) + tlv(1, struct.pack('>H', idx)) + tlv(3, bytes([mtype]))
    if desc:
        m += tlv(2, desc.encode() + b'\0')
    if exit_angle is not None:
        m += tlv(11, struct.pack('>h', exit_angle))
    if angles:
        m += tlv(10, b''.join(struct.pack('>h', a) for a in angles))
    if jtype is not None:
        m += tlv(9, bytes([jtype]))
    return m + tlv(8, bytes([side]))


class Roundabout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang'):
            raise unittest.SkipTest('clang not found')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rghook-rb-')
        d = Path(cls.tmp.name)
        (d / 'h.c').write_text(HARNESS)
        cls.exe = d / 'h'
        subprocess.run(['clang', '-w', '-I', str(ROOT / 'src'), '-o', str(cls.exe), str(d / 'h.c')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def pub(self, m):
        pps = Path(self.tmp.name) / 'routeguidance'
        if pps.exists():
            pps.unlink()
        args = [str(self.exe), '5202', m.hex(), '5201', current(1).hex()]
        subprocess.run(args, check=True, env=dict(os.environ, RGHOOK_PPS=str(pps)))
        out = dict(l.split(':', 2)[0::2] for l in pps.read_text().split('\0')[0].splitlines() if l.count(':') >= 2)
        return int(out['maneuver_exit_angle']), out.get('maneuver_exit_angle_from')

    def test_iphones_own_angle_is_kept(self):
        self.assertEqual(self.pub(man(1, 6, '9시 방향으로 진출', exit_angle=45)), (45, None))
        self.assertEqual(self.pub(man(1, 2, '2시 방향', exit_angle=0)), (0, None))                # a right turn: its own picture

    def test_korean_clock_direction(self):
        self.assertEqual(self.pub(man(1, 6, '회전교차로에서 9시 방향으로 진출')), (-90, 'description'))
        self.assertEqual(self.pub(man(1, 29, '회전교차로 2시 방향')), (60, 'description'))
        self.assertEqual(self.pub(man(1, 7, '12시 방향', exit_angle=0)), (0, 'description'))
        self.assertEqual(self.pub(man(1, 6, '회전교차로 10시')), (-60, 'description'))          # a roundabout: 시 alone will do
        self.assertEqual(self.pub(man(1, 3, '1시 방향 도로로 진입')), (30, 'description'))     # straight / follow road: by angle too
        self.assertEqual(self.pub(man(1, 3, '오후 3시 도착')), (0, None))                        # not a direction
        self.assertEqual(self.pub(man(1, 6, '13시 방향')), (0, None))
        self.assertEqual(self.pub(man(1, 0, 'Take the exit at 2 o’clock')), (60, 'description'))
        self.assertEqual(self.pub(man(1, 6, "Exit at the 9 o'clock position")), (-90, 'description'))

    def test_exit_number_on_the_junction(self):
        # right-hand traffic, anticlockwise: from the entry (180) the right road first
        self.assertEqual(self.pub(man(1, 28, angles=(180, 90, 0, -90))), (90, 'exit_number'))
        self.assertEqual(self.pub(man(1, 30, angles=(-90, 0, 90, 180))), (-90, 'exit_number'))
        self.assertEqual(self.pub(man(1, 29, angles=(170, 60, -120))), (-120, 'exit_number'))
        self.assertEqual(self.pub(man(1, 28, angles=(180, 90, 0, -90), side=1)), (-90, 'exit_number'))  # left-hand: clockwise
        self.assertEqual(self.pub(man(1, 33, angles=(180, 90, 0))), (0, None))                  # exit 6 of 2 roads: unknown
        self.assertEqual(self.pub(man(1, 29, '9시 방향', angles=(180, 90, 0))), (-90, 'description'))  # the text first

    def test_other_pictures_untouched(self):
        self.assertEqual(self.pub(man(1, 1, '9시 방향')), (0, None))                             # left turn: a fixed arrow
        self.assertEqual(self.pub(man(1, 8, '9시 방향')), (0, None))


if __name__ == '__main__':
    unittest.main()
