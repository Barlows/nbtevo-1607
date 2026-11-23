"""The plugin's per-build tables (src/cpm.cpp kBuilds, src/tilt_*.json), without firmware, and against a build's own HmiMain3D when
one is given.

No firmware is kept here. The always-run cases check that each tilt patch is self-consistent: the same 19 places, the same new code,
and every branch it writes lands where the patch means it to (key filter -> the function's exit, the direction block -> the send call,
the original argument setup -> the direction block, the trace's branch -> the argument setup). With CPA_HMI_1607 set to the plain
HmiMain3D of ID5_1607-490-1610T Build 153 (sha256 0fa0e6e0...), the 1607 entry is checked word by word against it, as the plugin
checks it in the car; CPA_HMI_1903 (ID5_1903-490-1837J Build 41, sha256 027cf939...) does the same for the 1903 entry, and
CPA_HMI_1807 (ID5_1807-490-1809H Build 29, sha256 5e74cde5...) for the 1807 entry.
"""
import hashlib
import json
import os
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPM = (ROOT / 'src' / 'cpm.cpp').read_text()
TILTS = {name: json.loads((ROOT / 'src' / ('tilt_%s.json' % name)).read_text())['words'] for name in ('006', '1703', '1607', '1903', '1807')}


def kbuilds():
    """kBuilds as {id: [touchFn, ctlSetFn, ctlProxy, playFn, playObj, tdFirst, tdEnd, tdTime, tdOrigin, tilt, fs, pbw, pbwBmw, ini]}."""
    block = CPM[CPM.index('const Build kBuilds[] = {'):]
    block = block[:block.index('\n};')]
    out = {}
    for chunk in block.split('\n  { "')[1:]:
        name = chunk[:chunk.index('"')]
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', chunk[chunk.index('",') + 2:], flags=re.S)
        body = body[body.index('",') + 2:].rstrip().rstrip(',').rstrip('}')
        f = [x.strip() for x in body.split(',') if x.strip()]
        nums = [int(x, 0) if re.match(r'^(0x)?[0-9a-f]+$', x) else x for x in f]
        # touchFn..tdOrigin (9), tilt, its count, fs, its count, pbw, pbwBmw, ini
        out[name] = nums[:9] + [nums[9], nums[11], nums[13], nums[14], nums[15]]
    return out


def word(w):
    return int(w, 16)


def b_target(addr, w):
    off = w & 0xffffff
    if off & 0x800000:
        off -= 1 << 24
    return addr + 8 + 4 * off


class TiltTables(unittest.TestCase):
    def test_same_places_and_code(self):
        new = {k: [w['new'] for w in v] for k, v in TILTS.items()}
        for name, words in TILTS.items():
            self.assertEqual(len(words), 19, name)
            # the key filter's bhi keeps the build's own exit (imm24), the trace's branch its own distance (far in 006); the two words
            # that store "pressed" name the build's own register (r8; r7 in 1903, issue #9); every other new word is the same everywhere
            reg = words[12]['new'][7]                                 # moveq r5, rX at tB + 0x24
            for i in range(18):
                if i == 2:
                    self.assertEqual(words[i]['new'][:2], '8a', name)
                    self.assertEqual(words[i]['new'][2:], words[i]['orig'][2:], name)
                    self.assertEqual(words[i]['orig'][:2], '1a', name)
                elif i == 10:                                         # strls rX, [sp, ip, lsl #2] at tB + 0x1c
                    self.assertEqual(words[i]['new'], '978d%s10c' % reg, name)
                elif i == 12:
                    self.assertEqual(words[i]['new'], '01a0500%s' % reg, name)
                else:
                    self.assertEqual(words[i]['new'], new['1703'][i], (name, i))

    def test_branches_land_where_meant(self):
        for name, words in TILTS.items():
            a = [word(w['addr']) for w in words]
            tA, tB, tC = a[0], a[3], a[18]
            self.assertEqual(a[:3], [tA, tA + 4, tA + 8], name)
            self.assertEqual(a[3:16], [tB + 4 * i for i in range(13)], name)
            self.assertEqual(a[16:18], [tB + 0x54, tB + 0x7c], name)
            # the direction block ends with a jump to the send call (tB + 0xa0), the setup jumps to the block, the trace to the setup
            self.assertEqual(b_target(a[15], word(words[15]['new'])), tB + 0xa0, name)
            self.assertEqual(b_target(a[17], word(words[17]['new'])), tB, name)
            self.assertEqual(b_target(a[18], word(words[18]['new'])), tB + 0x40, name)
            # the original words the patch expects at those places
            self.assertEqual([w['orig'] for w in words[:2]], ['e355000e', '1355000d'], name)
            self.assertEqual(words[16]['orig'], '03833001', name)   # orreq r3, r3, #1 (tilt left = Back)
            self.assertEqual(words[17]['orig'], 'e355003a', name)   # cmp r5, #0x3a
            self.assertEqual(words[18]['orig'], 'e51b302c', name)

    def test_kbuilds_entries(self):
        b = kbuilds()
        self.assertEqual(set(b), {'ID5_1803-510-1749MA Build 71', 'ID5_1703-490-1650ZD Build 139', 'ID5_1607-490-1610T Build 153',
                                  'ID5_1903-490-1837J Build 41', 'ID5_1807-490-1809H Build 29'})
        e = b['ID5_1607-490-1610T Build 153']
        self.assertEqual(e[:9], [0x8b4d24, 0x1ec2a24, 0x4d64, 0x1ec2984, 0x51b4, 8, 0xc, 0x18, 0x1c])
        self.assertEqual(e[9], 'kTilt1607')
        self.assertEqual(e[11:], [0, 3, 1])                         # Full-screen CarPlay on a BMW only: patchCommon1607, carplay.ini
        # the controller and playback proxies keep their distance from 1703's (both 0xfc lower), the send functions theirs (0xa0)
        e7 = b['ID5_1703-490-1650ZD Build 139']
        self.assertEqual(e7[2] - e[2], 0xfc)
        self.assertEqual(e7[4] - e[4], 0xfc)
        self.assertEqual(e7[1] - e7[3], e[1] - e[3])

    def test_kbuilds_1903(self):
        e = kbuilds()['ID5_1903-490-1837J Build 41']
        # the installer's scan of the stock file, checked by hand (issue #9 / #10); 006's CTouchData and proxies
        self.assertEqual(e[:9], [0x44d53c, 0x2137d88, 0x4e60, 0x2137ce8, 0x52b0, 4, 8, 0x14, 0x18])
        self.assertEqual(e[9:], ['kTilt1903', 'kFsCode1903', 1, 1, 0])   # carplay.ini already full width
        self.assertEqual((TILTS['1903'][10]['new'], TILTS['1903'][12]['new']), ('978d710c', '01a05007'))   # "pressed" in r7
        fs = CPM[CPM.index('const Word kFsCode1903[] = {'):]
        fs = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}', fs[:fs.index('};')])
        self.assertEqual(len(fs), 7)
        self.assertEqual([o for _, o, _ in fs], ['0xe5d00108'] * 2 + ['0xe5d43108'] * 2 + ['0xe5d53108', '0xe5d43108', '0xe12fff33'])
        self.assertEqual([n for _, _, n in fs], ['0xe3a00001'] * 2 + ['0xe3a03001'] * 4 + ['0xe3a00001'])

    def test_installer_lists_1903(self):
        s = (ROOT / 'carplay-advanced.sh').read_text()
        table = s[s.index('supported_table() {'):]
        table = table[:table.index('\nEOF')]
        rows = {tuple(l.split()[:2]): l.split()[2] for l in table.splitlines() if l.startswith('ID5_1903')}
        self.assertEqual(rows[('ID5_1903-490-1837J_Build_41', 'HmiMain3D')],
                         '027cf939eb503cb55048fba575b4155d28acfaec62020d40d6ea1caa3204b8cf')
        self.assertEqual(rows[('ID5_1903-490-1837J_Build_41', 'libconn_carplay.so')],
                         '062a20f49fc4f4bab1a7e3b9a5910d12ba8e6b70895a434291c20d83ee0a5f85')
        self.assertEqual(len(rows[('ID5_1903-490-1837J_Build_41', 'scan')]), 64)
        self.assertNotIn(('ID5_1903-490-1837J_Build_41', 'fullscreen'), rows)

    def test_kbuilds_1807(self):
        e = kbuilds()['ID5_1807-490-1809H Build 29']
        # the installer's scan of the stock file (sha256 5e74cde5...), checked by hand: 006_017_006's code moved by 0x2f28
        self.assertEqual(e[:9], [0x43ef54, 0x1fd9fe4, 0x4e60, 0x1fd9f44, 0x52b0, 4, 8, 0x14, 0x18])
        self.assertEqual(e[9:], ['kTilt1807', 'kFsCode1807', 1, 1, 1])
        e6 = kbuilds()['ID5_1803-510-1749MA Build 71']
        self.assertEqual(e[0] - e6[0], 0x2f28)
        self.assertEqual([e[2]] + e[4:9], [e6[2]] + e6[4:9])           # 006's proxies and CTouchData
        self.assertEqual(e[1] - e[3], e6[1] - e6[3])                   # the two send functions 0xa0 apart
        self.assertEqual((TILTS['1807'][10]['new'], TILTS['1807'][12]['new']), ('978d710c', '01a05007'))   # "pressed" in r7
        for w7, w6 in zip(TILTS['1807'], TILTS['006']):
            self.assertIn(int(w7['addr'], 16) - int(w6['addr'], 16), (0x2f28, 0x2f40))
        fs = CPM[CPM.index('const Word kFsCode1807[] = {'):]
        fs = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}', fs[:fs.index('};')])
        f6 = CPM[CPM.index('const Word kFsCode[] = {'):]
        f6 = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}', f6[:f6.index('};')])
        self.assertEqual([(o, n) for _, o, n in fs], [(o, n) for _, o, n in f6])   # 006's words
        self.assertEqual([int(a, 16) - int(b, 16) for (a, _, _), (b, _, _) in zip(fs, f6)], [0x2f28] * 4 + [0x2f40] * 2 + [0x2f28])

    def test_installer_lists_1807(self):
        s = (ROOT / 'carplay-advanced.sh').read_text()
        table = s[s.index('supported_table() {'):]
        table = table[:table.index('\nEOF')]
        rows = {tuple(l.split()[:2]): l.split()[2] for l in table.splitlines() if l.startswith('ID5_1807')}
        self.assertEqual(rows[('ID5_1807-490-1809H_Build_29', 'HmiMain3D')],
                         '5e74cde50c01dbf9c104188fadf50c6f8642c5d0715038d150d85dda4e2112c7')
        self.assertEqual(len(rows[('ID5_1807-490-1809H_Build_29', 'scan')]), 64)
        self.assertNotIn(('ID5_1807-490-1809H_Build_29', 'libconn_carplay.so'), rows)   # not reported yet
        self.assertNotIn(('ID5_1807-490-1809H_Build_29', 'fullscreen'), rows)

    def test_hud_only_note_for_a_supported_build(self):
        # 1.3.9, st1ng issue #25: an app with its own list chose HUD-only on 1807; the script's log now says the build is supported
        import subprocess
        src = (ROOT / 'carplay-advanced.sh').read_text()
        lines = src.splitlines()
        a = lines.index('if [ "$ACTION" = install ] && [ "$MODE" = hud ] && [ "$KIND" != id6 ]; then')
        b = lines.index('fi', a)
        funcs = re.search(r'^supported_table\(\) \{.*?^\}\n', src, re.S | re.M).group(0) + \
            re.search(r'^is_software_name\(\) \{.*?^\}\n', src, re.S | re.M).group(0)
        def run(mode, kind, build):
            sh = 'say() { echo "$*"; }\n' + funcs + f'ACTION=install MODE={mode} KIND={kind}\nhmi_build="{build}"\n' + \
                '\n'.join(lines[a:b + 1]) + '\n'
            return subprocess.run(['sh', '-c', sh], capture_output=True, text=True, check=True).stdout
        b1807 = 'NBTevo_ID5_1807_Release ID5_1807-490-1809H Build 29'
        out = run('hud', 'id5', b1807)
        self.assertIn('install --mode full --kind id5 --hmi ID5_1807-490-1809H_Build_29', out)
        self.assertEqual(run('full', 'id5', b1807), '')
        self.assertEqual(run('hud', 'id6', b1807), '')
        self.assertEqual(run('hud', 'id5', 'NBTevo_ID5_2007_Release ID5_2007-510-2013A Build 26'), '')
        self.assertEqual(run('hud', 'id5', ''), '')

    def test_hud_only_end_message_by_kind(self):
        src = (ROOT / 'carplay-advanced.sh').read_text()
        i = src.index("say 'where the car is coded for apps (A4A). Fullscreen, knob tilts and the touchpad are native on iDrive 6.'")
        self.assertIn('if [ "$KIND" = id6 ]; then', src[i - 200:i])
        self.assertIn('need the full install (--full)', src[i:i + 300])

    def test_installer_lists_1607(self):
        s = (ROOT / 'carplay-advanced.sh').read_text()
        table = s[s.index('supported_table() {'):]
        table = table[:table.index('\nEOF')]
        rows = {tuple(l.split()[:2]): l.split()[2] for l in table.splitlines() if l.startswith('ID5_1607')}
        self.assertEqual(rows[('ID5_1607-490-1610T_Build_153', 'HmiMain3D')],
                         '0fa0e6e0de49235c02a8bcebca588dad18bd5eab512066ab44ed295a09ca3fbd')
        self.assertEqual(rows[('ID5_1607-490-1610T_Build_153', 'libconn_carplay.so')],
                         'bbdbae7df387c448a3eec9dd607f9e7b196b899039a0123e6299d5ad0fa96827')
        self.assertEqual(rows[('ID5_1607-490-1610T_Build_153', 'scan')], 'none')
        self.assertNotIn(('ID5_1607-490-1610T_Build_153', 'fullscreen'), rows)     # full screen since 1.2.12 (BMW F20)


@unittest.skipUnless(os.environ.get('CPA_HMI_1607'), 'CPA_HMI_1607: the plain HmiMain3D of ID5_1607-490-1610T Build 153')
class Build1607(unittest.TestCase):
    """The 1607 entry against its HmiMain3D: what the plugin compares before it writes, and how the places were found."""

    @classmethod
    def setUpClass(cls):
        cls.data = Path(os.environ['CPA_HMI_1607']).read_bytes()
        assert hashlib.sha256(cls.data).hexdigest() == '0fa0e6e0de49235c02a8bcebca588dad18bd5eab512066ab44ed295a09ca3fbd'
        cls.e = kbuilds()['ID5_1607-490-1610T Build 153']

    def u32(self, va):
        return struct.unpack_from('<I', self.data, va - 0x100000)[0]          # text: file offset 0 at 0x100000

    def test_tilt_originals(self):
        for w in TILTS['1607']:
            self.assertEqual('%08x' % self.u32(word(w['addr'])), w['orig'], w['addr'])

    def test_hook_entries(self):
        touch, ctl, _, play = self.e[:4]
        self.assertEqual((self.u32(touch), self.u32(touch + 4)), (0xe92d4ff0, 0xe28db020))   # installTouchHook
        self.assertEqual((self.u32(ctl), self.u32(ctl + 4)), (0xe92d49f0, 0xe28db018))       # installCtlHook
        self.assertEqual(self.u32(play), 0xe92d4870)

    def test_touch_data_offsets(self):
        touch = self.e[0]
        # ldr r1, [fp, #-0x8c] (CTouchData**); ldr r3, [r1]; ldr r3, [r3, #0x1c] (origin); cmp r3, #1
        self.assertEqual([self.u32(touch + 0x20 + 4 * i) for i in range(4)], [0xe51b108c, 0xe5913000, 0xe593301c, 0xe3530001])
        self.assertEqual(self.u32(touch + 0x84), 0xe5933008)        # ldr r3, [r3, #8]: the first finger
        self.assertEqual(self.u32(touch + 0x4a8), 0xe593300c)       # ldr r3, [r3, #0xc]: the end of the list

    def test_controller_send(self):
        tB = word(TILTS['1607'][3]['addr'])
        call = tB + 0xa0
        self.assertEqual(b_target(call, self.u32(call)), self.e[1])                         # bl ctlSetFn
        # add r0, r0, #0x4d00 / add r0, r0, #0x64: the proxy at *(activity + 0xb0) + ctlProxy
        self.assertEqual((self.u32(tB + 0x68), self.u32(tB + 0x78)), (0xe2800c4d, 0xe2800064))
        self.assertEqual(self.u32(tB + 0x4c), 0xe59100b0)                                   # ldr r0, [r1, #0xb0]

    def test_playback_send(self):
        # the HMI's own next-track call: mov r1, #0x8d; add r0, r0, #0x5100; add r0, r0, #0xb4; bl playFn
        call = 0xc5b3e8
        self.assertEqual([self.u32(call - 12 + 4 * i) for i in range(3)], [0xe3a0108d, 0xe2800c51, 0xe28000b4])
        self.assertEqual(b_target(call, self.u32(call)), self.e[3])



@unittest.skipUnless(os.environ.get('CPA_HMI_1903'), 'CPA_HMI_1903: the plain HmiMain3D of ID5_1903-490-1837J Build 41')
class Build1903(unittest.TestCase):
    """The 1903 entry against its HmiMain3D (issues #9, #10): every word the plugin compares before it writes."""

    @classmethod
    def setUpClass(cls):
        cls.data = Path(os.environ['CPA_HMI_1903']).read_bytes()
        assert hashlib.sha256(cls.data).hexdigest() == '027cf939eb503cb55048fba575b4155d28acfaec62020d40d6ea1caa3204b8cf'
        cls.e = kbuilds()['ID5_1903-490-1837J Build 41']

    def u32(self, va):
        return struct.unpack_from('<I', self.data, va - 0x100000)[0]

    def test_tilt_originals(self):
        for w in TILTS['1903']:
            self.assertEqual('%08x' % self.u32(word(w['addr'])), w['orig'], w['addr'])
        tB = word(TILTS['1903'][3]['addr'])
        self.assertEqual(self.u32(tB + 0x84), 0x01a05007)           # moveq r5, r7: "pressed" is r7 here

    def test_fullscreen_originals(self):
        fs = CPM[CPM.index('const Word kFsCode1903[] = {'):]
        for a, o, _ in re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}', fs[:fs.index('};')]):
            self.assertEqual(self.u32(int(a, 16)), int(o, 16), a)
        # the variant query: movw r1, #0x9c71 (CARPLAY_FULLSCREEN) before the blx
        self.assertEqual(self.u32(0x44d5c0), 0xe3091c71)

    def test_hook_entries(self):
        touch, ctl, _, play = self.e[:4]
        self.assertEqual((self.u32(touch), self.u32(touch + 4)), (0xe92d4ff0, 0xe28db020))
        self.assertEqual((self.u32(ctl), self.u32(ctl + 4)), (0xe92d49f0, 0xe28db018))
        self.assertEqual(self.u32(play), 0xe92d4870)

    def test_touch_data_offsets(self):
        touch = self.e[0]
        # ldr r1, [fp, #-0x64] (CTouchData**); ldr r3, [r1]; ldr r3, [r3, #0x18] (origin); cmp r3, #1
        self.assertEqual([self.u32(touch + 0x20 + 4 * i) for i in range(4)], [0xe51b1064, 0xe5913000, 0xe5933018, 0xe3530001])

    def test_controller_send(self):
        tB = word(TILTS['1903'][3]['addr'])
        call = tB + 0xa0
        self.assertEqual(b_target(call, self.u32(call)), self.e[1])
        self.assertEqual((self.u32(tB + 0x68), self.u32(tB + 0x78)), (0xe2800c4e, 0xe2800060))
        self.assertEqual(self.u32(tB + 0x4c), 0xe59100b0)

    def test_playback_send(self):
        call = 0x740f84
        self.assertEqual([self.u32(call - 12 + 4 * i) for i in range(3)], [0xe3a0108d, 0xe2800c52, 0xe28000b0])
        self.assertEqual(b_target(call, self.u32(call)), self.e[3])



@unittest.skipUnless(os.environ.get('CPA_HMI_1807'), 'CPA_HMI_1807: the plain HmiMain3D of ID5_1807-490-1809H Build 29')
class Build1807(unittest.TestCase):
    """The 1807 entry against its HmiMain3D (ghackt/Carplay-advanced-installer issue #2): every word the plugin compares before it writes."""

    @classmethod
    def setUpClass(cls):
        cls.data = Path(os.environ['CPA_HMI_1807']).read_bytes()
        assert hashlib.sha256(cls.data).hexdigest() == '5e74cde50c01dbf9c104188fadf50c6f8642c5d0715038d150d85dda4e2112c7'
        cls.e = kbuilds()['ID5_1807-490-1809H Build 29']

    def u32(self, va):
        return struct.unpack_from('<I', self.data, va - 0x100000)[0]

    def test_tilt_originals(self):
        for w in TILTS['1807']:
            self.assertEqual('%08x' % self.u32(word(w['addr'])), w['orig'], w['addr'])
        tB = word(TILTS['1807'][3]['addr'])
        self.assertEqual(self.u32(tB + 0x84), 0x01a05007)           # moveq r5, r7: "pressed" is r7 here

    def test_fullscreen_originals(self):
        fs = CPM[CPM.index('const Word kFsCode1807[] = {'):]
        for a, o, _ in re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}', fs[:fs.index('};')]):
            self.assertEqual(self.u32(int(a, 16)), int(o, 16), a)
        # the variant query: movw r1, #0x9c71 (CARPLAY_FULLSCREEN) before the blx
        self.assertEqual(self.u32(0x43efd8), 0xe3091c71)

    def test_hook_entries(self):
        touch, ctl, _, play = self.e[:4]
        self.assertEqual((self.u32(touch), self.u32(touch + 4)), (0xe92d4ff0, 0xe28db020))
        self.assertEqual((self.u32(ctl), self.u32(ctl + 4)), (0xe92d49f0, 0xe28db018))
        self.assertEqual(self.u32(play), 0xe92d4870)

    def test_touch_data_offsets(self):
        touch = self.e[0]
        # ldr r1, [fp, #-0x64] (CTouchData**); ldr r3, [r1]; ldr r3, [r3, #0x18] (origin); cmp r3, #1
        self.assertEqual([self.u32(touch + 0x20 + 4 * i) for i in range(4)], [0xe51b1064, 0xe5913000, 0xe5933018, 0xe3530001])

    def test_controller_send(self):
        tB = word(TILTS['1807'][3]['addr'])
        call = tB + 0xa0
        self.assertEqual(b_target(call, self.u32(call)), self.e[1])
        self.assertEqual((self.u32(tB + 0x68), self.u32(tB + 0x78)), (0xe2800c4e, 0xe2800060))
        self.assertEqual(self.u32(tB + 0x4c), 0xe59100b0)

    def test_playback_send(self):
        call = 0x727de4
        self.assertEqual([self.u32(call - 12 + 4 * i) for i in range(3)], [0xe3a0108d, 0xe2800c52, 0xe28000b0])
        self.assertEqual(b_target(call, self.u32(call)), self.e[3])


if __name__ == '__main__':
    unittest.main()
