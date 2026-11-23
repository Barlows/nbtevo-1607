/* libcpreboot.so — reboots the head unit (NBT Evo, run on hu-omap):  LD_PRELOAD=<plugin>/libcpreboot.so /bin/cat
 *
 * Why: in the car the system launcher does not restart the HMI (starterPackageCtrl -d0 -s/-e HMI only flips the package state; the
 * HmiMain3D process keeps running), the MINI ignores holding the volume knob, OnOffDSICommander does not exist on HMI 1703, and
 * /opt/sys/bin/sysshutdown only unmounts the disks and waits for someone else to reset. The links the plugin needs are made at boot
 * (boot.sh from /bin/start_screen.sh), so "Restart to apply" is a reboot.
 * A freestanding LD_PRELOAD (no crt for a standalone binary in this toolchain): the constructor
 * flushes the files the plugin writes (fdatasync) and every filesystem (sync), then calls sysmgr_reboot() (all in libc.so.3). The host command never runs. Exit status 1 if the reboot call returns.
 *
 * 1.4.2, CPREBOOT_HSFZ=1 (the menu's Restart head unit / Restart to apply): first the restart the phone app CarPlay Advanced Installer
 * 1.6.7 sends (dev.nbtevo.manager.HeadUnitReset / HsfzCodec): one UDS ECUReset 0x11 0x01 to ECU 0x63 (the NBT Evo) from tester 0xF4,
 * in a BMW HSFZ frame [length u32 BE = 2 + UDS][control 0x0001][source][target][UDS] on TCP 6801, after the ECU has identified itself
 * with ReadDataByIdentifier 0xF187 (or 0xF190); 0x51 0x01 from 0x63 to 0xF4 is the acknowledgement, 0x7F <sid> 0x78 "pending" is
 * waited out, gateway frames of other controls (the 0x0002 echo) are skipped. Here it goes to the head unit's own HSFZ port:
 * 127.0.0.1, then the unit's other IPv4 addresses (getifaddrs). The menu's reboot from the HMI did not take ("sysmgr_reboot returned",
 * or nothing): the HMI's child is not the installer's root shell; the ECU reset is the unit's own diagnostic service. If it is not
 * acknowledged, or the unit is still up CPREBOOT_WAIT (15) s later, sysmgr_reboot as before. The socket calls are looked up in
 * libsocket at run time (as kombi.h), so the library's imports do not change. Without CPREBOOT_HSFZ (the installer): as before.
 * Host tests (tests/test_reboot_hsfz.py): built with CPREBOOT_HOST against a fake HSFZ server.
 */
#ifdef CPREBOOT_HOST
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
static int sysmgr_reboot(void) {
  write(2, "SYSMGR_REBOOT\n", 14);
  return -1;
}
#else
extern int open(const char *, int, ...);
extern int close(int);
extern int fdatasync(int);
extern long write(int, const void *, unsigned long);
extern unsigned sleep(unsigned);
extern int sysmgr_reboot(void);
extern void sync(void);
extern void _exit(int);
extern char *getenv(const char *);
extern int usleep(unsigned);
extern int fcntl(int, int, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#endif

static void say(const char *s) {
  unsigned long n = 0;
  while (s[n])
    n++;
  write(2, s, n);
}

static const char *const kFlush[] = {
  "/var/opt/hmi/data/rw/carplaymenu", /* the switches and generated files (directory: its entries) */
  "/var/opt/hmi/data/rw/carplaymenu/carplay.ini",
  "/var/opt/hmi/data/rw/carplaymenu/Spec_State_Merged.pbw",
  "/var/opt/hmi/data/rw/Plugins/CarPlayMenu",
  "/var/opt/hmi/data/rw/Plugins/CarPlayMenu/boot.sh",
};

/* ---- HSFZ ECU reset (CPREBOOT_HSFZ=1) ---- */
typedef unsigned char u8;
static int (*so_socket)(int, int, int);
static int (*so_connect)(int, const void *, unsigned);
static long (*so_send)(int, const void *, unsigned long, int);
static long (*so_recv)(int, void *, unsigned long, int);
static int (*so_setsockopt)(int, int, int, const void *, unsigned);
static int (*so_getsockopt)(int, int, int, void *, unsigned *);
static int (*so_getpeername)(int, void *, unsigned *);
static int (*so_getifaddrs)(void **);
static void (*so_freeifaddrs)(void *);
#if defined(CPREBOOT_HOST) && defined(__linux__)
enum { kAfInet = 2, kSockStream = 1, kSolSocket = 1, kSoRcvTimeo = 20, kSoSndTimeo = 21, kSoError = 4, kFGetfl = 3, kFSetfl = 4, kNonblock = 04000 };
struct sa4 { unsigned short family; u8 port[2]; u8 addr[4]; u8 zero[8]; };
#define SA4_FAMILY(p) (((const unsigned short *)(p))[0])
static void sa4_init(struct sa4 *a) { a->family = kAfInet; }
#else
enum { kAfInet = 2, kSockStream = 1, kSolSocket = 0xffff, kSoRcvTimeo = 0x1006, kSoSndTimeo = 0x1005, kSoError = 0x1007, kFGetfl = 3, kFSetfl = 4, kNonblock = 0x80 };
struct sa4 { u8 len, family; u8 port[2]; u8 addr[4]; u8 zero[8]; };  /* BSD sockaddr_in (QNX io-pkt) */
#define SA4_FAMILY(p) (((const u8 *)(p))[1])
static void sa4_init(struct sa4 *a) { a->len = sizeof *a; a->family = kAfInet; }
#endif
struct ifa { struct ifa *next; char *name; unsigned flags; void *addr, *netmask, *dstaddr, *data; };  /* struct ifaddrs */

static void zero(void *p, unsigned n) {
  u8 *b = (u8 *)p;
  while (n--)
    *b++ = 0;
}
static void sayn(const char *a, unsigned v, const char *b) {  /* a + number (decimal) + b */
  char t[12];
  int i = 11;
  t[i] = 0;
  do {
    t[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v && i);
  say(a);
  say(t + i);
  say(b);
}
static void sayip(const char *a, const u8 *ip, const char *b) {
  sayn(a, ip[0], ".");
  sayn("", ip[1], ".");
  sayn("", ip[2], ".");
  sayn("", ip[3], b);
}
static void sayhex(const char *a, unsigned v, const char *b) {
  static const char h[] = "0123456789ABCDEF";
  char t[3] = { h[(v >> 4) & 15], h[v & 15], 0 };
  say(a);
  say(t);
  say(b);
}
static int load_socket(void) {
  static const char *const libs[] = { "libsocket.so.3", "libsocket.so.2", "libsocket.so" };
  void *h = 0;
#ifdef CPREBOOT_HOST
  h = dlopen(0, 2);
#else
  for (unsigned i = 0; i < 3 && !h; i++)
    h = dlopen(libs[i], 2 /* RTLD_NOW */);
#endif
  (void)libs;
  if (!h)
    return 0;
  so_socket = (int (*)(int, int, int))dlsym(h, "socket");
  so_connect = (int (*)(int, const void *, unsigned))dlsym(h, "connect");
  so_send = (long (*)(int, const void *, unsigned long, int))dlsym(h, "send");
  so_recv = (long (*)(int, void *, unsigned long, int))dlsym(h, "recv");
  so_setsockopt = (int (*)(int, int, int, const void *, unsigned))dlsym(h, "setsockopt");
  so_getsockopt = (int (*)(int, int, int, void *, unsigned *))dlsym(h, "getsockopt");
  so_getpeername = (int (*)(int, void *, unsigned *))dlsym(h, "getpeername");
  so_getifaddrs = (int (*)(void **))dlsym(h, "getifaddrs");
  so_freeifaddrs = (void (*)(void *))dlsym(h, "freeifaddrs");
  return so_socket && so_connect && so_send && so_recv && so_setsockopt && so_getsockopt && so_getpeername;
}
/* a TCP connection within ms (non-blocking connect, then getpeername, as kombi.h) with 1 s receive / send timeouts */
static int tcp_connect(const u8 *ip, unsigned port, unsigned ms) {
  int f = so_socket(kAfInet, kSockStream, 0);
  if (f < 0)
    return -1;
  int fl = fcntl(f, kFGetfl, 0);
  fcntl(f, kFSetfl, fl | kNonblock);
  struct sa4 a;
  zero(&a, sizeof a);
  sa4_init(&a);
  a.port[0] = (u8)(port >> 8);
  a.port[1] = (u8)port;
  for (int i = 0; i < 4; i++)
    a.addr[i] = ip[i];
  so_connect(f, &a, sizeof a);
  int up = 0;
  for (unsigned t = 0; t <= ms; t += 20) {
    struct sa4 p;
    unsigned pl = sizeof p;
    if (so_getpeername(f, &p, &pl) == 0) {
      up = 1;
      break;
    }
    int err = 0;
    unsigned el = sizeof err;
    if (so_getsockopt(f, kSolSocket, kSoError, &err, &el) == 0 && err)
      break;
    usleep(20000);
  }
  if (!up) {
    close(f);
    return -1;
  }
  fcntl(f, kFSetfl, fl & ~kNonblock);
  long tv[2] = { 1, 0 };  /* struct timeval: 1 s */
  so_setsockopt(f, kSolSocket, kSoRcvTimeo, tv, sizeof tv);
  so_setsockopt(f, kSolSocket, kSoSndTimeo, tv, sizeof tv);
  return f;
}
static int send_all(int f, const u8 *p, unsigned n) {
  while (n) {
    long w = so_send(f, p, n, 0);
    if (w <= 0)
      return 0;
    p += w;
    n -= (unsigned)w;
  }
  return 1;
}
/* n bytes within tries seconds (each recv waits at most 1 s); 0 = closed or timed out */
static int recv_exact(int f, u8 *p, unsigned n, unsigned *tries) {
  while (n) {
    long r = so_recv(f, p, n, 0);
    if (r == 0)
      return 0;
    if (r < 0) {
      if (!*tries)
        return 0;
      (*tries)--;
      continue;
    }
    p += r;
    n -= (unsigned)r;
  }
  return 1;
}
enum { kTester = 0xF4, kHeadUnit = 0x63, kPort = 6801 };
static int hsfz_send(int f, const u8 *uds, unsigned n) {
  u8 m[16];
  unsigned len = n + 2;
  m[0] = (u8)(len >> 24); m[1] = (u8)(len >> 16); m[2] = (u8)(len >> 8); m[3] = (u8)len;
  m[4] = 0; m[5] = 1;  /* control 0x0001: diagnostic message */
  m[6] = kTester; m[7] = kHeadUnit;
  for (unsigned i = 0; i < n; i++)
    m[8 + i] = uds[i];
  return send_all(f, m, 8 + n);
}
/* the head unit's answer to service sid: 1 positive (out[0..n) = its UDS bytes), -1 negative (nrc), 0 none within secs;
   gateway frames of other controls and other addresses skipped, 0x7F sid 0x78 (pending) waited out */
static int hsfz_answer(int f, u8 sid, u8 *out, unsigned *outn, unsigned secs, u8 *nrc) {
  unsigned tries = secs;
  for (;;) {
    u8 h[6];
    if (!recv_exact(f, h, 6, &tries))
      return 0;
    unsigned len = (unsigned)h[0] << 24 | (unsigned)h[1] << 16 | (unsigned)h[2] << 8 | h[3];
    unsigned ctl = (unsigned)h[4] << 8 | h[5];
    if (len > 8192)
      return 0;
    static u8 b[8192];
    if (len && !recv_exact(f, b, len, &tries))
      return 0;
    if (ctl != 1 || len < 3 || b[0] != kHeadUnit || b[1] != kTester)
      continue;  /* the gateway's echo (0x0002) and the like */
    const u8 *u = b + 2;
    unsigned un = len - 2;
    if (un >= 3 && u[0] == 0x7F && u[1] == sid && u[2] == 0x78)
      continue;  /* response pending */
    if (un >= 3 && u[0] == 0x7F && u[1] == sid) {
      *nrc = u[2];
      return -1;
    }
    if (u[0] == (u8)(sid + 0x40)) {
      unsigned k = un < 8 ? un : 8;
      for (unsigned i = 0; i < k; i++)
        out[i] = u[i];
      *outn = k;
      return 1;
    }
  }
}
static int parse_ip(const char *s, u8 *ip) {
  for (int i = 0; i < 4; i++) {
    unsigned v = 0, d = 0;
    while (*s >= '0' && *s <= '9' && d < 3) {
      v = v * 10 + (unsigned)(*s++ - '0');
      d++;
    }
    if (!d || v > 255 || (i < 3 && *s++ != '.'))
      return 0;
    ip[i] = (u8)v;
  }
  return *s == 0;
}
static unsigned env_num(const char *name, unsigned def) {
  const char *s = getenv(name);
  if (!s || !*s)
    return def;
  unsigned v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (unsigned)(*s++ - '0');
  return v;
}
/* 2 acknowledged, 1 sent without an acknowledgement, 0 not sent */
static int hsfz_reset(void) {
  if (!load_socket()) {
    say("CarPlayMenu: ECU reset: no socket library\n");
    return 0;
  }
  unsigned port = env_num("CPREBOOT_HSFZ_PORT", kPort);
  u8 cand[8][4];
  unsigned nc = 0;
  const char *host = getenv("CPREBOOT_HSFZ_HOST");
  if (host && *host) {
    if (parse_ip(host, cand[0]))
      nc = 1;
  } else {
    cand[0][0] = 127; cand[0][1] = 0; cand[0][2] = 0; cand[0][3] = 1;
    nc = 1;
    void *list = 0;
    if (so_getifaddrs && so_getifaddrs(&list) == 0) {
      for (struct ifa *i = (struct ifa *)list; i && nc < 8; i = i->next) {
        if (!i->addr || SA4_FAMILY(i->addr) != kAfInet)
          continue;
        const u8 *a = (const u8 *)i->addr + 4;
        if (a[0] == 127 || a[0] == 0)
          continue;
        int dup = 0;
        for (unsigned k = 0; k < nc; k++)
          if (cand[k][0] == a[0] && cand[k][1] == a[1] && cand[k][2] == a[2] && cand[k][3] == a[3])
            dup = 1;
        if (!dup) {
          for (int k = 0; k < 4; k++)
            cand[nc][k] = a[k];
          nc++;
        }
      }
      if (so_freeifaddrs)
        so_freeifaddrs(list);
    }
  }
  for (unsigned c = 0; c < nc; c++) {
    int f = tcp_connect(cand[c], port, 1500);
    if (f < 0) {
      sayip("CarPlayMenu: ECU reset: no HSFZ port at ", cand[c], "\n");
      continue;
    }
    sayip("CarPlayMenu: ECU reset: HSFZ port at ", cand[c], "\n");
    static const unsigned dids[2] = { 0xF187, 0xF190 };
    int known = 0;
    for (int d = 0; d < 2 && !known; d++) {
      u8 q[3] = { 0x22, (u8)(dids[d] >> 8), (u8)dids[d] }, a[8], nrc = 0;
      unsigned an = 0;
      if (!hsfz_send(f, q, 3))
        break;
      int r = hsfz_answer(f, 0x22, a, &an, 3, &nrc);
      if (r == 1 && an >= 3 && a[1] == q[1] && a[2] == q[2]) {
        known = 1;
        sayhex("CarPlayMenu: ECU reset: ECU 0x63 answered DID 0x", q[1], "");
        sayhex("", q[2], "\n");
      }
    }
    if (!known) {
      say("CarPlayMenu: ECU reset: ECU 0x63 did not identify itself there: no reset sent\n");
      close(f);
      continue;
    }
    say("CarPlayMenu: ECU reset: sending ONE UDS ECUReset 0x11 0x01 to ECU 0x63\n");
    static const u8 reset[2] = { 0x11, 0x01 };
    if (!hsfz_send(f, reset, 2)) {
      close(f);
      say("CarPlayMenu: ECU reset: the request could not be sent\n");
      return 0;
    }
    u8 a[8], nrc = 0;
    unsigned an = 0;
    int r = hsfz_answer(f, 0x11, a, &an, 5, &nrc);
    close(f);
    if (r == 1 && an >= 2 && a[1] == 0x01) {
      say("CarPlayMenu: ECU reset: acknowledged by the head unit (UDS 0x51 0x01)\n");
      return 2;
    }
    if (r == -1) {
      sayhex("CarPlayMenu: ECU reset: refused by ECU 0x63 (NRC 0x", nrc, ")\n");
      return 0;
    }
    say("CarPlayMenu: ECU reset: sent, no acknowledgement (the head unit may be restarting)\n");
    return 1;  /* sent once: never again (as the app) */
  }
  say("CarPlayMenu: ECU reset: no HSFZ port with ECU 0x63 on this unit\n");
  return 0;
}

__attribute__((constructor)) static void cp_reboot(void) {
  for (unsigned i = 0; i < sizeof kFlush / sizeof kFlush[0]; i++) {
    int fd = open(kFlush[i], 0);
    if (fd >= 0) {
      fdatasync(fd);
      close(fd);
    }
  }
  sync();
  const char *hz = getenv("CPREBOOT_HSFZ");
  if (hz && hz[0] == '1') {
    int r = hsfz_reset();
    if (r) {
      unsigned w = env_num("CPREBOOT_WAIT", 15);
      sayn("CarPlayMenu: waiting ", w, " s for the head unit to restart\n");
      sleep(w);
      say("CarPlayMenu: still running after the ECU reset\n");
    }
  }
  say("CarPlayMenu: rebooting the head unit (sysmgr_reboot)\n");
  sleep(1);
  sysmgr_reboot();
  say("CarPlayMenu: sysmgr_reboot returned: not rebooted\n");
  _exit(1);
}
