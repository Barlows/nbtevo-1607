// hud.h — "Add route guidance to HUD": CarPlay's next maneuver on the HUD / cluster, over MOST.
//
// A plugin thread (inside HmiMain3D) becomes a client of its own of the head unit's MOST service (DSI MOSTSubSystem.DSYSMOSTServer 4.5,
// the same one the HMI uses) and draws on the cluster's HUDTerminal (FBlock 0x82, instance 0), as the factory navigation does:
//   * where it comes from: the guidance the iPhone sends to CarPlay, decoded by librghook.so in mm-ipod into
//     /dev/shmem/carplaymenu-routeguidance (src/rghook.c; in RAM since 1.4.4, before on the disk as /var/iap/pps/routeguidance);
//   * protocol, DSI client and cluster layout: found by the NBT-AAD project (github.com/Sq00ky/Supra-iDrive6-Android-Auto, milestone-7
//     src/most, src/cluster/hudterm.hpp, hud6wa_profile.hpp), verified by them on an F20 ("6WA" cluster); the code here is ours.
//     Every call is a StartResultAck method (op 6) to the owner of 0x82/00 in the registry; the cluster answers ResultAck (op 0xD).
//     6WA instances: 1 frame, 2 street, 3 distance, 4 unit (texts), 5 and 6 the picture, double-buffered (SwitchVisibility).
//     The navigation creates the instances, so they differ per cluster: the BMW F-series multifunction cluster ("6WB") has 0x17 the
//     area, 0x1A/0x1B/0x1C the texts and 0x1F/0x20 a perspective plane of 221 x 158 (JayJay3071/nbt-android-auto, docs/
//     CLUSTER-HUDTERMINAL.md, verified there on an old NBT). Which one this car has is asked with ImageGetSize (detectLayout).
//   * set-up (initTerminal), the way the cluster firmware and the navigation's own set-up show it (MINIHU docs/HUD_TERMINAL_REFERENCES.md
//     "The whole picture", docs/CLUSTER_FIRMWARE.md): the cluster gives out the handles when the navigation creates its instances by
//     name (200 CreateInstance) and puts them into frames (22B FrameAddItem). Their answers pass this client too: hudhandles.h reads
//     the roles from them, per terminal, and the map is kept in the settings folder (hud_handles, hud_handles_2) for the next start
//     and for the sender in mm-ipod. Then the picture buffer's size is asked (C04 ImageGetSize) and the picture box fitted to it.
//   * terminals: 82/00 is drawn on (the HUD on the MINI F54; "FB_HUD_Terminal_hud" in its firmware); 82/02 ("FB_HUD_Terminal_kombi",
//     on clusters with a navigation area of their own, and a windscreen HUD's on some BMWs) gets a copy of every drawing call, with its
//     own handles, once the navigation was seen setting it up. The switch file hud_inst decides instead (see chooseBox).
//   * the BMW navigation has priority: while another client sends commands to 0x82, nothing is drawn (and the picture is redrawn later).
// Cars with an Ethernet KOMBI (G-series, J29) get the same guidance over SOME/IP from librghook-kombi.so in mm-ipod (src/rgkombi.cpp,
// kombi.h), which the installer chose; the installer's cluster file says whether this plugin draws on MOST too (kombiOnly below).
// Layout of libsys_dsi_colibry.gpp.so.8 (the variant the HMI loads) checked on 006_017_006: CDSIClient with the channel at +0x130, the
// socket at +0x138, the fd at +0x140 (0x14c bytes); the CDSIIStream given to the callback has the id at +0x48, the data at +0x10 and the
// size at +0x08; the data callback is vtable slot 7 (+0x1c), attached 3 and detached 4.
#pragma once

extern "C" {
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int pthread_create(int *, const void *, void *(*)(void *), void *);
int usleep(unsigned);
int sched_yield(void);
void *calloc(__SIZE_TYPE__, __SIZE_TYPE__);
struct hud_timespec {
  long sec;
  long nsec;
};
int clock_gettime(int, struct hud_timespec *);
}
#include "hudhandles.h"

// the switch files (the tests point this at a folder of their own)
#ifndef HUD_SWITCHES
#define HUD_SWITCHES "/var/opt/hmi/data/rw/carplaymenu/"
#endif
namespace hud {

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef short s16;

void (*g_log)(const char *) = 0;
// Switch debug_log (the library's cached check): the per-event lines (every HUDTerminal message, every maneuver and fill step) only
// with it; none = off. Without it the log keeps start-up, errors (the cluster's op f / op 9 answers) and the rare state changes.
bool (*g_debug)() = 0;
// Menu "HUD debug log" (switch file hud_debug, checked by the worker every 5 s): the HUD's own debug lines without debug_log
volatile int g_hudDebug;
bool verbose() {
  return g_hudDebug || (g_debug && g_debug());
}
#ifndef HUD_PPS
#define HUD_PPS "/dev/shmem/carplaymenu-routeguidance"
#endif
const char *const kPps = HUD_PPS;  // librghook's file (tests: -DHUD_PPS)

// ---- DSI client of the MOST service ------------------------------------------------------------------------------------------------
typedef void (*FnConnCtor)(void *);
typedef int (*FnConnAdd)(void *, void *);
typedef void (*FnConnStart)(void *);
typedef void (*FnCliCtor)(void *, const char *, const char *, int, int);
typedef void (*FnSetNotif)(void *, unsigned);
typedef void (*FnStreamCtor)(void *, void *, void *, int, unsigned, unsigned, unsigned, const void *, const void *, int);
typedef void (*FnStreamWrite)(void *, const void *, unsigned);
typedef int (*FnStreamSend)(void *, int);
typedef void (*FnStreamDtor)(void *);
struct {
  FnConnCtor connCtor;
  FnConnAdd connAdd;
  FnConnStart connStart;
  FnCliCtor cliCtor;
  FnSetNotif setNotif;
  FnStreamCtor streamCtor;
  FnStreamWrite streamWrite;
  FnStreamSend streamSend;
  FnStreamDtor streamDtor;
  void *vtable;
} S;

enum : u32 {
  kReqRegisterClient = 0xF,
  kReqProcessMessage = 0x2,
  kInfoClientResponse = 0x80000000u,
  kInfoMessage = 0x80000001u,
  kAttrCommState = 0xC0000000u,
  kAttrRegistry = 0xC0000001u
};
// what the HMI's MOST client subscribes to (without it the server sends no messages)
const u32 kSubs[] = { 0xC0000002u, 0xC0000003u, kAttrRegistry, kInfoMessage, 0x80000002u,
                      0x80000003u, 0x80000004u, 0x80000005u,   0x80000006u,  0x80000007u };
enum {
  kFBlock = 0x82,
  kOpStartResultAck = 6,
  kOpResultAck = 0xD
};
enum {
  kAudioMaster = 0x20,
  kVolumeActual = 0x200,
  kOpIncrement = 3,
  kOpDecrement = 4
};

void *g_conn;
char *g_cli;
volatile int g_attached, g_registered, g_lock;
volatile u32 g_client;
volatile u16 g_self, g_host, g_amHost;    // g_host: the terminal drawn on; g_amHost: owner of AudioMaster 20/00 ("Media gestures")
volatile u16 g_hostOf[2];                 // HUDTerminal 82/00 and 82/02 in the MOST registry (0: not there)
volatile u32 g_navMs, g_answers, g_sent;  // last command of another client to 0x82 (BMW navigation), answers from the cluster
// The terminals (slot 0 = 82/00, slot 1 = 82/02): g_inst is drawn on, the other one gets copies (g_mirror) once the navigation set
// it up or hud_inst asks for it. hud_inst fixes both (g_instFixed).
u8 g_inst;
volatile int g_mirror, g_instFixed, g_mirrorFixed;
inline u8 instOf(int slot) {
  return (u8)(slot ? 2 : 0);
}
inline int slotOf(u8 inst) {
  return inst == 2 ? 1 : inst == 0 ? 0 : -1;
}
void readTextEnc();
void textRefused(u32 now);  // a SetText refused (setText's coding fallback, 1.3.9)
bool g_streetDesc = true;  // 1.3.11: the street line falls back to the maneuver description (switch hud_street "road": off)
void readBox2();                            // hud_box_2 (1.3.9)
bool wordIs(const char *b, const char *w);  // a word at the start of a switch file
void mirrorAnswer(u16 h, u16 w, u16 ht);    // an ImageGetSize answer from the terminal the copies go to (1.3.9)
// the layout probe (detectLayout): ImageGetSize answered for handle 5 (the 6WA picture) / 0x1F (the 6WB plane); the BMW navigation
// seen drawing on a 6WB handle (0x18..0x35). g_plane1f (1.3.8): 0x1F answered the 6WB plane's own 221 x 158; g_w1f / g_h1f its size
volatile int g_size5, g_size1f, g_seen6wb, g_plane1f;
volatile u16 g_w1f, g_h1f;
// 1.3.2: the layout was guessed (6WA: no size question answered), asked again (layoutRetry), and the cluster's answers to the
// picture calls (AddPolygon, RemoveAll, SwitchVisibility: only ever on the picture buffers) on the terminal drawn on (refusedSwitch)
volatile int g_layoutGuess;
volatile u32 g_picTaken, g_picRefused;
// the picture buffer's size (measureBox): the handle asked, its answer (w, h) and the answers / refusals counted
volatile u16 g_askH, g_askW, g_askHt;
volatile int g_askOk, g_askErr;
// the handles learnt from the navigation's set-up, per terminal (hudhandles.h): g_learnt[slot] is a complete map not yet used
hudh::Learner g_learn[2];
hudh::Map g_learntMap[2];
volatile int g_learnt[2];
// day / night (effect colour): -1 not seen, 0 day, 1 night. From the HMI's CarPlay night mode on MOST (AuxiliaryInput 24/70
// CarPlaySetNightMode, CD7), as it passes; Vehicle 05/00 DayNight_ChangeOver (C31) is only logged (its values are not known).
volatile int g_night = -1;
volatile u32 g_dnLogged;

#ifdef HUD_FAKE_CLOCK
u32 g_fakeMs = 1000;  // tests: the clock is set by hand
u32 nowMs() {
  return g_fakeMs;
}
#else
u32 nowMs() {
  hud_timespec t;
  clock_gettime(2 /* CLOCK_MONOTONIC */, &t);
  return (u32)(t.sec * 1000 + t.nsec / 1000000);
}
#endif
// sleeps instead of spinning: with QNX's fixed priorities a sched_yield() loop may never let the lock's owner run
void lock() {
  while (__sync_lock_test_and_set(&g_lock, 1))
    usleep(1000);
}
void unlock() {
  __sync_lock_release(&g_lock);
}
void logf1(const char *fmt, u32 a, u32 b = 0, u32 c = 0) {
  char m[160];
  snprintf(m, sizeof m, fmt, a, b, c);
  if (g_log)
    g_log(m);
}

// request buffer: little-endian integers aligned from the start of the payload (DSI stream)
struct Buf {
  u8 b[512];
  u32 n;
  Buf() : n(0) {
  }
  void align(u32 a) {
    while (n % a && n < sizeof b)
      b[n++] = 0;
  }
  void u8_(u8 v) {
    if (n < sizeof b)
      b[n++] = v;
  }
  void u16_(u16 v) {
    align(2);
    u8_((u8)v);
    u8_((u8)(v >> 8));
  }
  void u32_(u32 v) {
    align(4);
    for (int i = 0; i < 4; i++)
      u8_((u8)(v >> (8 * i)));
  }
  void bytes(const u8 *p, u32 k) {
    for (u32 i = 0; i < k; i++)
      u8_(p[i]);
  }
};

bool request(u32 id, const Buf &p) {  // only with the client connected; serialised by g_lock
  if (!g_cli || !g_attached)
    return false;
  unsigned long long st[(0x400) / 8];
  memset(st, 0, sizeof st);
  S.streamCtor(st, g_cli + 0x130, g_cli + 0x138, *(int *)(g_cli + 0x140), 0x100, 7, id, g_cli + 0x110, g_cli + 0x118, 0);
  if (p.n)
    S.streamWrite(st, p.b, p.n);
  int rc = S.streamSend(st, -1);
  S.streamDtor(st);
  return rc >= 0;
}

void onAttached(void *) {
  g_attached = 1;
  g_registered = 0;
  Buf p;
  const char *nm = "CarPlayHud";
  u32 k = 0;
  while (nm[k])
    k++;
  p.u32_(k + 1);
  p.bytes((const u8 *)nm, k + 1);
  p.align(4);
  lock();
  request(kReqRegisterClient, p);
  for (u32 s : kSubs)
    S.setNotif(g_cli, s);
  unlock();
  if (g_log)
    g_log("CarPlayMenu: HUD: attached to MOSTSubSystem.DSYSMOSTServer, registering");
}
void onDetached(void *) {
  g_attached = 0;
  g_registered = 0;
  if (g_log)
    g_log("CarPlayMenu: HUD: detached from the MOST service");
}

u32 rd32(const u8 *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}
u16 rd16(const u8 *p) {
  return (u16)(p[0] | p[1] << 8);
}

// Our calls come back to every subscriber with the head unit's node address as the source (0x2000, not our client's): a call
// equal (function and payload) to one of the last ones we sent is ours, not the BMW navigation (like NBT-AAD's RecentCalls).
// 256 of them (1.3.2; 32 before): a 6WB picture sends every polygon twice (filled and outline), and copies to 82/02 double that,
// so one picture can be more than 32 calls, and a late echo of its first ones looked like another client drawing.
const u32 kRecent = 256;
struct Recent {
  u16 fkt;
  u32 n, h, t;
};
Recent g_recent[kRecent];
volatile u32 g_recentN;
u32 hashOf(const u8 *d, u32 n) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < n; i++)
    h = (h ^ d[i]) * 16777619u;
  return h;
}
void noteSent(u16 fkt, const u8 *d, u32 n) {
  Recent &r = g_recent[g_recentN++ % kRecent];
  r.fkt = fkt;
  r.n = n;
  r.h = hashOf(d, n);
  r.t = nowMs();
}
bool isOurs(u16 fkt, const u8 *d, u32 n) {
  u32 h = hashOf(d, n), now = nowMs();
  for (u32 i = 0; i < kRecent; i++) {
    const Recent &r = g_recent[i];
    if (r.t && r.fkt == fkt && r.n == n && r.h == h && now - r.t < 5000)
      return true;
  }
  return false;
}

// Debugging a cluster (F54 vs the 6WA profile): with debug_log, the first 300 HUDTerminal messages in both directions go to the log
// with their first data bytes (answers and errors to our calls, and the BMW navigation's own drawing commands when it guides).
// Without it only the cluster's errors to our calls (always) and, for 10 s after a test channel batch (g_probeMs), everything.
volatile u32 g_dbg, g_probeMs;
void dbgLine(const char *head, u16 fkt, u8 op, const u8 *d, u32 n, bool always = false) {
  if (g_dbg >= 600 || !g_log)
    return;
  if (!always && !(g_probeMs && nowMs() - g_probeMs < 10000) && !verbose())
    return;
  g_dbg++;
  char m[240];
  int k = snprintf(m, sizeof m, "CarPlayMenu: HUD %s fkt %03x op %x len %u:", head, fkt, op, n);
  for (u32 i = 0; i < n && i < 64 && k < (int)sizeof m - 4; i++)
    k += snprintf(m + k, sizeof m - k, " %02x", d[i]);
  g_log(m);
}
void dbgOut(u16 fkt, const u8 *d, u32 n) {
  dbgLine("out ", fkt, kOpStartResultAck, d, n);
}
void dbgMsg(const char *dir, u16 src, u16 dst, u16 fkt, u8 op, const u8 *p, u32 len) {
  u32 dn = len >= 32 ? rd32(p + 28) : 0;
  if (32 + dn > len)
    dn = len > 32 ? len - 32 : 0;
  char h[40];
  snprintf(h, sizeof h, "%s %04x>%04x", dir, src, dst);
  dbgLine(h, fkt, op, p + 32, dn, dst == g_self && (op == 0xF || op == 9));  // ErrorAck / Error to our calls: always
}
// The BMW navigation guiding (1.3.2). It builds its pictures in bursts (RemoveAll, polygons, the buffer swap), and between them
// sends the distance text and bar. nbt-android-auto, on a 6WB: "single stray results made v2 yield wrongly", so it counts 4 in
// 3 s. Here a burst of 4 picture calls within 3 s starts the wait; while it waits, any drawing call of another client keeps it
// 5 s more. Until 1.3.2 one call of any kind started it, so a text, a visibility or the position marker that another client sends
// now and then (or keeps sending while the car moves) could keep the CarPlay picture away for good. The first 8 such calls are
// logged, each with what it did.
volatile u32 g_burstMs, g_burstN, g_foreignLogged;
// 1.4.5 (ghackt issue #5, G30 NBT Evo ID6: "navigation guidance doesn't return after a speed camera alert", and with both HUD effects
// on the alerts did not show at all): a drawing call of another client that does not start the wait above (a speed camera warning,
// drawn with a few calls on the navigation's handles, which are ours too) hid the CarPlay picture without the worker knowing, so it
// came back only with the next maneuver; and the blink's buffer switch and the fill's redraws painted over the warning. Now such a
// call while the CarPlay picture is up "covers" it: nothing of ours is sent (no effect, no distance) until the other client has been
// quiet kCoverMs, then the whole picture is drawn again. A client that never stops (more than kCoverMaxMs at a stretch: the stray
// senders 1.3.2 met) is ignored for the rest of the session, as before 1.4.5.
const u32 kCoverMs = 4000, kCoverMaxMs = 15000;
volatile u32 g_coverMs;  // the last such call (0: none)
bool pictureCall(u16 fkt) {
  return fkt == 0x21D || fkt == 0x21E || fkt == 0x20E || fkt == 0x220 || fkt == 0xC05 || fkt == 0xC09;
}
void navSeen(u8 inst, u16 src, u16 dst, u16 fkt, u8 op, const u8 *d, u32 n) {
  u32 now = nowMs();
  bool waiting = g_navMs && now - g_navMs < 5000;
  if (pictureCall(fkt)) {
    if (!g_burstN || now - g_burstMs > 3000) {
      g_burstMs = now;
      g_burstN = 0;
    }
    g_burstN++;
  }
  bool starts = !waiting && pictureCall(fkt) && g_burstN >= 4;
  if (g_foreignLogged < 8 && g_log) {
    g_foreignLogged++;
    char m[200];
    snprintf(m, sizeof m, "CarPlayMenu: HUD: other client on 82/%02x: %04x -> %04x fkt %03x op %u [%02x %02x %02x %02x]%s", inst, src,
             dst, fkt, op, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0,
             waiting  ? ": the BMW navigation, still waiting"
             : starts ? ": 4 picture calls in 3 s, the BMW navigation draws"
                      : ": not counted alone");
    g_log(m);
  }
  if (waiting || starts)
    g_navMs = now ? now : 1;  // not now | 1: one millisecond ahead, now - g_navMs wrapped round
  else
    g_coverMs = now ? now : 1;  // not the BMW navigation's burst: maybe a warning over our picture (the worker decides)
}

void onData(void *, void *is) {
  if (!is)
    return;
  const char *s = (const char *)is;
  u32 id = *(const u32 *)(s + 0x48), len = *(const u32 *)(s + 0x08);
  const u8 *p = *(const u8 *const *)(s + 0x10);
  if (!p)
    return;
  if (id == kInfoClientResponse && len >= 6) {
    g_client = rd32(p);
    g_self = rd16(p + 4);
    if (g_client == 0xffffffffu) {
      if (g_log)
        g_log("CarPlayMenu: HUD: MOST registration refused");
      return;
    }
    g_registered = 1;
    lock();
    S.setNotif(g_cli, kAttrCommState);
    unlock();
    logf1("CarPlayMenu: HUD: MOST client %u, device %04x", g_client, g_self);
  } else if (id == kAttrRegistry && len >= 4) {
    u32 cnt = rd32(p);
    u16 host = 0, am = 0;
    static bool listed;  // once: every HUDTerminal instance in the registry (82/00 the cluster; others, e.g. 82/02, maybe a HUD)
    for (u32 i = 0; i < cnt && 4 + i * 4 + 4 <= len && !listed; i++) {
      u16 key = rd16(p + 4 + i * 4 + 2);
      if (key >> 8 == kFBlock)
        logf1("CarPlayMenu: HUD: registry: HUDTerminal 82/%02x at device %04x", key & 0xff, rd16(p + 4 + i * 4));
    }
    listed = listed || cnt > 0;
    u16 t0 = 0, t2 = 0;
    for (u32 i = 0; i < cnt && 4 + i * 4 + 4 <= len; i++) {
      u16 key = rd16(p + 4 + i * 4 + 2);
      if (key == ((kFBlock << 8) | 0) && !t0)
        t0 = rd16(p + 4 + i * 4);
      if (key == ((kFBlock << 8) | 2) && !t2)
        t2 = rd16(p + 4 + i * 4);
      if (key == ((kAudioMaster << 8) | 0) && !am)
        am = rd16(p + 4 + i * 4);
    }
    g_hostOf[0] = t0;
    g_hostOf[1] = t2;
    if (!g_instFixed && cnt)  // no 82/00 but an 82/02: that one is drawn on
      g_inst = (u8)(!t0 && t2 ? 2 : 0);
    host = g_hostOf[slotOf(g_inst) < 0 ? 0 : slotOf(g_inst)];
    if (host != g_host) {
      g_host = host;
      logf1("CarPlayMenu: HUD: drawing on HUDTerminal 82/%02x at device %04x", g_inst, host);
    }
    if (am != g_amHost) {
      g_amHost = am;
      logf1("CarPlayMenu: MOST: AudioMaster 20/00 at device %04x", am);
    }
  } else if (id == kInfoMessage && len >= 32) {  // MessageType: tx, tag, val, dst, src, fblock, inst, fkt, op, tag, len, data
    u16 dst = rd16(p + 12), src = rd16(p + 14);
    u8 fb = p[16], inst = p[17], op = p[20];
    u16 fkt = rd16(p + 18);
    u32 an = len >= 32 ? rd32(p + 28) : 0;
    if (32 + an > len)
      an = 0;
    // day / night, for the effect colour (see g_night): the HMI tells Connectivity CarPlay's night mode (a method: sender handle, then
    // the value, last byte non-zero = night; read from the HMI's MOST map, not seen in a car yet). The first sightings are logged.
    if ((fb == 0x24 && inst == 0x70 && fkt == 0xCD7) || (fb == 0x05 && fkt == 0xC31)) {
      if (g_dnLogged < 12) {
        g_dnLogged++;
        dbgLine(fb == 5 ? "day/night Vehicle C31" : "day/night CarPlaySetNightMode", fkt, op, p + 32, an, true);
      }
      if (fb == 0x24 && an >= 1 && (op == 0 || op == 2 || op == 6 || op == 0xC)) {
        int n = p[32 + an - 1] != 0;
        if (n != g_night) {
          g_night = n;
          if (g_log)
            g_log(n ? "CarPlayMenu: HUD: night (CarPlay night mode from the HMI)"
                    : "CarPlayMenu: HUD: day (CarPlay night mode from the HMI)");
        }
      }
      return;
    }
    if (fb != kFBlock)
      return;
    if (op == kOpResultAck && dst == g_self)
      g_answers++;
    u32 varTag = rd32(p + 4), varVal = rd32(p + 8);
    // ImageGetSize answers: [handle][w][h] (F54, 6 bytes) or [handle][01][w][h] (6WB, 7 bytes), sizes u16 big-endian. The F54 also
    // answers for handles it does not have, with 0 x 0: only a real size counts. Only the terminal drawn on.
    if (fkt == 0xC04 && dst == g_self && src != g_self && inst == g_inst) {
      if (op == kOpResultAck && (an == 6 || an == 7)) {
        const u8 *a = p + 32 + (an == 7 ? 3 : 2);
        u16 h = (u16)(p[32] << 8 | p[33]), w = (u16)(a[0] << 8 | a[1]), ht = (u16)(a[2] << 8 | a[3]);
        if (w && ht && h == 5)
          g_size5 = 1;
        if (w && ht && h == 0x1f) {
          g_size1f = 1;
          g_w1f = w;
          g_h1f = ht;
          if (w == 221 && ht == 158)
            g_plane1f = 1;
        }
        if (h == g_askH) {
          g_askW = w;
          g_askHt = ht;
          g_askOk = 1;
        }
      } else if (op == 0xF || op == 9)
        g_askErr++;
    }
    // 1.3.9: 82/02's answer for its picture buffer (askMirrorSize), for the copies' box
    if (fkt == 0xC04 && dst == g_self && src != g_self && g_mirror && inst != g_inst && op == kOpResultAck && (an == 6 || an == 7)) {
      const u8 *a = p + 32 + (an == 7 ? 3 : 2);
      mirrorAnswer((u16)(p[32] << 8 | p[33]), (u16)(a[0] << 8 | a[1]), (u16)(a[2] << 8 | a[3]));
    }
    if (dst == g_self && src != g_self && inst == g_inst && fkt == 0x210 && (op == 0xF || op == 9))
      textRefused(nowMs());
    if (dst == g_self && src != g_self && inst == g_inst && (fkt == 0x21D || fkt == 0x220 || fkt == 0x20E)) {
      if (op == kOpResultAck)
        g_picTaken++;
      else if (op == 0xF || op == 9)
        g_picRefused++;
    }
    dbgMsg(src == g_self ? "echo" : dst == g_self ? "in  " : "seen", src, dst, fkt, op, p, len);
    bool normal = varTag == 2 && !(varVal >= 4 && varVal <= 9);  // not the send status of a message of ours
    bool drawing = (fkt >= 0x200 && fkt <= 0x22B) || (fkt >= 0xC03 && fkt <= 0xC09);
    u32 dn = rd32(p + 28);
    const u8 *dd = p + 32;
    if (32 + dn > len)
      dn = 0;
    // the navigation's set-up answers (the cluster's ResultAck to another client: we create nothing), per terminal
    int slot = slotOf(inst);
    if (op == kOpResultAck && src != g_self && dst != g_self && normal && slot >= 0 && (fkt == 0x200 || fkt == 0x22B)) {
      hudh::Learner &L = g_learn[slot];
      if (fkt == 0x200)
        L.created(dd, dn);
      else {
        u16 bar = L.m.bar;
        bool now = L.added(dd, dn);
        if (now || (L.done && L.m.bar != bar)) {  // complete, or the bargraph that follows the texts
          g_learntMap[slot] = L.m;
          g_learnt[slot] = 1 + (L.family() == 11);
        }
      }
    }
    if (op <= 8 && src != g_self && normal && drawing && !isOurs(fkt, dd, dn)) {  // another client (the BMW navigation) drawing
      if (inst == g_inst && dn >= 2 && (dd[0] << 8 | dd[1]) >= 0x18 && (dd[0] << 8 | dd[1]) <= 0x35)  // 6WB: the F54's end at 0x17
        g_seen6wb = 1;
      navSeen(inst, src, dst, fkt, op, dd, dn);
    }
  }
}

void *connThread(void *) {
  S.connStart(g_conn);
  if (g_log)
    g_log("CarPlayMenu: HUD: DSI connection ended");
  return 0;
}

bool dsiOpen() {
  void *lib = dlopen("libsys_dsi_colibry.gpp.so.8", 2);
  if (!lib)
    lib = dlopen("/opt/sys/lib/libsys_dsi_colibry.gpp.so.8", 2);
  if (!lib) {
    if (g_log)
      g_log("CarPlayMenu: HUD: libsys_dsi_colibry.gpp.so.8 not found");
    return false;
  }
  S.connCtor = (FnConnCtor)dlsym(lib, "_ZN14CDSIConnectionC1Ev");
  S.connAdd = (FnConnAdd)dlsym(lib, "_ZN14CDSIConnection3addER10CDSIClient");
  S.connStart = (FnConnStart)dlsym(lib, "_ZN14CDSIConnection5startEv");
  S.cliCtor = (FnCliCtor)dlsym(lib, "_ZN10CDSIClientC1EPKcS1_ii");
  S.setNotif = (FnSetNotif)dlsym(lib, "_ZN10CDSIClient15setNotificationEj");
  S.streamCtor = (FnStreamCtor)dlsym(
      lib, "_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
  S.streamWrite = (FnStreamWrite)dlsym(lib, "_ZN11CDSIOStream5writeEPKvj");
  S.streamSend = (FnStreamSend)dlsym(lib, "_ZN11CDSIOStream4sendEi");
  S.streamDtor = (FnStreamDtor)dlsym(lib, "_ZN11CDSIOStreamD1Ev");
  S.vtable = dlsym(lib, "_ZTV10CDSIClient");
  if (!S.connCtor || !S.connAdd || !S.connStart || !S.cliCtor || !S.setNotif || !S.streamCtor || !S.streamWrite || !S.streamSend ||
      !S.streamDtor || !S.vtable) {
    if (g_log)
      g_log("CarPlayMenu: HUD: DSI symbols missing");
    return false;
  }
  g_conn = calloc(1, 0x400);
  g_cli = (char *)calloc(1, 0x400);
  if (!g_conn || !g_cli)
    return false;
  S.connCtor(g_conn);
  S.cliCtor(g_cli, "DSYSMOSTServer", "MOSTSubSystem", 4, 5);
  void **vt = (void **)calloc(16, sizeof(void *));  // CDSIClient vtable: 8 slots (0x28 bytes with the header)
  memcpy(vt, (char *)S.vtable + 8, 8 * sizeof(void *));
  vt[3] = (void *)&onAttached;
  vt[4] = (void *)&onDetached;
  vt[7] = (void *)&onData;
  *(void ***)g_cli = vt;
  S.connAdd(g_conn, g_cli);
  int t;
  return pthread_create(&t, 0, connThread, 0) == 0;
}

}  // namespace hud
#include "hudshapes.h"
namespace hud {

// ---- HUDTerminal: calls -------------------------------------------------------------------------------------------------------------
enum {
  kSetVisibility = 0x204,
  kSwitchVisibility = 0x20E,
  kSetText = 0x210,
  kSetLineWidth = 0x219,
  kSetFillColor = 0x21B,
  kAddPolygon = 0x21D,
  kRemoveAll = 0x220
};
// Where the picture is. The shapes below are drawn in the 6WA box (70,20)-(170,120) (F20, NBT-AAD), which needs a picture buffer of at
// least 170 x 120. The MINI F54's buffers are 120 x 72 (ImageGetSize, car 2026-11-13; 128 x 74 surfaces in its firmware): its BMW
// navigation draws turn arrows in about x 37..83, y 1..68 (captured in the car, HMI ID5_1703-490, 2026-11-11), and a test arrow there
// was visible while ours in the 6WA box was not (text was). So:
//   * kBox6wa: drawn as they are;
//   * kBoxMini: every point mapped to (35,15)-(85,65) by halving (toMini, the car-verified MINI drawing);
//   * kBoxFit: mapped linearly onto g_box (toBox).
// The start: MINI when its layout package is present, else 6WA. Then measureBox asks the cluster for the picture buffer's size and
// fits the box to the buffer (fitBox): 120 x 72 gives the MINI box, 170 x 120 or more the 6WA box. (The F25 X3 answers 0 x 0 for
// buffer 5, issue #9: its pictures are other instances, which the set-up gives; ghackt's hud_box mini was verified there.) Switch files decide instead:
// hud_box_6wa (the 6WA box), hud_box "mini" / "6wa" / "<x> <y> <size>" (ghackt's form, a square) / "<x0> <y0> <x1> <y1>" (PR #13,
// FreZZZeR: (35,15)-(85,65) verified on an F30 6WB with a 126 x 77 buffer).
enum {
  kBox6wa = 0,
  kBoxMini = 1,
  kBoxFit = 2
};
int g_boxMode;
bool g_boxFixed;  // a switch chose the box: measureBox leaves it alone
Box g_box;
// hud_hide (PR #13, FreZZZeR): handles (decimal) made invisible on every picture, for a surface the cluster shows itself over other
// content (an F30 6WB puts its own distance bar on 0x15/0x16 over the speed: "21 22", car-verified). On the F54 those are the bars.
u16 g_hide[8];
u32 g_nHide;
// hud_comma (ghackt, issue #12): the iPhone writes distances in the phone's locale, "1,5 km" in NL / DE / FR; the clusters' filters
// made the comma a space ("1 5 km", car-reported on an F31). The distance's comma goes as a point ("1.5"), as the plugin's own
// metres fallback writes it; with hud_comma it goes as it is. Commas in the street stay spaces.
bool g_comma;
// hud_colour_day: the effect colour while the HMI's CarPlay night mode says day (g_night 0). The F54's palette differs by day: its
// colour 5, orange at night, is green by day (car-reported 2026-11-18); which index is orange by day is not measured yet
// (hud_probe.sh colours in daylight). Without the file the effect colour is the same day and night.
int g_fxDay = -1;
u8 g_fillDim = 3;  // the context tone: grey (F54; 0x0A, the first choice, is transparent there); hud_mono draws everything in kFill
// Test switches (files in the settings folder, read when the HUD starts; all off by default, the drawing without them is unchanged):
//   hud_convex   the convex pieces of before instead of whole polygons. Whole polygons (hudshapes.h mergePieces, a few calls a picture
//                instead of up to 96, which is what makes the two effects below cheap) are always on since 2026-11-13: the F54 terminal
//                fills them cleanly (hud_probe.sh concave) and the pictures look the same (test switch hud_concave until then).
//   hud_fill     progress fill: from the step's start the maneuver fills with the effect colour from the bottom up, in 8 steps, full
//                at the turn. Range (mib2q-carplay-rgi's bargraph rule): the iPhone's step length (maneuver_step_length), at most
//                the cap; unknown -> the cap. Optional content: "<cap metres> <colour index>", default "300 5".
//                F54 fill colours at night (car, 2026-11-13, hud_probe.sh colours): 0 4 6 7 white, 2 5 11..31 orange, 3 8 grey,
//                1 9 10 nothing (10, the first default, only cut the white away). The BMW navigation uses 4 and 5; day colours unchecked.
//   hud_blink    in the last fifth of that range the maneuver blinks, 600 ms per phase, route colour <-> effect colour: both phases
//                sit in the two picture buffers and SwitchVisibility swaps them, one MOST call a phase.
//   hud_colour   the effect colour index (menu "HUD effects > Effect colour"); else the second number of hud_fill; else 5.
// hud_fill and hud_blink are menu options since 2026-11-13 ("HUD effects").
// Colour indexes are the cluster's palette (F54 at night: 4 white = the route, 3 grey = the context tone, 5 orange; 10 = 0x0A transparent).
bool g_concave, g_fillOn, g_blinkOn;
u32 g_fxCap = 300;
u8 g_fxColour = 5;  // orange (F54), as the BMW navigation uses
// a switch file: present -> true; up to two numbers in it go to a and b
bool readSwitch(const char *name, u32 *a, u32 *b) {
  char path[96];
  snprintf(path, sizeof path, HUD_SWITCHES "%s", name);
  int fd = open(path, 0);
  if (fd < 0)
    return false;
  char t[32];
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  if (n < 0)
    n = 0;
  t[n] = 0;
  u32 *out[2] = { a, b };
  u32 k = 0;
  for (char *q = t; *q && k < 2;) {
    if (*q >= '0' && *q <= '9') {
      u32 v = 0;
      while (*q >= '0' && *q <= '9')
        v = v * 10 + (u32)(*q++ - '0');
      if (out[k])
        *out[k] = v;
      k++;
    } else
      q++;
  }
  return true;
}
// value of one hex digit, -1 if c is not one
int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
// a switch file's first word, lower case ("mini", "6wa"); empty without the file
void readWord(const char *name, char *w, u32 cap) {
  char path[96];
  snprintf(path, sizeof path, HUD_SWITCHES "%s", name);
  w[0] = 0;
  int fd = open(path, 0);
  if (fd < 0)
    return;
  char t[16];
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  u32 k = 0;
  for (long i = 0; i < n && k + 1 < cap; i++) {
    char c = t[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    if (c == '-')
      c = '_';  // "6wb-hud" = "6wb_hud" (hud_car, 1.4.0)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      break;
    w[k++] = c;
  }
  w[k] = 0;
}
// a switch file's numbers (decimal or 0x hex), up to max: how many (-1: no file)
int readNums(const char *name, u32 *v, u32 max) {
  char path[96];
  snprintf(path, sizeof path, HUD_SWITCHES "%s", name);
  int fd = open(path, 0);
  if (fd < 0)
    return -1;
  char t[64];
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  if (n < 0)
    n = 0;
  t[n] = 0;
  u32 k = 0;
  for (char *q = t; *q && k < max;) {
    if (*q >= '0' && *q <= '9') {
      u32 x = 0;
      if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))
        for (q += 2; hexv(*q) >= 0; q++)
          x = x << 4 | (u32)hexv(*q);
      else
        while (*q >= '0' && *q <= '9')
          x = x * 10 + (u32)(*q++ - '0');
      v[k++] = x;
    } else
      q++;
  }
  return (int)k;
}
void logBox(const char *why) {
  char m[160];
  if (g_boxMode == kBoxFit)
    snprintf(m, sizeof m, "CarPlayMenu: HUD: picture box (%d,%d)-(%d,%d) (%s)", g_box.x0, g_box.y0, g_box.x1, g_box.y1, why);
  else
    snprintf(m, sizeof m, "CarPlayMenu: HUD: picture box %s (%s)",
             g_boxMode == kBoxMini ? "MINI (35,15)-(85,65)" : "6WA (70,20)-(170,120)", why);
  if (g_log)
    g_log(m);
}
// hud_inst (ghackt, issue #1): "<drawn on> [<copy to>]", e.g. "0 2" (82/00, copies to 82/02), "2" (82/02 only), "0" (no copies)
// 1.4.0: "Car and HUD type" (main menu since 1.4.2; cpm.cpp writes the switch file hud_car): the car's cluster / HUD chosen in the menu
// instead of the guesses. It only stands in for what detection decides; hud_layout, hud_box / hud_box_6wa and hud_inst still decide
// over it. Words: "auto" (or no file), "6wa", "6wb", "6wb_hud", "mini".
//   6wa      BMW 6WA cluster, or an analog cluster with a windscreen HUD (issue #27): the BMW box (not the MINI one, even when the
//            unit carries the MINI layout package), the 6WA layout, the terminal and copies as detected
//   6wb      BMW 6WB cluster without a windscreen HUD: the 6WB layout, no copies to 82/02
//   6wb_hud  BMW 6WB cluster with a windscreen HUD (issues #2 / #25 / #26): the 6WB layout, drawn on 82/00, copies to 82/02
//   mini     MINI: the MINI box, the 6WA layout (a MINI cluster is not a 6WB), the terminal and copies as detected
enum { kCarAuto = 0, kCar6wa, kCar6wb, kCar6wbHud, kCarMini };
int g_car = kCarAuto;
const char *carName() {
  return g_car == kCar6wa ? "BMW 6WA cluster" : g_car == kCar6wb ? "BMW 6WB cluster" : g_car == kCar6wbHud ? "BMW 6WB cluster + HUD"
       : g_car == kCarMini ? "MINI" : "automatic";
}
void readCar() {
  char w[12];
  readWord("hud_car", w, sizeof w);
  g_car = kCarAuto;
  if (wordIs(w, "6wb_hud") || wordIs(w, "6wbhud"))
    g_car = kCar6wbHud;
  else if (wordIs(w, "6wb"))
    g_car = kCar6wb;
  else if (wordIs(w, "6wa"))
    g_car = kCar6wa;
  else if (wordIs(w, "mini"))
    g_car = kCarMini;
  if (g_car != kCarAuto && g_log) {
    char m[120];
    snprintf(m, sizeof m, "CarPlayMenu: HUD: car type %s (menu: Car and HUD type, switch hud_car)", carName());
    g_log(m);
  }
}
void readInst() {
  u32 in[2];
  int ni = readNums("hud_inst", in, 2);
  if (ni < 0 && g_car == kCar6wbHud) {  // the menu's car type, when no hud_inst decides
    g_inst = 0;
    g_instFixed = 1;
    g_mirrorFixed = 1;
    g_mirror = 1;
    if (g_log)
      g_log("CarPlayMenu: HUD: drawing on HUDTerminal 82/00, copies to 82/02 (car type BMW 6WB cluster + HUD)");
    return;
  }
  if (ni < 0 && g_car == kCar6wb) {
    g_mirrorFixed = 1;
    g_mirror = 0;
    if (g_log)
      g_log("CarPlayMenu: HUD: no copies to the other terminal (car type BMW 6WB cluster, no HUD)");
    return;
  }
  if (ni >= 1 && slotOf((u8)in[0]) >= 0 && in[0] < 256) {
    g_inst = (u8)in[0];
    g_instFixed = 1;
    g_mirrorFixed = 1;
    g_mirror = ni >= 2 && in[1] != in[0] && slotOf((u8)in[1]) >= 0;
    logf1("CarPlayMenu: HUD: drawing on HUDTerminal 82/%02x, copies to the other terminal %u (switch hud_inst)", g_inst, g_mirror);
  } else if (ni >= 0 && g_log)
    g_log("CarPlayMenu: HUD: hud_inst: only 0 and 2 are known terminals, ignored");
}
void chooseBox() {
  readCar();
  readTextEnc();
  readBox2();
  {
    char sw[8];
    readWord("hud_street", sw, sizeof sw);
    if (sw[0] == 'r') {
      g_streetDesc = false;
      if (g_log)
        g_log("CarPlayMenu: HUD: street line: road names only (switch hud_street road)");
    }
  }
  int mono = open("/var/opt/hmi/data/rw/carplaymenu/hud_mono", 0);
  if (mono >= 0) {
    close(mono);
    g_fillDim = 4;
  }
  u32 cap = g_fxCap, col = g_fxColour, day = 0;
  g_concave = !readSwitch("hud_convex", 0, 0);
  g_fillOn = readSwitch("hud_fill", &cap, &col);
  g_blinkOn = readSwitch("hud_blink", 0, 0);  // the menu writes "1": no colour in it
  readSwitch("hud_colour", &col, 0);
  if (cap >= 50 && cap <= 5000)
    g_fxCap = cap;
  if (col < 20)
    g_fxColour = (u8)col;
  if (readNums("hud_colour_day", &day, 1) == 1 && day < 20)
    g_fxDay = (int)day;
  logf1("CarPlayMenu: HUD: test switches concave %u fill %u blink %u", g_concave, g_fillOn, g_blinkOn);
  if (g_fillOn || g_blinkOn)
    logf1("CarPlayMenu: HUD: effect cap %u m, colour %u, by day %d", g_fxCap, g_fxColour, (u32)g_fxDay);
  // MINI or not: the coded brand first, as the menu decides its layout (brandMini in cpm.cpp; 02 = MINI, 00 BMW .. 05). Many BMW
  // units carry the MINI package too (a BMW F20 with HMI 1607 got the MINI box from it, cluster 6WA, 1.2.12); only without the
  // variable does the MINI layout's presence decide. The switches below, and the measured buffer (measureBox), still come after.
  const char *why;
  const char *br = getenv("HMI_BRAND");
  if (br && br[0] == '0' && br[1] >= '0' && br[1] <= '5' && !br[2]) {
    g_boxMode = br[1] == '2' ? kBoxMini : kBox6wa;
    why = br[1] == '2' ? "HMI_BRAND=02, a MINI" : "HMI_BRAND not MINI, whatever layouts the unit carries";
  } else {
    int f = open("/opt/hmi/data/ro/mini/id5/widget/generated/Spec_State_Merged.pbw", 0);
    if (f < 0)
      f = open("/opt/hmi/ID5/data/ro/mini/id5/widget/generated/Spec_State_Merged.pbw", 0);
    g_boxMode = f >= 0 ? kBoxMini : kBox6wa;
    if (f >= 0)
      close(f);
    why = g_boxMode == kBoxMini ? "a MINI" : "the default";
  }
  if (g_car == kCarMini) {  // the menu's car type instead of the brand guess (the box switches below still decide over it)
    g_boxMode = kBoxMini;
    why = "car type MINI";
  } else if (g_car != kCarAuto) {
    g_boxMode = kBox6wa;
    why = "car type BMW";
  }
  int o = open("/var/opt/hmi/data/rw/carplaymenu/hud_box_6wa", 0);
  if (o >= 0) {
    close(o);
    g_boxMode = kBox6wa;
    g_boxFixed = true;
    why = "switch hud_box_6wa";
  }
  char w[8];
  readWord("hud_box", w, sizeof w);
  u32 bx[4];
  int nb = w[0] >= '0' && w[0] <= '9' ? readNums("hud_box", bx, 4) : 0;
  if (w[0] == 'm' || w[0] == '6') {
    g_boxMode = w[0] == 'm' ? kBoxMini : kBox6wa;
    g_boxFixed = true;
    why = "switch hud_box";
  } else if (nb == 3 && bx[2] >= 10 && bx[2] <= 400 && bx[0] < 1000 && bx[1] < 1000) {
    Box b = { (int)bx[0], (int)bx[1], (int)(bx[0] + bx[2]), (int)(bx[1] + bx[2]) };
    g_box = b;
    g_boxMode = kBoxFit;
    g_boxFixed = true;
    why = "switch hud_box";
  } else if (nb == 4 && bx[2] > bx[0] && bx[3] > bx[1] && bx[2] < 1000 && bx[3] < 1000) {
    Box b = { (int)bx[0], (int)bx[1], (int)bx[2], (int)bx[3] };
    g_box = b;
    g_boxMode = kBoxFit;
    g_boxFixed = true;
    why = "switch hud_box";
  }
  logBox(why);
  u32 hv[8];
  int nh = readNums("hud_hide", hv, 8);
  g_nHide = 0;
  for (int i = 0; i < nh; i++)
    if (hv[i] && hv[i] < 0x100)
      g_hide[g_nHide++] = (u16)hv[i];
  if (g_nHide)
    logf1("CarPlayMenu: HUD: hud_hide: %u handle(s) made invisible on every picture", g_nHide);
  g_comma = readSwitch("hud_comma", 0, 0);
  if (g_comma && g_log)
    g_log("CarPlayMenu: HUD: the distance's decimal comma sent as it is (switch hud_comma)");
  readInst();
}
// The handles of the terminal drawn on: the 6WA profile (F20, NBT-AAD; also the MINI F54) until the navigation's set-up (hudhandles.h)
// or a map kept from an earlier start gives them, or detectLayout finds the 6WB layout. hBar: the distance bar (known from a set-up only).
u16 hFrame = 1, hStreet = 2, hDist = 3, hUnit = 4, hBufA = 5, hBufB = 6, hBar;
bool g_mapKnown;  // the handles came from a set-up (this start's or a kept one): detectLayout's 6WB handles do not replace them
// The other terminal's handles for its copies (g_mirror): from its own set-up; until then the same numbers.
hudh::Map g_mmap;
bool g_mmapKnown;
// 1.3.9 (issue #26: on a 6WB the windscreen HUD on 82/02 showed the top of the arrow and cut the rest off with a straight line, while
// the cluster on 82/00 showed it whole): the copies' picture is put into its own box of the plane. The 6WB picture lies in
// (66,18)-(166,143) (hudshapes.h wideOne); the copies' AddPolygon points are mapped linearly from there into g_box2:
//   * switch hud_box_2 "<x0> <y0> <x1> <y1>" (plane coordinates), or "full" for the cluster's box unchanged;
//   * else, when 82/02 answers ImageGetSize for its picture buffer with less than the plane (221 x 158), the box fitted into it;
//   * else (82/02 answering the full plane, or nothing) the upper part where that HUD showed the picture: (88,12)-(144,82), the
//     arrow at 56 % of the cluster's size with the same 4:5 proportions, above the line where the photo cut it (about y 90).
// Only on a 6WB, only the copies, only the picture buffers; the cluster's own picture is not touched.
const int kSrcX0 = 66, kSrcY0 = 18;  // the 6WB picture's box (66,18)-(166,143): 100 x 125
struct Box2 {
  int x0, y0, x1, y1;
};
Box2 g_box2 = { 88, 12, 144, 82 };
int g_box2Mode;              // 0 the default, 1 hud_box_2, 2 hud_box_2 "full" (no change), 3 fitted to 82/02's answer
volatile u16 g_mw, g_mh;     // 82/02's picture buffer size (its ImageGetSize answer)
volatile int g_box2Due;      // the box is worked out again (a new answer) before the next copy
bool g_mirrorAsked;          // the size question went to 82/02 this session
const u8 kFill = 4;  // the BMW navigation's colour on 6WA, F54 and 6WB alike (JayJay's 6WB recording: 21B 04 / 0A)
// 6WB: texts in ISO-8859-15 (encoding 01, as verified there; the big distance font has no comma) and the picture moved from the 6WA
// box (70,20)-(170,120) into the plane, centred on x 116 like JayJay's arrows: (66,40)-(166,140) of 221 x 158
bool g_6wb;

// a MOST message through DSYSMOSTServer: destination, FBlock, instance, function, OpType, data; tx: the transmission type (0; the
// BMW navigation's HUDTerminal calls use 4, as nbt-android-auto saw on a 6WB, where 0 works too)
bool send(u16 dst, u8 fb, u8 inst, u16 fkt, u8 op, const u8 *d, u32 n, u32 tx = 0) {
  if (!g_registered || !dst)
    return false;
  Buf p;
  p.u32_(g_client);
  p.u32_(tx);
  p.u32_(1);
  p.u32_(0);  // transmission, TXControl variant, value
  p.u16_(dst);
  p.u16_(g_self);
  p.u8_(fb);
  p.u8_(inst);
  p.u16_(fkt);
  p.u8_(op);
  p.u32_(++g_sent);
  p.u32_(n);
  p.bytes(d, n);
  lock();
  bool ok = request(kReqProcessMessage, p);
  unlock();
  return ok;
}
// one of our handles in the other terminal's numbering (its set-up's map; unknown handles stay as they are)
u16 mirrorHandle(u16 h) {
  if (!g_mmapKnown)
    return h;
  const hudh::Map &m = g_mmap;
  return h == hFrame          ? m.frame
         : h == hStreet       ? m.street
         : h == hDist         ? m.dist
         : h == hUnit         ? m.unit
         : h == hBufA         ? m.bufA
         : h == hBufB         ? m.bufB
         : h == hBar && m.bar ? m.bar
                              : h;
}
const u16 kAddPolygonFkt = 0x21D;
// the copies' box from 82/02's picture buffer size: the 4:5 box as large as fits with 1/8 left free, centred. Only constant divisions.
void fitBox2(u32 w, u32 h) {
  int W = (int)(w - (w >> 3)), H = (int)(h - (h >> 3)), dw, dh;
  if (H * 4 <= W * 5) {
    dh = H;
    dw = H * 4 / 5;
  } else {
    dw = W;
    dh = W * 5 / 4;
  }
  g_box2.x0 = ((int)w - dw) >> 1;
  g_box2.y0 = ((int)h - dh) >> 1;
  g_box2.x1 = g_box2.x0 + dw;
  g_box2.y1 = g_box2.y0 + dh;
}
void logBox2(const char *why) {
  char m[160];
  if (g_box2Mode == 2)
    snprintf(m, sizeof m, "CarPlayMenu: HUD: copies' picture as on the cluster (%s)", why);
  else
    snprintf(m, sizeof m, "CarPlayMenu: HUD: copies' picture box (%d,%d)-(%d,%d) of the plane (%s)", g_box2.x0, g_box2.y0, g_box2.x1,
             g_box2.y1, why);
  if (g_log)
    g_log(m);
}
// hud_box_2 (read with the other switches)
void readBox2() {
  char w[8];
  readWord("hud_box_2", w, sizeof w);
  u32 b[4];
  int nb = w[0] >= '0' && w[0] <= '9' ? readNums("hud_box_2", b, 4) : 0;
  if (w[0] == 'f') {
    g_box2Mode = 2;
    logBox2("switch hud_box_2");
  } else if (nb == 4 && b[2] > b[0] + 8 && b[3] > b[1] + 8 && b[2] < 1000 && b[3] < 1000) {
    g_box2.x0 = (int)b[0];
    g_box2.y0 = (int)b[1];
    g_box2.x1 = (int)b[2];
    g_box2.y1 = (int)b[3];
    g_box2Mode = 1;
    logBox2("switch hud_box_2");
  }
}
// a size answer from 82/02 for its picture buffer: smaller than the plane = the box fitted to it (unless the switch decided)
void mirrorSize(u16 w, u16 h) {
  g_mw = w;
  g_mh = h;
  if (g_box2Mode == 1 || g_box2Mode == 2 || !w || !h || w > 1023 || h > 1023)
    return;
  if (w >= 221 && h >= 158)
    return;  // the full plane: the default box stays
  g_box2Due = 1;
}
// one AddPolygon copy (handle, item, type, count, points): its points mapped from the 6WB picture's box into g_box2
void box2Points(u8 *m, u32 n) {
  if (g_box2Due) {
    g_box2Due = 0;
    fitBox2(g_mw, g_mh);
    g_box2Mode = 3;
    logf1("CarPlayMenu: HUD: 82/02's picture buffer is %u x %u", g_mw, g_mh);
    logBox2("fitted to 82/02's picture buffer");
  }
  if (g_box2Mode == 2)
    return;
  u32 k = m[5];
  if (6 + 4 * k > n)
    return;
  int dw = g_box2.x1 - g_box2.x0, dh = g_box2.y1 - g_box2.y0;
  for (u32 i = 0; i < k; i++) {
    u8 *p = m + 6 + 4 * i;
    int x = (s16)(p[0] << 8 | p[1]), y = (s16)(p[2] << 8 | p[3]);
    x = g_box2.x0 + (x - kSrcX0) * dw / 100;
    y = g_box2.y0 + (y - kSrcY0) * dh / 125;
    x = x < 0 ? 0 : x > 1023 ? 1023 : x;
    y = y < 0 ? 0 : y > 1023 ? 1023 : y;
    p[0] = (u8)(x >> 8);
    p[1] = (u8)x;
    p[2] = (u8)(y >> 8);
    p[3] = (u8)y;
  }
}
// only the answer for the copies' picture buffer counts
void mirrorAnswer(u16 h, u16 w, u16 ht) {
  if (h == mirrorHandle(hBufA))
    mirrorSize(w, ht);
}
// the other terminal's host while it gets copies (g_mirror), else 0
u16 mirrorHost() {
  int sl = slotOf(g_inst);
  return g_mirror && sl >= 0 ? g_hostOf[1 - sl] : 0;
}
// Every drawing call goes to the terminal drawn on and, while g_mirror, to the other one with its handles: all of them start with a
// handle, SwitchVisibility (20E) with two. Not the size question (C04): its answer steers this terminal only. primary: only here.
bool call(u16 fkt, const u8 *d, u32 n, bool primary = false, u32 tx = 0) {
  if (!g_registered || !g_host)
    return false;
  dbgOut(fkt, d, n);
  noteSent(fkt, d, n);
  bool ok = send(g_host, kFBlock, g_inst, fkt, kOpStartResultAck, d, n, tx);
  u16 mh = primary || fkt == 0xC04 ? 0 : mirrorHost();
  if (mh && n >= 2 && n <= 300) {
    static u8 m[300];
    memcpy(m, d, n);
    u16 h = mirrorHandle((u16)(d[0] << 8 | d[1]));
    m[0] = (u8)(h >> 8);
    m[1] = (u8)h;
    if (fkt == 0x20E && n >= 4) {
      h = mirrorHandle((u16)(d[2] << 8 | d[3]));
      m[2] = (u8)(h >> 8);
      m[3] = (u8)h;
    }
    u16 h0 = (u16)(d[0] << 8 | d[1]);
    if (fkt == kAddPolygonFkt && g_6wb && n >= 6 && (h0 == hBufA || h0 == hBufB))
      box2Points(m, n);
    noteSent(fkt, m, n);
    usleep(5000);
    send(mh, kFBlock, instOf(1 - slotOf(g_inst)), fkt, kOpStartResultAck, m, n);
  }
  usleep(20000);  // like the navigation: one call at a time, no bursts
  return ok;
}
// 1.3.9: the size question for the copies' picture buffer on the other terminal (both forms, transmission 0 and 4, as askSizes)
void askMirrorSize() {
  int sl = slotOf(g_inst);
  if (sl < 0 || !g_hostOf[1 - sl])
    return;
  u16 h = mirrorHandle(hBufA);
  u8 a[3] = { (u8)(h >> 8), (u8)h, 1 };
  for (u32 tx = 0; tx <= 4; tx += 4) {
    send(g_hostOf[1 - sl], kFBlock, instOf(1 - sl), 0xC04, kOpStartResultAck, a, 3, tx);
    send(g_hostOf[1 - sl], kFBlock, instOf(1 - sl), 0xC04, kOpStartResultAck, a, 2, tx);
  }
}
// "Media gestures": AudioMaster VolumeActual Increment (steps > 0) or Decrement, with the number of steps (u8)
bool volume(int steps) {
  u8 n = (u8)(steps > 0 ? (steps > 255 ? 255 : steps) : (-steps > 255 ? 255 : -steps));
  return send(g_amHost, kAudioMaster, 0, kVolumeActual, steps > 0 ? kOpIncrement : kOpDecrement, &n, 1);
}
void be16(u8 *o, u16 v) {
  o[0] = (u8)(v >> 8);
  o[1] = (u8)v;
}
// handle 0 is no instance ("instance 0 is forbidden", the navigation's trace): a text switched off by hud_text_handles
bool callH(u16 fkt, u16 h) {
  if (!h)
    return true;
  u8 d[2];
  be16(d, h);
  return call(fkt, d, 2);
}
bool callHB(u16 fkt, u16 h, u8 b, bool primary = false) {
  if (!h)
    return true;
  u8 d[3];
  be16(d, h);
  d[2] = b;
  return call(fkt, d, 3, primary);
}
bool switchVis(u16 hide, u16 show) {
  u8 d[4];
  be16(d, hide);
  be16(d + 2, show);
  return call(kSwitchVisibility, d, 4);
}
// 1.3.9 (issue #26: Chinese street names showed as "?????" on a 6WB cluster and its HUD): which MOST string coding a 6WB text goes
// in when it does not fit ISO-8859-15 (the coding byte after the handle: 00 UTF-16BE, 01 ISO-8859-15, 02 UTF-8, as in the MOST
// string type). kEncWide16 first (the 6WA's and the MINI's own coding, which showed Cyrillic there), then kEncWide8 if the cluster
// refuses that, then kEncLatin (only what ISO-8859-15 has; the rest left out, never '?'). Latin-only texts keep 01, JayJay's
// verified 6WB coding. Switch hud_text_enc: "auto" (default), "utf16", "utf8", "latin" (fixed, no fallback).
enum { kEncWide16 = 0, kEncWide8 = 1, kEncLatin = 2 };
// 1.3.11 (st1ng issue #27, Korea): Hangul. A cluster or HUD without Hangul in its font may refuse the text or draw nothing. If a
// text with Hangul is refused (within 3 s), the texts go again with Hangul romanized (Revised Romanization, syllable by syllable,
// without the sound changes between syllables; each word capitalised: 강남대로 -> Gangnamdaero). hud_text_enc "roman" always.
volatile int g_roman;        // Hangul romanized
volatile u32 g_hangulMs;     // when the last text with Hangul went out unromanized
// one Hangul syllable (U+AC00..U+D7A3) in Latin letters, into o (at least 8 bytes); its length. Divisions by constants only.
u32 romanSyllable(u32 c, char *o) {
  static const char *const kL[19] = { "g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h" };
  static const char *const kV[21] = { "a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae",
                                      "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i" };
  static const char *const kT[28] = { "", "k", "k", "k", "n", "n", "n", "t", "l", "k", "l", "l", "l", "l",
                                      "p", "l", "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t" };
  u32 x = c - 0xac00, t = x % 28, v = (x / 28) % 21, l = x / 588, n = 0;
  for (const char *p = kL[l]; *p; p++)
    o[n++] = *p;
  for (const char *p = kV[v]; *p; p++)
    o[n++] = *p;
  for (const char *p = kT[t]; *p; p++)
    o[n++] = *p;
  return n;
}
inline bool isHangul(u32 c) {
  return c >= 0xac00 && c <= 0xd7a3;
}
volatile int g_encStep;      // auto: the coding a wide text is tried in now
int g_encFixed = -1;         // hud_text_enc: the coding fixed (-1: auto)
volatile u32 g_wideMs;       // when the last 6WB text in a wide coding went out (a refusal within 3 s is taken for it)
volatile int g_textRedo;     // a wide text was refused: the texts again in the next coding (worker)
// a word at the start of a switch file (no strncmp in the plugin's build)
bool wordIs(const char *b, const char *w) {
  for (; *w; b++, w++)
    if (*b != *w)
      return false;
  return true;
}
void readTextEnc() {
  char path[96];
  snprintf(path, sizeof path, HUD_SWITCHES "%s", "hud_text_enc");
  int fd = open(path, 0);
  if (fd < 0)
    return;
  char b[16] = { 0 };
  long n = read(fd, b, sizeof b - 1);
  close(fd);
  (void)n;
  const char *name = 0;
  if (wordIs(b, "utf16") || wordIs(b, "utf-16"))
    g_encFixed = kEncWide16, name = "UTF-16";
  else if (wordIs(b, "utf8") || wordIs(b, "utf-8"))
    g_encFixed = kEncWide8, name = "UTF-8";
  else if (wordIs(b, "latin") || wordIs(b, "iso"))
    g_encFixed = kEncLatin, name = "ISO-8859-15 only (the rest left out)";
  else if (wordIs(b, "roman"))
    g_roman = 2, name = "Hangul romanized (switch value roman), the rest as before";
  if (name && g_log) {
    char m[120];
    snprintf(m, sizeof m, "CarPlayMenu: HUD: 6WB texts outside ISO-8859-15: %s (switch hud_text_enc)", name);
    g_log(m);
  }
}
// a SetText on the terminal drawn on was refused: if a wide one went out just before, the next coding (auto only)
void textRefused(u32 now) {
  if (!g_roman && g_hangulMs && now - g_hangulMs <= 3000) {  // 1.3.11: a text with Hangul refused: romanized next
    g_roman = 1;
    g_hangulMs = 0;
    g_wideMs = 0;
    g_textRedo = 1;
    if (g_log)
      g_log("CarPlayMenu: HUD: the cluster refused a text with Hangul: Hangul romanized next (switch hud_text_enc roman)");
    return;
  }
  if (g_encFixed >= 0 || !g_wideMs || now - g_wideMs > 3000 || g_encStep >= kEncLatin)
    return;
  g_encStep++;
  g_wideMs = 0;
  g_textRedo = 1;
  if (g_log)
    g_log(g_encStep == kEncWide8 ? "CarPlayMenu: HUD: the cluster refused a text in UTF-16: UTF-8 next"
                                 : "CarPlayMenu: HUD: the cluster refused a text in UTF-8: ISO-8859-15 only, the rest left out");
}

// One character of a text for the cluster's font (a code point; 0: left out). Typographic characters that apps put into street names
// (Yandex Navi's street names stopped showing while Apple's and Google's did, 2026-11-18; a cluster may refuse a text with a
// character its font lacks) become their plain forms: no-break and other spaces a space, dashes '-', quotes ' and ", the ellipsis
// '.'; invisible format characters (soft hyphen, zero width, direction marks, BOM) are left out. A comma: in the distance a point
// (hud_comma: the comma), elsewhere a space.
u32 textChar(u32 c, bool dist) {
  if (c == ',')
    return dist ? (g_comma ? ',' : '.') : ' ';
  if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0) || c > 0xffff)
    return ' ';
  if (c == 0xa0 || (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f || c == 0x3000)
    return ' ';
  if (c == 0xad || (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x2069) || c == 0xfeff)
    return 0;
  if ((c >= 0x2010 && c <= 0x2015) || c == 0x2212)
    return '-';
  if (c == 0x2018 || c == 0x2019 || c == 0x201a || c == 0x2032)
    return '\'';
  if (c == 0x201c || c == 0x201d || c == 0x201e || c == 0xab || c == 0xbb)
    return '"';
  if (c == 0x2026)
    return '.';
  return c;
}
// handle, coding (00 UTF-16BE, 01 ISO-8859-15, 02 UTF-8), the text, its end (00 00 / 00). The street names come from the iPhone in UTF-8; the MINI F54 cluster shows Latin with diacritics and
// Cyrillic (tested in the car with "ŽĐ Бул"), so they are converted, not stripped. Up to 32 characters; outside the BMP, control
// characters and broken sequences become a space. hud_text_handles may set a text's handle to 0: not drawn.
bool setText(u16 h, const char *s) {
  if (!h)
    return true;
  // the code points first (at most 32), then the coding: UTF-16BE (6WA, MINI); on a 6WB ISO-8859-15 when they all fit, else the
  // coding hud_text_enc / the fallback gives (1.3.9, issue #26)
  u32 cp[32], k = 0;
  bool latin = true;
  const u8 *q = (const u8 *)s;
  while (*q && k < 32) {
    u32 c = *q++, more = 0;
    if (c >= 0xf0) {
      c &= 0x07;
      more = 3;
    } else if (c >= 0xe0) {
      c &= 0x0f;
      more = 2;
    } else if (c >= 0xc0) {
      c &= 0x1f;
      more = 1;
    } else if (c >= 0x80) {
      c = ' ';
    }
    for (; more && (*q & 0xc0) == 0x80; more--)
      c = c << 6 | (*q++ & 0x3f);
    c = more ? ' ' : textChar(c, h == hDist);
    if (!c)
      continue;
    if (c > 0xff && c != 0x20ac)
      latin = false;
    cp[k++] = c;
  }
  bool hangul = false;
  for (u32 i = 0; i < k; i++)
    if (isHangul(cp[i]))
      hangul = true;
  if (hangul && g_roman) {  // 1.3.11: Hangul in Latin letters, each word capitalised; at most 32 characters as before
    u32 r[32], m = 0;
    bool wordStart = true;
    for (u32 i = 0; i < k && m < 32; i++) {
      if (isHangul(cp[i])) {
        char syl[8];
        u32 sn = romanSyllable(cp[i], syl);
        for (u32 j = 0; j < sn && m < 32; j++) {
          char ch = syl[j];
          r[m++] = (u32)(wordStart && j == 0 && ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        }
        wordStart = false;
      } else {
        r[m++] = cp[i];
        wordStart = cp[i] == ' ';
      }
    }
    k = m;
    latin = true;
    for (u32 i = 0; i < k; i++) {
      cp[i] = r[i];
      if (r[i] > 0xff && r[i] != 0x20ac)
        latin = false;
    }
    hangul = false;
  }
  if (hangul)
    g_hangulMs = nowMs() ? nowMs() : 1;
  int enc = !g_6wb ? kEncWide16 : latin ? kEncLatin : g_encFixed >= 0 ? g_encFixed : g_encStep;
  u8 d[2 + 1 + 32 * 3 + 2];
  u32 n = 0;
  be16(d, h);
  n = 2;
  d[n++] = enc == kEncWide16 ? 0 : enc == kEncWide8 ? 2 : 1;
  for (u32 i = 0; i < k; i++) {
    u32 c = cp[i];
    if (enc == kEncLatin) {
      if (c == 0x20ac)
        d[n++] = 0xa4;  // ISO-8859-15: the euro sign, the rest of Latin-1
      else if (c <= 0xff)
        d[n++] = (u8)c;
      else if (n > 3 && d[n - 1] != ' ')
        d[n++] = ' ';  // not in ISO-8859-15: left out (one space between the parts that are), never '?'
    } else if (enc == kEncWide8) {
      if (c < 0x80)
        d[n++] = (u8)c;
      else if (c < 0x800) {
        d[n++] = (u8)(0xc0 | c >> 6);
        d[n++] = (u8)(0x80 | (c & 0x3f));
      } else {
        d[n++] = (u8)(0xe0 | c >> 12);
        d[n++] = (u8)(0x80 | (c >> 6 & 0x3f));
        d[n++] = (u8)(0x80 | (c & 0x3f));
      }
    } else {
      d[n++] = (u8)(c >> 8);
      d[n++] = (u8)c;
    }
  }
  if (enc == kEncLatin)
    while (n > 3 && d[n - 1] == ' ')
      n--;
  d[n++] = 0;
  if (enc == kEncWide16)
    d[n++] = 0;
  if (g_6wb && enc != kEncLatin)
    g_wideMs = nowMs() ? nowMs() : 1;
  return call(kSetText, d, n);
}
// filled (type 3). Convex pieces of up to kPiecePts points, or with whole (a whole polygon, up to kPolyPts: not hulled)
bool polygon(u16 h, u16 idx, const Pt *p, u32 k, bool whole = false) {
  static Pt q[kPolyPts];
  if (k > (whole ? kPolyPts : kPiecePts))
    return false;
  for (u32 i = 0; i < k; i++)
    q[i] = p[i];
  if (g_boxMode != kBox6wa) {  // the MINI cluster's picture area (half size), or the box fitted to the buffer
    k = g_boxMode == kBoxMini ? (whole ? toMiniPoly(q, k) : toMini(q, k)) : whole ? toBoxPoly(q, k, g_box) : toBox(q, k, g_box);
    if (k < 3)
      return true;
  }
  if (g_6wb) {  // the 6WB's junction-view plane 0x1F/0x20 (hudshapes.h toWide: x 1:1, y 5/4 against its perspective, as 1.2.12)
    k = whole ? toWidePoly(q, k) : toWide(q, k);
    if (k < 3)
      return true;
  }
  static u8 d[6 + 4 * kPolyPts];
  be16(d, h);
  be16(d + 2, idx);
  d[4] = 3;
  d[5] = (u8)k;
  for (u32 i = 0; i < k; i++) {
    be16(d + 6 + 4 * i, (u16)q[i].x);
    be16(d + 8 + 4 * i, (u16)q[i].y);
  }
  if (!g_6wb || k + 1 > kPolyPts)
    return call(kAddPolygon, d, 6 + 4 * k);
  // 6WB: the navigation set its plane up itself (fill mode, line colour) and its own look is outlines; JayJay saw polylines
  // (type 2, line width 6) there. So the polygon goes as filled and again as a closed outline (item idx + 0x100).
  call(kAddPolygon, d, 6 + 4 * k);
  be16(d + 2, (u16)(idx + 0x100));
  d[4] = 2;
  d[5] = (u8)(k + 1);
  be16(d + 6 + 4 * k, (u16)q[0].x);
  be16(d + 8 + 4 * k, (u16)q[0].y);
  return call(kAddPolygon, d, 6 + 4 * (k + 1));
}

// Maneuver pictures: hudshapes.h (all 54 iAP2 / CPManeuverType types, junction side roads).

// ---- guidance state (librghook's file) --------------------------------------------------------------------------------------------
struct Guide {
  bool active, hold, arrived;  // arrived: route state 2 (1.4.6: the only state 0..6 that ends a picture at once besides a gap's end)
  u32 type, index, dist, step, units, side, jtype, nang, nl;
  int angle, ang[8];
  char distS[24], road[96];
  u32 roadFrom;  // 1.3.11: 0 the road after the maneuver, 1 the current road, 2 the maneuver's description, 3 none
  Lane lanes[8];
};
// 1.3.11 (st1ng issue #27, 1903 Build 41 in Korea, and the F10 of issue #25: no street on the HUD, "no street from the iPhone
// (maneuver_road and current_road empty)"): the street line falls back to the iPhone's ManeuverDescription (0x5202 field 2, which
// librghook already passed as maneuver_description), so the line is not left empty when the iPhone names no road. Switch hud_street
// "road": only road names, as before (g_streetDesc, declared at the top).
bool keyIs(const char *l, const char *k) {
  u32 i = 0;
  while (k[i] && l[i] == k[i])
    i++;
  return !k[i] && l[i] == ':';
}
const char *valOf(const char *l) {
  const char *c = l;
  while (*c && *c != ':')
    c++;
  if (*c)
    c++;
  while (*c && *c != ':')
    c++;
  return *c ? c + 1 : c;
}
// parses a decimal number with an optional leading "-" (reported through neg)
u32 num(const char *s, int *neg = 0) {
  bool m = *s == '-';
  if (m)
    s++;
  u32 v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (u32)(*s++ - '0');
  if (neg)
    *neg = m;
  return v;
}
void copyS(char *d, u32 cap, const char *s) {
  u32 i = 0;
  while (s[i] && s[i] != '\n' && i + 1 < cap) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}
bool g_noFile;  // no file: no iPhone session with guidance
bool readGuide(Guide &g) {
  memset(&g, 0, sizeof g);
  int fd = open(kPps, 0);
  g_noFile = fd < 0;
  if (fd < 0)
    return false;
  char b[2048];
  long n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0)
    return false;
  b[n] = 0;
  u32 rs = 0;
  bool man = false;
  char cur[96] = "", desc[96] = "";
  for (char *l = b; *l;) {
    char *e = l;
    while (*e && *e != '\n')
      e++;
    char sv = *e;
    *e = 0;
    const char *v = valOf(l);
    if (keyIs(l, "route_state"))
      rs = num(v);
    else if (keyIs(l, "dist_to_maneuver"))
      g.dist = num(v);
    else if (keyIs(l, "dist_to_maneuver_string"))
      copyS(g.distS, sizeof g.distS, v);
    else if (keyIs(l, "dist_to_maneuver_units"))
      g.units = num(v);
    else if (keyIs(l, "maneuver_step_length"))
      g.step = num(v);  // rghook.c: the maneuver's step length (0x5202 TLV 5), metres
    else if (keyIs(l, "maneuver_junction_type"))
      g.jtype = num(v);
    else if (keyIs(l, "maneuver_junction_angles")) {  // "-90,0,90"
      g.nang = 0;
      for (const char *q = v; *q && g.nang < 8;) {
        int sg = 1;
        if (*q == '-') {
          sg = -1;
          q++;
        }
        int x = 0;
        bool dig = false;
        while (*q >= '0' && *q <= '9') {
          x = x * 10 + (*q++ - '0');
          dig = true;
        }
        if (dig)
          g.ang[g.nang++] = sg * x;
        while (*q && *q != ',' && *q != '\n')
          q++;
        if (*q == ',')
          q++;
        else
          break;
      }
    } else if (keyIs(l, "lanes")) {  // "status/highlight/angle,angle;..." left to right (rghook.c)
      g.nl = 0;
      for (const char *q = v; *q && *q != '\n' && g.nl < 8;) {
        Lane &L = g.lanes[g.nl++];
        L.status = num(q);
        L.hl = 1000;
        L.na = 0;
        while (*q >= '0' && *q <= '9')
          q++;
        if (*q == '/') {
          q++;
          int ng;
          u32 h = num(q, &ng);
          L.hl = ng ? -(int)h : (int)h;
          if (*q == '-')
            q++;
          while (*q >= '0' && *q <= '9')
            q++;
        }
        if (*q == '/')
          q++;
        while (*q && *q != ';' && *q != '\n') {
          int ng;
          u32 a = num(q, &ng);
          if (*q == '-')
            q++;
          bool dig = *q >= '0' && *q <= '9';
          while (*q >= '0' && *q <= '9')
            q++;
          if (dig && L.na < 4)
            L.ang[L.na++] = ng ? -(int)a : (int)a;
          if (*q == ',')
            q++;
          else
            break;
        }
        while (*q && *q != ';' && *q != '\n')
          q++;
        if (*q == ';')
          q++;
      }
    } else if (keyIs(l, "maneuver_index")) {
      g.index = num(v);
      man = true;
    } else if (keyIs(l, "maneuver_type"))
      g.type = num(v);
    else if (keyIs(l, "maneuver_exit_angle")) {
      int ng;
      u32 a = num(v, &ng);
      g.angle = ng ? -(int)a : (int)a;
    } else if (keyIs(l, "maneuver_driving_side"))
      g.side = num(v);
    else if (keyIs(l, "maneuver_road"))
      copyS(g.road, sizeof g.road, v);
    else if (keyIs(l, "current_road"))
      copyS(cur, sizeof cur, v);
    else if (keyIs(l, "maneuver_description"))
      copyS(desc, sizeof desc, v);
    *e = sv;
    l = sv ? e + 1 : e;
  }
  g.roadFrom = 0;
  if (!g.road[0]) {
    copyS(g.road, sizeof g.road, cur);
    g.roadFrom = 1;
  }
  if (!g.road[0] && g_streetDesc) {
    copyS(g.road, sizeof g.road, desc);
    g.roadFrom = 2;
  }
  if (!g.road[0])
    g.roadFrom = 3;
  g.active = man && (rs == 1 || rs == 5 || rs == 6);  // route active, recalculating, heading to the route
  g.hold = rs == 3 || rs == 4;                        // Loading / Locating (seen in the car between routes): keep the last picture
  g.arrived = rs == 2;
  return true;
}
void splitDistText(const Guide &g, char *num_, char *unit) {  // "450 m" -> "450" + "m"; without the iPhone's text, metres
  num_[0] = unit[0] = 0;
  if (g.distS[0]) {
    u32 i = 0, k = 0;
    while (g.distS[i] == ' ')
      i++;
    while (g.distS[i] && g.distS[i] != ' ' && k < 8)
      num_[k++] = g.distS[i++];
    num_[k] = 0;
    while (g.distS[i] == ' ')
      i++;
    copyS(unit, 8, g.distS + i);
  } else if (g.dist >= 1000) {
    snprintf(num_, 9, "%u.%u", g.dist / 1000, (g.dist % 1000) / 100);
    copyS(unit, 8, "km");
  } else {
    snprintf(num_, 9, "%u", g.dist);
    copyS(unit, 8, "m");
  }
}
// iAP2 distance units (RouteGuidance): 0 km, 1 mi, 2 m, 3 yd, 4 ft (in the car: "4" with 0 for 3961 m, "50" with 2 for 50 m)
void splitDist(const Guide &g, char *num_, char *unit) {
  splitDistText(g, num_, unit);
  if (g.distS[0] && !unit[0]) {
    static const char *const kU[5] = { "km", "mi", "m", "yd", "ft" };
    if (g.units < 5)
      copyS(unit, 8, kU[g.units]);
  }
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------------
volatile int g_enabled;
bool g_shown, g_fresh = true;
u16 g_visible = 6;  // hBufB (a constant: no static initialiser in the plugin)
u32 g_lastType = ~0u, g_lastIndex = ~0u;
int g_lastAngle, g_lastJ;
char g_lastRoad[96], g_lastNum[9], g_lastUnit[8];

u16 otherBuf(u16 b) {
  return b == hBufA ? hBufB : hBufA;
}
// the effect colour: hud_colour_day's while the HMI says day, else the effect colour
u8 fxColour() {
  return g_night == 0 && g_fxDay >= 0 ? (u8)g_fxDay : g_fxColour;
}
u8 toneColour(u32 t) {
  return t == 2 ? fxColour() : t == 1 ? g_fillDim : (u8)kFill;
}
Shape g_shape;
Polys g_polys;  // large: not on the worker thread's stack
bool g_blinking;
u32 g_level = ~0u, g_blinkTick;  // hud_fill / hud_blink state: the level drawn, both buffers hold a phase
// one picture into buffer buf (emptied first). level: the progress fill 0..8 (hud_fill); high: the blink's bright phase, the whole
// maneuver in the effect colour. The lanes are added after the effect, so they never fill or blink.
void drawPicture(u16 buf, const Guide &g, u32 level, bool high) {
  Shape &s = g_shape;
  s = shapeOf(g.type, g.angle, g.side, g.jtype, g.ang, g.nang);
  if (high)
    allTone(s, 0, 2);
  else
    applyFill(s, level);
  addLanes(s, g.lanes, g.nl, g.side == 1);  // the iPhone's lanes under the arrow, while it shows them
  callH(kRemoveAll, buf);
  callHB(kSetLineWidth, buf, g_6wb ? 6 : 0);  // 6WB: the outlines (polygon); JayJay: 6 looks right, 0x16 far too thick
  callHB(kSetFillColor, buf, kFill);          // as the BMW navigation does
  u32 tone = 0;
  if (g_concave) {  // whole polygons (always, unless the test switch hud_convex)
    mergePieces(s, g_polys);
    for (u32 i = 0; i < g_polys.n; i++) {
      const Poly &q = g_polys.q[i];
      if (q.t != tone) {
        tone = q.t;
        callHB(kSetFillColor, buf, toneColour(tone));
      }
      polygon(buf, (u16)i, q.p, q.n, true);
    }
  } else {
    for (u32 i = 0; i < s.n; i++) {
      if (s.t[i] != tone) {
        tone = s.t[i];
        callHB(kSetFillColor, buf, toneColour(tone));
      }
      polygon(buf, (u16)i, s.p[i], s.k[i]);
    }
  }
}
// The buffer now shown: SwitchVisibility, as the navigation does; on a 6WB also SetVisibility of both planes (1.3.2), as
// nbt-android-auto's car-verified 6WB sender does (it shows 0x1F with 204 and hides 0x20 itself, and never relies on the swap).
void showBuf(u16 old, u16 now) {
  switchVis(old, now);
  if (g_6wb) {
    callHB(kSetVisibility, now, 1);
    callHB(kSetVisibility, old, 0);
  }
}
u32 g_firstMs;  // the session's first picture, for one summary line 3 s later (worker)
bool g_firstDone;
void drawManeuver(const Guide &g, const char *dn, const char *du, u32 level = 0) {
  u16 hidden = otherBuf(g_visible);
  drawPicture(hidden, g, level, false);
  showBuf(g_visible, hidden);
  callH(kRemoveAll, g_visible);
  g_visible = hidden;
  if (!g_firstDone && !g_firstMs) {
    u32 t = nowMs();
    g_firstMs = t ? t : 1;
  }
  if (g_fresh) {
    callHB(kSetVisibility, hStreet, 1);
    callHB(kSetVisibility, hDist, 1);
    callHB(kSetVisibility, hUnit, 1);
    g_fresh = false;
  }
  setText(hStreet, g.road);
  setText(hDist, dn);
  setText(hUnit, du);
  callHB(kSetVisibility, hFrame, 1);
  // The frame's distance bar (its handle known from the set-up): we never set its value, so it would show whatever the cluster has
  // (an F30 6WB showed a bar over the speed, PR #13): hidden, on both terminals.
  if (hBar)
    callHB(kSetVisibility, hBar, 0);
  for (u32 i = 0; i < g_nHide; i++)  // hud_hide: surfaces the cluster shows itself over other content (this terminal only)
    callHB(kSetVisibility, g_hide[i], 0, true);
  g_shown = true;
}
void drawClear() {  // the state the BMW navigation leaves the terminal in at the end of a route
  callHB(kSetVisibility, hFrame, 0);
  callH(kRemoveAll, hBufA);
  callH(kRemoveAll, hBufB);
  if (g_visible != hBufB) {
    switchVis(hBufA, hBufB);
    g_visible = hBufB;
  }
  setText(hStreet, "");
  setText(hDist, "");
  setText(hUnit, "");
  callHB(kSetVisibility, hStreet, 0);
  g_shown = false;
  g_fresh = true;
  g_lastType = g_lastIndex = ~0u;
  g_blinking = false;
  g_level = ~0u;
}

// Test channel: /dev/shmem/carplaymenu-hudcmd, one HUDTerminal call per line, "fkt hexbytes" (e.g. "21d 0005000003040070007800800078"),
// a line "@2" sends the following calls to HUDTerminal 82/02 ("@0" back to 82/00; no copies to the other terminal here),
// run once and the file deleted; to try a cluster's layout without rebuilding the plugin. Lines starting with # are skipped. Each
// batch restarts the 600-line HUD debug log, so its calls and the cluster's answers land in the log (tools/hud_probe.sh).
const char *const kCmd = "/dev/shmem/carplaymenu-hudcmd";
void runCmds() {
  int fd = open(kCmd, 0);
  if (fd < 0)
    return;
  static char b[8192];
  long n = read(fd, b, sizeof b - 1);
  close(fd);
  unlink(kCmd);
  if (n <= 0)
    return;
  b[n] = 0;
  g_dbg = 0;  // log this batch and the cluster's answers, even after the first 600 lines and without debug_log
  u32 pt = nowMs();
  g_probeMs = pt ? pt : 1;
  u32 ran = 0;
  u8 inst = 0;  // "@2": the following calls go to 82/02
  for (char *l = b; *l;) {
    char *e = l;
    while (*e && *e != '\n')
      e++;
    char sv = *e;
    *e = 0;
    if (*l == '@') {
      inst = (u8)(l[1] == '2' ? 2 : 0);
      if (!g_hostOf[inst ? 1 : 0])
        logf1("CarPlayMenu: HUD: test channel: no HUDTerminal 82/%02x in the registry, its calls are skipped", inst);
    } else if (*l && *l != '#') {
      u32 fkt = 0;
      char *q = l;
      while (hexv(*q) >= 0)
        fkt = fkt << 4 | (u32)hexv(*q++);
      u8 d[128];
      u32 k = 0;
      int hi = -1;
      for (; *q && k < sizeof d; q++) {
        int v = hexv(*q);
        if (v < 0)
          continue;
        if (hi < 0)
          hi = v;
        else {
          d[k++] = (u8)(hi << 4 | v);
          hi = -1;
        }
      }
      u16 host = g_hostOf[inst ? 1 : 0];  // exactly the terminal asked for, no copies
      if (fkt && host) {
        noteSent((u16)fkt, d, k);
        dbgLine(inst ? "out 82/02" : "out ", (u16)fkt, kOpStartResultAck, d, k);
        send(host, kFBlock, inst, (u16)fkt, kOpStartResultAck, d, k);
        usleep(20000);
        ran++;
      }
    }
    *e = sv;
    l = sv ? e + 1 : e;
  }
  logf1("CarPlayMenu: HUD: test channel ran %u calls", ran);
}

// The installer's choice (carplaymenu/cluster: most / kombi / both). "kombi": librghook-kombi.so sends the directions to the Ethernet
// KOMBI and nothing is drawn here, even on a car that also has a MOST HUDTerminal; "most", "both" or no file: drawn here as before.
bool kombiOnly() {
  int fd = open("/var/opt/hmi/data/rw/carplaymenu/cluster", 0);
  if (fd < 0)
    return false;
  char b[8] = { 0 };
  long n = read(fd, b, sizeof b - 1);
  close(fd);
  (void)n;
  return b[0] == 'k';
}

// ---- set-up: terminals, handles, picture size -----------------------------------------------------------------------------------
// The handle map per terminal, kept for the next start and for the sender in mm-ipod (it starts long after the navigation's set-up):
// hud_handles (82/00), hud_handles_2 (82/02), "frame street distance unit pictureA pictureB bar" (hudhandles.h format / parse).
const char *handlesFile(int slot) {
  return slot ? "/var/opt/hmi/data/rw/carplaymenu/hud_handles_2" : "/var/opt/hmi/data/rw/carplaymenu/hud_handles";
}
void saveMap(int slot, const hudh::Map &m) {
  char t[64];
  u32 n = hudh::format(m, t, sizeof t);
  if (!n)
    return;
  int fd = open(handlesFile(slot), 0x301 /* O_WRONLY | O_CREAT | O_TRUNC */, 0644);
  if (fd < 0)
    return;
  write(fd, t, n);
  close(fd);
}
bool loadMap(int slot, hudh::Map &m) {
  int fd = open(handlesFile(slot), 0);
  if (fd < 0)
    return false;
  char t[64];
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  if (n <= 0)
    return false;
  t[n] = 0;
  return hudh::parse(t, m);
}
void logMap(const char *head, const hudh::Map &m, const char *why) {
  char t[200];
  snprintf(t, sizeof t, "CarPlayMenu: HUD: %s: panel %u street %u distance %u unit %u picture %u/%u bar %u (%s)", head,
           (u32)m.frame, (u32)m.street, (u32)m.dist, (u32)m.unit, (u32)m.bufA, (u32)m.bufB, (u32)m.bar, why);
  if (g_log)
    g_log(t);
}
// hud_text_handles (ghackt, issue #9): "<street> <distance> <unit>" (decimal or 0x hex; 0 = that text not drawn) for a cluster whose
// texts the set-up did not give (the F25 X3: 0x0c 0x12 0x0f, verified by its owner)
// On a 6WB every guidance instance is 0x17 or above: handles below that are a 6WA cluster's (issue #10, 1.2.11: a BMW F36 with a 6WB
// tried issue #9's F25 workaround "0x0c 0x12 0x0f", which moved the texts where the 6WB has nothing). Not taken there, and logged.
void textOverride() {
  u32 th[3];
  if (readNums("hud_text_handles", th, 3) == 3 && th[0] < 0x100 && th[1] < 0x100 && th[2] < 0x100) {
    for (int i = 0; i < 3; i++)
      if (g_6wb && th[i] && th[i] < 0x17) {
        logf1("CarPlayMenu: HUD: hud_text_handles %u %u %u ignored: 6WA instances on a 6WB (texts on 0x1a 0x1b 0x1c; delete it)", th[0],
              th[1], th[2]);
        return;
      }
    hStreet = (u16)th[0];
    hDist = (u16)th[1];
    hUnit = (u16)th[2];
    logf1("CarPlayMenu: HUD: texts on street %u, distance %u, unit %u (switch hud_text_handles)", hStreet, hDist, hUnit);
  }
}
bool g_measureDue = true;  // the picture buffer's size is to be asked (at start, and after the handles changed)
void drawClear();
// the terminal drawn on gets these handles; what is on show is cleared first (on the old ones) and drawn again on the new ones
void useMap(const hudh::Map &m, const char *why) {
  if (g_shown)
    drawClear();
  hFrame = m.frame;
  hStreet = m.street;
  hDist = m.dist;
  hUnit = m.unit;
  hBufA = m.bufA;
  hBufB = m.bufB;
  hBar = m.bar;
  g_visible = hBufB;  // drawClear's rule: buffer B is the one shown when nothing is drawn
  g_mapKnown = true;
  g_measureDue = true;
  char head[40];
  snprintf(head, sizeof head, "handles on 82/%02x", g_inst);
  logMap(head, m, why);
  textOverride();
}
// the other terminal's map: its copies use it, and (unless hud_inst decided) the copies start
void useMirrorMap(const hudh::Map &m, const char *why) {
  g_mmap = m;
  g_mmapKnown = true;
  char head[48];
  snprintf(head, sizeof head, "handles on 82/%02x (copies)", instOf(1 - slotOf(g_inst)));
  logMap(head, m, why);
  if (!g_mirrorFixed && !g_mirror) {
    g_mirror = 1;
    logf1("CarPlayMenu: HUD: copies to HUDTerminal 82/%02x (the navigation set it up; hud_inst \"%u\" stops them)",
          instOf(1 - slotOf(g_inst)), g_inst);
  }
}
// a set-up the DSI thread completed (g_learnt): kept, and used
void takeLearnt() {
  int me = slotOf(g_inst);
  for (int sl = 0; sl < 2; sl++) {
    int v = g_learnt[sl];
    if (!v)
      continue;
    g_learnt[sl] = 0;
    hudh::Map m = g_learntMap[sl];
    saveMap(sl, m);
    const char *why = v == 2 ? "from the navigation's set-up, frames 11-13, untried" : "from the navigation's set-up";
    if (sl == me)
      useMap(m, why);
    else
      useMirrorMap(m, why);
  }
}

// 1.3.8 (ghackt/Carplay-advanced-installer issue #2, BMW F10 6WB with windscreen HUD, HUD-only): on a 6WB both HUDTerminals were
// on the cluster (82/00 and 82/02 at 0102) and nothing went to 82/02, the probable windscreen HUD (F-series clusters render its
// picture), because the copies only started from the navigation's set-up, which a HUD-only install starts too late to see. On a
// 6WB with 82/02 in the registry the copies now start with the same 6WB handles (82/02's own set-up, once seen, replaces them).
// Not on a 6WA or a MINI (there 82/02 is the kombi terminal); hud_inst decides instead ("0": no copies).
void mirror6wb() {
  int sl = slotOf(g_inst);
  if (g_mirrorFixed || g_mirror || sl < 0 || !g_hostOf[1 - sl])
    return;
  g_mirror = 1;
  if (g_mmapKnown)
    logf1("CarPlayMenu: HUD: 6WB: copies to HUDTerminal 82/%02x at device %04x with its set-up's handles (hud_inst \"%u\" stops them)",
          instOf(1 - sl), g_hostOf[1 - sl], g_inst);
  else
    logf1("CarPlayMenu: HUD: 6WB: copies to HUDTerminal 82/%02x at device %04x with the same 6WB handles (hud_inst \"%u\" stops them)",
          instOf(1 - sl), g_hostOf[1 - sl], g_inst);
}
void use6wb(const char *why) {
  g_6wb = true;
  g_boxMode = kBox6wa;  // the plane has its own mapping (polygon)
  if (!g_mapKnown) {
    hFrame = 0x17;
    hStreet = 0x1a;
    hDist = 0x1b;
    hUnit = 0x1c;
    hBufA = 0x1f;
    hBufB = 0x20;
    g_visible = hBufB;
    textOverride();
  }
  if (g_log) {
    char m[160];
    snprintf(m, sizeof m, "CarPlayMenu: HUD: cluster layout 6WB (%s): texts ISO-8859-15, outlines%s", why,
             g_mapKnown ? ", the set-up's handles" : "; area 0x17, texts 0x1a-0x1c, picture 0x1f/0x20");
    g_log(m);
  }
  mirror6wb();
}
// ImageGetSize (C04, it only reads) for the 6WB plane 0x1F and the 6WA picture 5, in the 6WB form [handle][01] and the F54 form
// [handle], each with transmission type 0 and 4 (1.3.2: nbt-android-auto asked a 6WB's sizes with 4, the BMW navigation's own)
void askSizes() {
  const u8 p1[] = { 0, 0x1f, 1 }, p2[] = { 0, 5, 1 }, p3[] = { 0, 0x1f }, p4[] = { 0, 5 };
  for (u32 tx = 0; tx <= 4; tx += 4) {
    call(0xC04, p1, 3, false, tx);
    call(0xC04, p2, 3, false, tx);
    call(0xC04, p3, 2, false, tx);
    call(0xC04, p4, 2, false, tx);
  }
}
// Which layout the cluster has: ImageGetSize for 0x1F and 5 (askSizes). Only the instances the navigation created have a size (the
// F54 answers 0 x 0 for the others, measured 2026-11-13). 0x1F alone with a size = 6WB; 1.3.8: 0x1F with the plane's 221 x 158 =
// 6WB even when 5 answers too (a BMW F10 6WB answered 5 with 126 x 126 and was drawn on the 6WA handles, unseen; issue #2). The switch file hud_layout ("6wa" or "6wb")
// decides instead. No size at all: the 6WA profile as a guess (g_layoutGuess), asked again later (layoutRetry) and given up for the
// 6WB layout when the cluster refuses the picture calls on the 6WA handles (refusedSwitch).
void detectLayout() {
  int fd = open("/var/opt/hmi/data/rw/carplaymenu/hud_layout", 0);
  if (fd >= 0) {
    char b[8] = { 0 };
    long n = read(fd, b, sizeof b - 1);
    close(fd);
    (void)n;
    if (b[0] == '6' && (b[2] == 'b' || b[2] == 'B')) {
      use6wb("switch file hud_layout");
      return;
    }
    if (b[0] == '6') {
      if (g_log)
        g_log("CarPlayMenu: HUD: cluster layout 6WA (switch file hud_layout)");
      return;
    }
  }
  if (fd < 0 && (g_car == kCar6wb || g_car == kCar6wbHud)) {  // the menu's car type (1.4.0)
    use6wb("car type BMW 6WB cluster");
    return;
  }
  if (fd < 0 && (g_car == kCar6wa || g_car == kCarMini)) {
    if (g_log)
      g_log(g_car == kCarMini ? "CarPlayMenu: HUD: cluster layout 6WA (car type MINI)"
                              : "CarPlayMenu: HUD: cluster layout 6WA (car type BMW 6WA cluster)");
    return;
  }
  askSizes();
  for (int i = 0; i < 30 && !(g_size1f && g_size5); i++)
    usleep(50000);
  logf1("CarPlayMenu: HUD: layout probe: ImageGetSize gave a size for 0x1f %u, for 5 %u", g_size1f, g_size5);
  if (g_size1f)
    logf1("CarPlayMenu: HUD: layout probe: 0x1f is %u x %u", g_w1f, g_h1f);
  if (g_plane1f)
    use6wb(g_size5 ? "ImageGetSize: 0x1f is the 221 x 158 plane, 5 answered too" : "ImageGetSize: 0x1f is the 221 x 158 plane");
  else if (g_size1f && !g_size5)
    use6wb("ImageGetSize");
  else if (g_size5) {
    if (g_log)
      g_log("CarPlayMenu: HUD: cluster layout 6WA");
  } else {
    g_layoutGuess = !g_mapKnown;
    if (g_log)
      g_log(g_mapKnown ? "CarPlayMenu: HUD: cluster layout not answered: the set-up's handles"
                       : "CarPlayMenu: HUD: cluster layout not answered: the 6WA profile for now (asked again; a 6WB if the cluster "
                         "refuses the 6WA handles)");
  }
}
// The size question unanswered: asked again every 10 s, 6 times (instances the navigation creates later, or a cluster slow to answer
// when the session starts). A later size for 0x1F alone switches to the 6WB layout. true = switched (the picture is drawn again).
u32 g_reprobeMs, g_reprobes;
bool layoutRetry(u32 now) {
  if (!g_layoutGuess || g_6wb || g_mapKnown)
    return false;
  if (g_plane1f || (g_size1f && !g_size5)) {
    g_layoutGuess = 0;
    use6wb("ImageGetSize, asked again");
    return true;
  }
  if (g_size5) {
    g_layoutGuess = 0;
    if (g_log)
      g_log("CarPlayMenu: HUD: cluster layout 6WA (ImageGetSize, asked again)");
    return false;
  }
  if (g_reprobes >= 6 || (g_reprobeMs && now - g_reprobeMs < 10000))
    return false;
  if (!g_reprobeMs) {  // the first time: 10 s from now
    g_reprobeMs = now ? now : 1;
    return false;
  }
  g_reprobeMs = now ? now : 1;
  g_reprobes++;
  askSizes();
  return false;
}
// The 6WA profile guessed and the cluster refusing the picture calls on its handles while taking none: they are no instances
// there, so it is taken for a 6WB (1.3.2; a BMW 6WB that answered no size stayed dark with the 6WA handles, 2026-11-15). Once.
bool refusedSwitch() {
  if (!g_layoutGuess || g_6wb || g_mapKnown || g_picTaken || g_picRefused < 3)
    return false;
  g_layoutGuess = 0;
  logf1("CarPlayMenu: HUD: the cluster refused %u picture calls on the 6WA handles and took none: the 6WB layout", g_picRefused);
  use6wb("the 6WA handles were refused");
  g_picRefused = g_picTaken = 0;
  return true;
}
// The picture box from the buffer's own size (ghackt's rule, issues #9 and #12): a buffer smaller than the 6WA box gets a square of
// 25/36 of its shorter side (at most 100), centred across, two thirds down the room left. 120 x 72 gives (35,15)-(85,65), the F54's
// MINI box, drawn as before; a buffer that holds the 6WA box gets the 6WA box. 0 x 0 is what the F54 answers for a handle that is
// no instance (2026-11-13): nothing changes (ghackt took it for the MINI box on the F25 X3, untried there).
// The divisions are by constants (no division helper on QNX).
void fitBox(u32 w, u32 h) {
  char m[160];
  if (!w && !h) {  // not (yet) an instance: asked again after the navigation's set-up; on an F25 X3 without it, hud_box mini
    if (g_log)
      g_log("CarPlayMenu: HUD: the picture buffer is 0 x 0 (not created, or not a picture here): the box stays");
    return;
  }
  if (w < 24 || h < 24 || w > 1023 || h > 1023) {
    logf1("CarPlayMenu: HUD: picture buffer %u x %u: not used, the box stays", w, h);
    return;
  }
  if (w >= 170 && h >= 120) {
    logf1("CarPlayMenu: HUD: picture buffer %u x %u holds the 6WA box", w, h);
    g_boxMode = kBox6wa;
    return;
  }
  u32 sd = (w < h ? w : h) * 25 / 36;
  if (sd > 100)
    sd = 100;
  int x = (int)((w - sd) / 2), y = (int)((h - sd + 1) * 2 / 3);
  if (x == 35 && y == 15 && sd == 50)
    g_boxMode = kBoxMini;
  else {
    Box b = { x, y, x + (int)sd, y + (int)sd };
    g_box = b;
    g_boxMode = kBoxFit;
  }
  snprintf(m, sizeof m, "fitted to the picture buffer, %u x %u", w, h);
  logBox(m);
}
// ImageGetSize on the picture buffer, in the F54's form [handle] and the 6WB's [handle][01] (a cluster refuses the form it does not
// take); 2 s for an answer. A switch's box and the 6WB plane stay as they are.
void measureBox() {
  g_measureDue = false;
  g_askH = hBufA;
  g_askOk = 0;
  g_askErr = 0;
  const u8 f1[] = { (u8)(hBufA >> 8), (u8)hBufA }, f2[] = { (u8)(hBufA >> 8), (u8)hBufA, 1 };
  call(0xC04, f1, 2);
  call(0xC04, f2, 3);
  for (int i = 0; i < 40 && !g_askOk && g_askErr < 2; i++)
    usleep(50000);
  if (!g_askOk) {
    logf1(g_askErr ? "CarPlayMenu: HUD: the picture buffer %u's size was refused: the box stays (hud_box sets one)"
                   : "CarPlayMenu: HUD: no answer for the picture buffer %u's size: the box stays (hud_box sets one)",
          hBufA);
    return;
  }
  logf1("CarPlayMenu: HUD: picture buffer %u is %u x %u", hBufA, g_askW, g_askHt);
  if (g_boxFixed || g_6wb)
    return;
  fitBox(g_askW, g_askHt);
}
// the maps kept from an earlier start (also for the installer's clear of a left-over arrow, probe.cpp)
void loadKept() {
  hudh::Map m;
  int me = slotOf(g_inst);
  for (int sl = 0; sl < 2; sl++)
    if (!g_learnt[sl] && loadMap(sl, m)) {
      if (sl == me)
        useMap(m, "kept from an earlier start");
      else
        useMirrorMap(m, "kept from an earlier start");
    }
  if (!g_mapKnown)
    textOverride();
}
// once the terminal is known: the kept maps, the layout (the picture box follows in the worker)
void initTerminal() {
  loadKept();
  detectLayout();
}

// a new picture at the next step (the layout changed under it)
void redrawSoon() {
  g_shown = false;
  g_fresh = true;
  g_lastType = g_lastIndex = ~0u;
  g_blinking = false;
  g_level = ~0u;
}
// The worker's step for g_coverMs (1.4.5, above navSeen): true = covered, send nothing of ours this tick. The first covered tick
// forgets the picture (redrawSoon), so the tick after the other client's last call + kCoverMs draws it whole again.
volatile u32 g_coverFrom, g_coverLogs;
volatile bool g_coverOff;
bool coverStep(u32 now) {
  u32 c = g_coverMs;
  bool recent = c && now - c < kCoverMs;
  if (recent && !g_coverOff && (g_shown || g_coverFrom)) {
    if (!g_coverFrom) {
      g_coverFrom = now ? now : 1;
      if (g_coverLogs < 20 && g_log) {
        g_coverLogs++;
        g_log("CarPlayMenu: HUD: another client drew over the CarPlay picture (a warning?): CarPlay waits until it is done");
      }
      redrawSoon();  // nothing of ours until then, the whole picture after
    } else if (now - g_coverFrom > kCoverMaxMs) {
      g_coverOff = true;
      if (g_log)
        g_log("CarPlayMenu: HUD: the other client keeps drawing: ignored for this session, CarPlay guidance draws");
    }
    if (!g_coverOff)
      return true;
  }
  if (g_coverFrom) {
    g_coverFrom = 0;
    if (g_coverLogs <= 20 && g_log)
      g_log("CarPlayMenu: HUD: the other client is done: CarPlay guidance drawn again");
  }
  return false;
}
void *worker(void *) {
  u32 lastLog = 0;
  bool probed = false;
  u32 dbgTick = 0;
  int lastNight = -1;
  bool kombi = kombiOnly();
  u32 since = nowMs();
  bool waitLogged = false, navLogged = false;
  u32 navLogs = 0, lastLive = 0;
  const u32 kGuideGapMs = 8000;
  if (kombi && g_log)
    g_log("CarPlayMenu: HUD: cluster 'kombi': the directions go to the KOMBI from librghook-kombi.so, nothing drawn on MOST");
  for (;;) {
    // without a route every 1 s (the file only exists in a session with guidance); 300 ms while an effect runs (the blink's 600 ms phase)
    usleep(g_noFile && !g_shown ? 1000000 : g_shown && (g_fillOn || g_blinkOn) ? 300000 : 500000);
    if (!g_registered || !g_host) {  // said once (1.3.2): nothing can be drawn before both
      if (!waitLogged && nowMs() - since > 20000 && g_log) {
        waitLogged = true;
        char m[200];
        snprintf(m, sizeof m, "CarPlayMenu: HUD: after 20 s still %s: nothing can be drawn yet",
                 !g_attached     ? "not attached to the MOST service"
                 : !g_registered ? "not registered with the MOST service"
                                 : "no HUDTerminal 82/00 (or 82/02) in the MOST registry");
        g_log(m);
      }
      continue;
    }
    runCmds();
    if (kombi)
      continue;
    if (!probed) {
      probed = true;
      initTerminal();
    }
    takeLearnt();  // the navigation's set-up, if it came since
    if (g_mirror && g_6wb && !g_mirrorAsked && g_box2Mode != 1 && g_box2Mode != 2) {  // 1.3.9: the copies' box from 82/02's size
      g_mirrorAsked = true;
      askMirrorSize();
    }
    if (layoutRetry(nowMs()) || refusedSwitch())
      redrawSoon();
    if (g_textRedo) {  // a wide text refused (1.3.9): the texts again in the next coding
      g_textRedo = 0;
      redrawSoon();
    }
    if (g_firstMs && nowMs() - g_firstMs > 3000) {  // the first picture's answers (1.3.2): whether the cluster takes our calls
      g_firstMs = 0;
      g_firstDone = true;
      char m[220];
      snprintf(m, sizeof m,
               "CarPlayMenu: HUD: first picture on 82/%02x, layout %s, handles frame %u picture %u/%u: %u calls sent, %u answered, "
               "picture calls taken %u, refused %u",
               g_inst, g_6wb ? "6WB" : "6WA", hFrame, hBufA, hBufB, g_sent, g_answers, g_picTaken, g_picRefused);
      if (g_log)
        g_log(m);
    }
    if (g_measureDue && !g_shown && !(g_navMs && nowMs() - g_navMs < 5000))
      measureBox();
    if (g_night != lastNight) {  // day / night: an effect on show is drawn again in its colour
      lastNight = g_night;
      g_level = ~0u;
      g_blinking = false;
    }
    if (++dbgTick % 10 == 1) {  // about every 5 s: the menu's "HUD debug log"
      int f = open("/var/opt/hmi/data/rw/carplaymenu/hud_debug", 0);
      int on = f >= 0;
      if (f >= 0)
        close(f);
      if (on && !g_hudDebug) {
        g_dbg = 0;  // the next 600 HUDTerminal lines go to the log
        g_hudDebug = on;
        char m[240];
        snprintf(
            m, sizeof m,
            "CarPlayMenu: HUD: debug log on: layout %s, box %d, drawing on 82/%02x at %04x (82/00 %04x, 82/02 %04x, copies %d), "
            "%u calls sent, %u answered, size 0x1f %u / 5 %u, night %d",
            g_6wb ? "6WB" : "6WA", g_boxMode, g_inst, g_host, g_hostOf[0], g_hostOf[1], g_mirror, g_sent, g_answers, g_size1f,
            g_size5, g_night);
        if (g_log)
          g_log(m);
        hudh::Map cur = { hFrame, hStreet, hDist, hUnit, hBufA, hBufB, hBar };
        logMap("handles", cur, g_mapKnown ? "from a set-up" : "the profile's");
        if (g_mmapKnown)
          logMap("copies' handles", g_mmap, "from a set-up");
      } else if (!on && g_hudDebug && g_log)
        g_log("CarPlayMenu: HUD: debug log off");
      g_hudDebug = on;
    }
    if (g_seen6wb && !g_6wb && !g_shown && !g_mapKnown)  // the BMW navigation drew on 6WB handles: this cluster is a 6WB
      use6wb("the BMW navigation draws on 0x18..0x35");
    bool nav = g_navMs && nowMs() - g_navMs < 5000;  // the BMW navigation uses the terminal: it has priority
    if (nav) {
      if (!navLogged && navLogs < 20 && g_log) {  // each wait said once, without debug_log too (1.3.2)
        navLogs++;
        g_log(g_shown ? "CarPlayMenu: HUD: BMW navigation is guiding, CarPlay guidance yields"
                      : "CarPlayMenu: HUD: BMW navigation is drawing: CarPlay guidance waits");
      }
      navLogged = true;
      if (g_shown) {
        g_shown = false;
        g_fresh = true;
        g_lastType = g_lastIndex = ~0u;
        g_blinking = false;
        g_level = ~0u;
      }
      continue;
    }
    if (navLogged && g_log && navLogs <= 20)
      g_log("CarPlayMenu: HUD: BMW navigation stopped drawing: CarPlay guidance draws again");
    navLogged = false;
    if (coverStep(nowMs()))  // another client's drawing over our picture (a speed camera warning): ours waits (1.4.5)
      continue;
    Guide g;
    bool got = g_enabled && readGuide(g);
    bool have = got && g.active;
    // 1.4.6 (ghackt issue #5, G30 rghook.log: route state 1, 0, 3, 0, 3, 0, ... all through a route): No Route Set less than
    // kGuideGapMs after guidance or Loading keeps the picture, as the KOMBI has since 1.3.1 (NBT-AAD's grace_s). Until 1.4.6 every 0
    // cleared it at once, and it came back only with the next maneuver. Arrived still clears at once, a gap over 8 s too.
    if ((have || (got && g.hold)) && g_shown)
      lastLive = nowMs();
    if (!have && got && g_shown && !g.arrived && (g.hold || nowMs() - lastLive < kGuideGapMs))
      continue;
    if (!have) {
      if (g_shown) {
        drawClear();
        if (g_log)
          g_log("CarPlayMenu: HUD: cleared");
      }
      continue;
    }
    char dn[9], du[8];
    splitDist(g, dn, du);
    int jsum = (int)g.jtype * 7919 + (int)g.nang;
    for (u32 i = 0; i < g.nang; i++)
      jsum = jsum * 31 + g.ang[i];
    jsum = jsum * 131 + (int)g.nl;  // the lanes: a change redraws the picture
    for (u32 i = 0; i < g.nl; i++) {
      jsum = jsum * 31 + (int)g.lanes[i].status * 7 + g.lanes[i].hl;
      for (u32 j = 0; j < g.lanes[i].na; j++)
        jsum = jsum * 31 + g.lanes[i].ang[j];
    }
    bool same = g.type == g_lastType && g.index == g_lastIndex && g.angle == g_lastAngle && jsum == g_lastJ;
    bool roadSame = true;
    for (u32 i = 0; i < sizeof g_lastRoad; i++) {
      if (g.road[i] != g_lastRoad[i]) {
        roadSame = false;
        break;
      }
      if (!g.road[i])
        break;
    }
    // test switches hud_fill / hud_blink: the range is the step length, at most the cap (mib2q-carplay-rgi's bargraph denominator)
    u32 level = 0;
    bool blinkZone = false;
    // the start marker (type 11, "start route") neither fills nor blinks: it is not a turn to count down to
    if ((g_fillOn || g_blinkOn) && g.type != 11) {
      u32 zone = g.step && g.step < g_fxCap ? g.step : g_fxCap, d = g.dist < zone ? g.dist : zone;
      if (g_fillOn)
        level = (u32)udiv((int)(zone - d) * 8, (int)zone);
      blinkZone = g_blinkOn && g.dist <= (u32)udiv((int)zone, 5);
    }
    bool newMan = !same || !roadSame || !g_shown, first = !g_shown;  // first: a route's first picture, logged without debug_log too
    if (newMan || blinkZone != g_blinking || (!blinkZone && g_fillOn && level != g_level)) {
      if (blinkZone) {  // low phase on show (route colour), high phase in the other buffer
        drawManeuver(g, dn, du, 0);
        drawPicture(otherBuf(g_visible), g, 0, true);
        g_blinking = true;
        g_blinkTick = 0;
      } else {
        drawManeuver(g, dn, du, level);
        g_blinking = false;
      }
      g_level = level;
      if (newMan && (first || verbose()) && g.roadFrom == 3 && g_log)
        g_log(g_streetDesc ? "CarPlayMenu: HUD: no street from the iPhone (maneuver_road, current_road and maneuver_description empty)"
                           : "CarPlayMenu: HUD: no street from the iPhone (maneuver_road and current_road empty)");
      if (newMan && (first || verbose()) && g.roadFrom == 2 && g_log)
        g_log("CarPlayMenu: HUD: no road name from the iPhone: the street line shows the maneuver's description");
      if (newMan && (first || verbose()))
        logf1("CarPlayMenu: HUD: maneuver %u type %u, %u m", g.index, g.type, g.dist);
      else if (!newMan && verbose())
        logf1("CarPlayMenu: HUD: fill level %u, blink %u, %u m", level, blinkZone, g.dist);
    } else {
      if (g_blinking && ++g_blinkTick % 2 == 0) {
        u16 o = otherBuf(g_visible);
        showBuf(g_visible, o);
        g_visible = o;
      }  // 600 ms
      bool dSame = true, uSame = true;
      for (u32 i = 0; i < 9; i++) {
        if (dn[i] != g_lastNum[i]) {
          dSame = false;
          break;
        }
        if (!dn[i])
          break;
      }
      for (u32 i = 0; i < 8; i++) {
        if (du[i] != g_lastUnit[i]) {
          uSame = false;
          break;
        }
        if (!du[i])
          break;
      }
      if (!dSame)
        setText(hDist, dn);
      if (!uSame)
        setText(hUnit, du);
    }
    g_lastType = g.type;
    g_lastIndex = g.index;
    g_lastAngle = g.angle;
    g_lastJ = jsum;
    memcpy(g_lastRoad, g.road, sizeof g_lastRoad);
    memcpy(g_lastNum, dn, sizeof g_lastNum);
    memcpy(g_lastUnit, du, sizeof g_lastUnit);
    if (nowMs() - lastLog > 30000 && verbose()) {
      lastLog = nowMs();
      logf1("CarPlayMenu: HUD: %u calls sent, %u answered by the cluster", g_sent, g_answers);
    }
  }
  return 0;
}

bool g_started;
// Opens the MOST client and the drawing thread (once): for the HUD and for the "Media gestures" volume.
void start() {
  if (g_started)
    return;
  g_started = true;
  chooseBox();
  if (!dsiOpen()) {
    if (g_log)
      g_log("CarPlayMenu: HUD: MOST client not started");
    return;
  }
  int t;
  if (pthread_create(&t, 0, worker, 0) != 0 && g_log)
    g_log("CarPlayMenu: HUD: worker thread failed");
}
// Switches the drawing on/off; off only makes the thread clear the HUD.
void enable(bool on) {
  g_enabled = on ? 1 : 0;
  if (on)
    start();
}

}  // namespace hud
