"""1.4.4: libcppinch.so (src/pinch.c) in Connectivity's input path does no library lookups and no switch-file reads per event.

Until 1.4.4 every touchscreen event (each move of a swipe) and every touchpad event in the pass-through looked the original function up
with dlopen + dlsym, and every knob rotation opened the knob_zoom switch on the disk (two lookups when it is off). Built for this
computer: pinch.c as a shared library, a stand-in libconn_carplay.so with the originals, and a host program that loads both and counts
dlopen calls and switch-file opens (its dlopen / open / clock_gettime / pthread_create are the ones pinch.c binds to: -rdynamic)."""
import subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FAKE_CONN = r'''
int calls_touch, calls_pad, calls_knob, calls_post;
int _ZN18CCarPlayVirtualHID11touchUpdateEhii(void *v, int t, int x, int y) { calls_touch++; return 1; }
int _ZN18CCarPlayVirtualHID14touchPadUpdateEiiiiPhiiS0_i(void *v, int t, int x, int y, int a, unsigned char *b, int c, int d,
                                                         unsigned char *e, int f) { calls_pad++; return 1; }
int _ZN18CCarplayServiceMgr10setKnobEvtEbbbiii(void *m, int s, int h, int b, int x, int y, int w) { calls_knob++; return 1; }
int HIDDevicePostReport(void *d, const unsigned char *r, unsigned n) { calls_post++; return 0; }
'''

MAIN = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
static int n_dlopen, n_flag_open;
static long fake_ms = 5000;
void *dlopen(const char *f, int m) {
  static void *(*real)(const char *, int);
  if (!real) real = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
  if (f && strstr(f, "libconn_carplay")) n_dlopen++;
  return real(f, m);
}
int open(const char *p, int fl, ...) {
  static int (*real)(const char *, int, ...);
  if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
  va_list a; va_start(a, fl); int mode = va_arg(a, int); va_end(a);
  if (p && strstr(p, "carplaymenu/")) n_flag_open++;
  return real(p, fl, mode);
}
int clock_gettime(clockid_t c, struct timespec *t) { t->tv_sec = fake_ms / 1000; t->tv_nsec = (fake_ms % 1000) * 1000000; return 0; }
int pthread_create(void *t, const void *a, void *(*f)(void *), void *arg) { return 0; }  /* no knob thread here */
int main(int argc, char **argv) {
  void *pin = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);    /* first, as LD_PRELOAD puts it: its own names resolve to itself */
  void *conn = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);   /* Connectivity's library, loaded before anything calls in */
  if (!conn || !pin) { printf("load failed %s\n", dlerror()); return 1; }
  n_dlopen = 0;
  int (*tu)(void *, int, int, int) = (int (*)(void *, int, int, int))dlsym(pin, "_ZN18CCarPlayVirtualHID11touchUpdateEhii");
  int (*tp)(void *, int, int, int, int, unsigned char *, int, int, unsigned char *, int) =
      (int (*)(void *, int, int, int, int, unsigned char *, int, int, unsigned char *, int))dlsym(pin, "_ZN18CCarPlayVirtualHID14touchPadUpdateEiiiiPhiiS0_i");
  int (*kn)(void *, int, int, int, int, int, int) = (int (*)(void *, int, int, int, int, int, int))dlsym(pin, "_ZN18CCarplayServiceMgr10setKnobEvtEbbbiii");
  int *ct = (int *)dlsym(conn, "calls_touch"), *cp = (int *)dlsym(conn, "calls_pad"), *ck = (int *)dlsym(conn, "calls_knob");
  char vhid[16] = {0};
  int ok = 0;
  for (int i = 0; i < 300; i++) ok += tu(vhid, i & 1, i, i);            /* a swipe on the touchscreen */
  printf("touch %d sent %d dlopen %d\n", *ct, ok, n_dlopen);
  n_dlopen = 0; ok = 0;
  for (int i = 0; i < 300; i++) ok += tp(vhid, 1, i, i, 0, 0, 0, 0, 0, 0);  /* the touchpad, one finger, before its HID */
  printf("pad %d sent %d dlopen %d\n", *cp, ok, n_dlopen);
  n_dlopen = 0; n_flag_open = 0;
  for (int i = 0; i < 200; i++) kn((void *)vhid, 0, 0, 0, 0, 0, i & 1 ? 1 : -1);  /* knob rotations within one second */
  printf("knob %d dlopen %d flagopen %d\n", *ck, n_dlopen, n_flag_open);
  int before = n_flag_open;
  fake_ms += 1100;
  for (int i = 0; i < 50; i++) kn((void *)vhid, 0, 0, 0, 0, 0, 1);
  printf("later flagopen %d\n", n_flag_open - before);
  return 0;
}
'''


class PinchInputPath(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / 'conn.c').write_text(FAKE_CONN)
        (d / 'main.c').write_text(MAIN)
        cls.conn = d / 'libconn_carplay.so'
        cls.pin = d / 'libcppinch.so'
        cls.exe = d / 'main'
        cc = ['cc', '-w']
        subprocess.run(cc + ['-shared', '-fPIC', '-Wl,-soname,libconn_carplay.so', '-o', str(cls.conn), str(d / 'conn.c')], check=True)
        subprocess.run(cc + ['-shared', '-fPIC', '-o', str(cls.pin), str(ROOT / 'src' / 'pinch.c')], check=True)
        subprocess.run(cc + ['-rdynamic', '-o', str(cls.exe), str(d / 'main.c'), '-ldl'], check=True)
        r = subprocess.run([str(cls.exe), str(cls.conn), str(cls.pin)], capture_output=True, text=True, timeout=30,
                           env={'LD_LIBRARY_PATH': str(d), 'PATH': '/usr/bin:/bin'})
        cls.out = {l.split()[0]: l.split()[1:] for l in r.stdout.splitlines()}
        cls.raw = r.stdout + r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_touchscreen_events_do_one_lookup(self):
        touch, sent, dl = self.out['touch'][0], self.out['touch'][2], self.out['touch'][4]
        self.assertEqual((touch, sent), ('300', '300'), self.raw)               # every event reached the original
        self.assertLessEqual(int(dl), 1, self.raw)                              # was 300: dlopen + dlsym per event

    def test_touchpad_pass_through_does_one_lookup(self):
        pad, sent, dl = self.out['pad'][0], self.out['pad'][2], self.out['pad'][4]
        self.assertEqual((pad, sent), ('300', '300'), self.raw)
        self.assertLessEqual(int(dl), 1, self.raw)

    def test_knob_rotation_reads_its_switches_once_a_second(self):
        knob, dl, opens = self.out['knob'][0], self.out['knob'][2], self.out['knob'][4]
        self.assertEqual(knob, '200', self.raw)                                 # passed on (knob_zoom off)
        self.assertLessEqual(int(dl), 1, self.raw)
        self.assertLessEqual(int(opens), 2, self.raw)                           # was 400: two paths at every rotation
        self.assertLessEqual(int(self.out['later'][1]), 2, self.raw)            # read again after a second, once
        self.assertGreaterEqual(int(self.out['later'][1]), 1, self.raw)


if __name__ == '__main__':
    unittest.main()
