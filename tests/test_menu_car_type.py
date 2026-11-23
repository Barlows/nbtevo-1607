"""1.4.0: "Car and HUD type" (in the main menu since 1.4.2). The menu XMLs, the plugin's table (cpm.cpp kCars), the texts and hud.h's words agree."""
import re, subprocess, sys, tempfile, unittest, zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def app_of(path):
    t = path.read_text()
    return ET.fromstring(t[t.index('<pluginApps'):]).find('pluginApp')


class MenuCarType(unittest.TestCase):
    def cars(self):
        cpm = (ROOT / 'src' / 'cpm.cpp').read_text()
        block = cpm[cpm.index('const CarType kCars[5] = {'):]
        block = block[:block.index('};')]
        return [(int(a), int(m), w, l) for a, m, w, l in re.findall(r'\{ (\d+), (\d+), "([^"]+)", "([^"]+)" \}', block)]

    def test_table(self):
        self.assertEqual([c[2] for c in self.cars()], ['auto', '6wa', '6wb', '6wb_hud', 'mini'])

    def test_both_menus_have_the_submenu(self):
        for f in ('CarPlayMenu.xml', 'CarPlayMenu-hud.xml'):
            app = app_of(ROOT / 'plugin' / f)
            main = app.find("hmiStates/hmiState[@id='20']/components")
            effects = app.find("hmiStates/hmiState[@id='81']/components")
            self.assertIn('120', [c.get('action') for c in main], f)             # 1.4.2: the button in the main menu
            self.assertNotIn('120', [c.get('action') for c in effects], f)       # no longer under HUD effects
            acts = [c.get('action') for c in main]
            self.assertEqual(acts.index('120'), acts.index('80') + 1, f)         # right after HUD effects >
            self.assertEqual(app.find("actions/hmiAction[@id='120']").get('target'), '121', f)
            radios = app.find("hmiStates/hmiState[@id='121']/components").findall('radiobutton')
            got = [(int(r.get('action')), int(r.get('model'))) for r in radios]
            self.assertEqual(got, [(a, m) for a, m, _, _ in self.cars()], f)
            for a, m, _, _ in self.cars():
                self.assertIsNotNone(app.find("actions/raAction[@id='%d']" % a), f)
                self.assertIsNotNone(app.find("models/raBoolModel[@id='%d']" % m), f)

    def test_texts_in_every_language(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d, 't.zip')
            subprocess.run([sys.executable, '-I', str(ROOT / 'src' / 'texts.py'), str(out)], check=True)
            with zipfile.ZipFile(out) as z:
                for n in z.namelist():
                    body = z.read(n).decode('utf-8')
                    for i in range(26, 33):
                        self.assertRegex(body, r'(?m)^%d=.+$' % i, n)
        app = app_of(ROOT / 'plugin' / 'CarPlayMenu.xml')
        tid = {int(m.get('id')): int(m.get('textId')) for m in app.find('models') if m.get('textId')}
        radios = app.find("hmiStates/hmiState[@id='121']/components").findall('radiobutton')
        self.assertEqual([tid[int(r.get('textModel'))] for r in radios], [27, 28, 29, 30, 31])

    def test_restart_carplay_row(self):
        # 1.4.4: "Restart CarPlay" (1.4.3; 1.4.1 / 1.4.2 "Restart head unit") in the main menu right under "Car and HUD type >", in both
        # menus; one Media/Radio entry only (a second pluginApp row landed under NBT-AAD's "Open Android Auto" in the car)
        for f in ('CarPlayMenu.xml', 'CarPlayMenu-hud.xml'):
            t = (ROOT / 'plugin' / f).read_text()
            apps = ET.fromstring(t[t.index('<pluginApps'):]).findall('pluginApp')
            self.assertEqual(len(apps), 1, f)
            self.assertIsNone(apps[0].get('entryButtonFocusID'), f)
            app = app_of(ROOT / 'plugin' / f)
            self.assertNotIn('144', [c.get('action') for c in app.find("hmiStates/hmiState[@id='121']/components")], f)
            comps = list(app.find("hmiStates/hmiState[@id='20']/components"))
            acts = [c.get('action') for c in comps]
            k = acts.index('144')
            self.assertEqual(acts[k - 1], '120', f)                             # right under Car and HUD type
            self.assertEqual((comps[k].tag, comps[k].get('model')), ('button', '145'), f)
            self.assertIsNotNone(app.find("actions/raAction[@id='144']"), f)
            self.assertEqual(app.find("models/textIdModel[@id='145']").get('textId'), '32', f)
        cpm = (ROOT / 'src' / 'cpm.cpp').read_text()
        self.assertIn('const unsigned kActRestartCarPlay = 144;', cpm)
        reg = cpm[cpm.index('d.stub = onRestartCarPlay;'):][:120]
        self.assertIn('self->addHandler(kActRestartCarPlay, d);', reg)
        # registered outside the "if (!kHudOnly)" block: the HUD-only menu has it too
        init = cpm[cpm.index('static const Stub cars[5]'):cpm.index('d.stub = onRestartCarPlay;')]
        self.assertNotIn('kHudOnly', init)
        h = cpm[cpm.index('bool onRestartCarPlay('):]
        h = h[:h.index('\n}\n')]
        self.assertIn('pthread_create(&t, 0, cpr::restartThread, 0)', h)    # NBT-AAD's restart for CarPlay (cprestart.h), a thread
        self.assertNotIn('spawnl', h)                                         # no reboot any more
        self.assertNotIn('kRebootCmd', cpm)
        with tempfile.TemporaryDirectory() as d:                              # its text
            out = Path(d, 't.zip')
            subprocess.run([sys.executable, '-I', str(ROOT / 'src' / 'texts.py'), str(out)], check=True)
            with zipfile.ZipFile(out) as z:
                self.assertIn('32=Restart CarPlay', z.read('en-US.txt').decode('utf-8'))
        # Restart to apply (full menu, restart.sh) still restarts the head unit, with 1.4.2's ECU reset first
        rs = (ROOT / 'plugin' / 'restart.sh').read_text()
        self.assertIn('CPREBOOT_HSFZ=1 LD_PRELOAD=$P/libcpreboot.so /bin/cat < /dev/null 2>> $LOG', rs)

    def test_effect_colour_first_in_hud_effects(self):
        # 1.4.3: Effect colour > above Fill toward the turn and Blink near the turn, in both menus
        for f in ('CarPlayMenu.xml', 'CarPlayMenu-hud.xml'):
            app = app_of(ROOT / 'plugin' / f)
            rows = [(c.tag, c.get('action')) for c in app.find("hmiStates/hmiState[@id='81']/components")]
            self.assertEqual(rows, [('button', '92'), ('checkbox', '84'), ('checkbox', '88'), ('checkbox', '108')], f)

    def test_hud_reads_the_same_words(self):
        hud = (ROOT / 'src' / 'hud.h').read_text()
        rc = hud[hud.index('void readCar() {'):]
        rc = rc[:rc.index('\n}\n')]
        for w in ('6wb_hud', '6wb', '6wa', 'mini'):
            self.assertIn('wordIs(w, "%s")' % w, rc)
        self.assertLess(rc.index('"6wb_hud"'), rc.index('"6wb")'))   # the longer word first (wordIs matches a prefix)


if __name__ == '__main__':
    unittest.main()
