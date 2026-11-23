"""1.4.4: the process wrappers (mm-ipod with librghook.so, Connectivity with libcppinch.so) start the process even when a file of ours is
gone, and the plugin's copy of the binary is replaced whole.

The wrapper texts are taken from the sources (src/cpm.cpp shadowOn's format, carplay-advanced.sh's HUD-only here-document), their
paths pointed at folders here, and run with this computer's sh (QNX's ksh is not here; the scripts use only POSIX sh). The binary
they exec is a stand-in that prints what it got."""
import os, re, shutil, stat, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SH = shutil.which('dash') or shutil.which('sh')


def c_strings(src, start, end):
    """The C string literals between two markers, joined and unescaped (the format snprintf gets)."""
    seg = src[src.index(start):]
    seg = seg[:seg.index(end)]
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', seg)
    return ''.join(lits).encode().decode('unicode_escape')


def plugin_wrapper(label, preload, pre, bin_, sys_):
    src = (ROOT / 'src' / 'cpm.cpp').read_text()
    fmt = c_strings(src, 'n = snprintf(sc, sizeof sc,', 's.label, s.preload, s.pre, pre + rl')
    rw = '/var/opt/hmi/data/rw/'
    vals = [label, preload, pre, preload[len(rw):], bin_[len(rw):], sys_]
    out, k = '', 0
    for part in re.split(r'(%s)', fmt):
        if part == '%s':
            out += vals[k]
            k += 1
        else:
            out += part
    assert k == len(vals), (k, fmt)
    return out


def installer_wrapper():
    s = (ROOT / 'carplay-advanced.sh').read_text()
    body = s[s.index('cat > "$BACKUP/staged-settings/mm-ipod" <<\'WRAP\'\n'):]
    body = body[body.index('\n') + 1:]
    return body[:body.index('\nWRAP\n') + 1]


class Wrappers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.rw = self.d / 'rw'                     # /var/opt/hmi/data/rw
        self.disk = self.d / 'sda0'                 # /fs/sda0
        (self.rw / 'Plugins' / 'CarPlayMenu').mkdir(parents=True)
        (self.rw / 'carplaymenu' / 'bin').mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def stand_in(self, path, name):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#!%s\necho "RAN %s $* PRELOAD=$LD_PRELOAD"\n' % (SH, name))
        path.chmod(0o755)

    def run_wrapper(self, text, env_preload=''):
        text = (text.replace('/var/opt/hmi/data/rw', str(self.rw)).replace('/fs/sda0', str(self.disk))
                .replace('/var/iap', str(self.d / 'iap')))
        w = self.d / 'wrapper'
        w.write_text(text)
        r = subprocess.run([SH, '-n', str(w)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr + text)
        env = {'PATH': '/usr/bin:/bin'}
        if env_preload:
            env['LD_PRELOAD'] = env_preload
        r = subprocess.run([SH, str(w), '-a', 'b c'], capture_output=True, text=True, env=env, timeout=20)
        return r.stdout.strip()  # (stderr: this computer's ld.so on the stand-in preload files)

    def cases(self, text, lib, copy, orig):
        lib.write_bytes(b'lib')
        self.stand_in(copy, 'copy')
        self.stand_in(orig, 'original')
        out = self.run_wrapper(text, '/x/other.so')
        self.assertEqual(out, 'RAN copy -a b c PRELOAD=%s:/x/other.so' % lib)       # as before: our library first, then the rest
        lib.unlink()
        self.assertEqual(self.run_wrapper(text), 'RAN copy -a b c PRELOAD=')          # library gone: runs without it
        lib.write_bytes(b'lib')
        copy.unlink()
        self.assertEqual(self.run_wrapper(text), 'RAN original -a b c PRELOAD=%s' % lib)  # copy gone: the original from the disk

    def test_plugin_mm_ipod_wrapper(self):
        text = plugin_wrapper('route guidance: mm-ipod', '/var/opt/hmi/data/rw/Plugins/CarPlayMenu/librghook.so',
                              '[ -d /var/iap ] || mkdir -p /var/iap\n', '/var/opt/hmi/data/rw/carplaymenu/bin/mm-ipod',
                              '/opt/conn/bin/carplay/mm-ipod')
        self.assertTrue(text.startswith('#!/bin/ksh\n# CarPlayMenu: route guidance: mm-ipod with '), text)
        self.assertNotIn('/var/iap/pps', text)
        self.cases(text, self.rw / 'Plugins/CarPlayMenu/librghook.so', self.rw / 'carplaymenu/bin/mm-ipod',
                   self.disk / 'opt/conn/bin/carplay/mm-ipod')
        self.assertTrue((self.d / 'iap').is_dir())                                    # rghook.log's folder

    def test_plugin_connectivity_wrapper(self):
        text = plugin_wrapper('touchpad pinch: Connectivity', '/var/opt/hmi/data/rw/Plugins/CarPlayMenu/libcppinch.so', '',
                              '/var/opt/hmi/data/rw/carplaymenu/bin/Connectivity', '/opt/conn/bin/Connectivity')
        self.cases(text, self.rw / 'Plugins/CarPlayMenu/libcppinch.so', self.rw / 'carplaymenu/bin/Connectivity',
                   self.disk / 'opt/conn/bin/Connectivity')

    def test_installer_hud_only_wrapper(self):
        text = installer_wrapper()
        self.assertTrue(text.startswith('#!/bin/ksh\n# CarPlayMenu: route guidance: mm-ipod with librghook.so'), text)
        self.assertNotIn('/var/iap/pps', text)
        self.cases(text, self.rw / 'Plugins/CarPlayMenu/librghook.so', self.rw / 'carplaymenu/bin/mm-ipod',
                   self.disk / 'opt/conn/bin/carplay/mm-ipod')

    def test_copy_is_atomic_and_checked(self):
        src = (ROOT / 'src' / 'cpm.cpp').read_text()
        cp = src[src.index('bool copyFile(const char *from'):]
        cp = cp[:cp.index('\n}\n')]
        self.assertIn('snprintf(tmp, sizeof tmp, "%s.tmp", to);', cp)              # written aside, renamed into place
        self.assertIn('rename(tmp, to)', cp)
        self.assertIn('total != want', cp)                                            # every byte of the original
        sh = src[src.index('bool shadowOn(const Shadow &s)'):]
        sh = sh[:sh.index('\n}\n')]
        self.assertIn('if (have < 0 || (want > 0 && have != want))', sh)              # a cut or outdated copy is made again
        self.assertIn('long want = elfSize(s.sys), have = elfSize(s.bin);', sh)       # from an ELF original only, never a script
        self.assertIn('n > 0 && n < (int)sizeof sc', sh)                              # never a cut script


if __name__ == '__main__':
    unittest.main()
