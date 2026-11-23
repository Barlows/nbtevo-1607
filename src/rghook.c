/* rghook.c: CarPlay route guidance for the NBT Evo's iAP2 stack (QNX mm-ipod, "Media 1.0.70", 2017), which predates it.
   LD_PRELOAD into mm-ipod. Only read()/write() on the iAP2 transport descriptor are touched (found by its first iAP2 bytes,
   or $RGHOOK_DEV). The driver itself is not changed:

   outgoing  IdentificationInformation (0x1D01): adds 0x5200/0x5203 to MessagesSentByAccessory, 0x5201/0x5202/0x5204 to
             MessagesReceivedFromDevice and a RouteGuidanceDisplayComponent (param 30), then fixes lengths and checksums.
   inject    StartRouteGuidanceUpdates (0x5200) once IdentificationAccepted (0x1D02) arrives. The packet takes the next link
             sequence number; every later packet of the driver is renumbered (+delta) and every acknowledgement number the phone
             sends back is shifted (-delta), so both ends see a gap-free link.
   incoming  packets are buffered and handed to the driver whole (fields can be rewritten safely). RouteGuidanceUpdate (0x5201),
             RouteGuidanceManeuverUpdate (0x5202) and LaneGuidanceInformation (0x5204) are decoded: the current state goes to
             $RGHOOK_PPS (default /dev/shmem/carplaymenu-routeguidance, RG_PPS below; PPS-style "key::value" lines), every
             message to $RGHOOK_LOG.

   kombi     librghook-kombi.so only (built with RGHOOK_KOMBI, installed as librghook.so on cars with an Ethernet KOMBI: G-series,
             J29): the same guidance also goes to the KOMBI's route-guidance service, from a worker thread (rgkombi.cpp, kombi.h);
             rg_view_get() below hands it the state under st_lock. The plain librghook.so (MOST cars) has no network code.

   Parameter layout: same as the community implementation for VW/Audi MIB2 (github.com/WaylandAce/mib2q-carplay-rgi, rgd_tlv.h). */
#include "rgview.h"
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long size_t;
typedef int s32;
typedef short s16;
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int snprintf(char *, unsigned long, const char *, ...);
extern char *getenv(const char *);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern void *memmove(void *, const void *, unsigned long);
extern int strcmp(const char *, const char *);
extern unsigned long strlen(const char *);
extern int sched_yield(void);
static void *libc(const char *n) {
  return dlsym(dlopen("libc.so.3", 2), n);
}
static long (*r_read)(int, void *, size_t);
static long (*r_write)(int, const void *, size_t);
static int (*r_open)(const char *, int, ...);
static int (*r_close)(int);
static int (*r_unlink)(const char *);

#define RG_COMP_ID 0x0F0E /* identifier of our RouteGuidanceDisplayComponent (unique among the head unit's components) */
/* The decoded guidance for hud.h (in the HMI, or here in mm-ipod): a file in RAM since 1.4.4. Until then it was /var/iap/pps/routeguidance
   on the flash disk, created, written and renamed by the iAP2 reader thread at every change (about once a second while guiding): a
   slow flash write held that thread, and with it the iPhone's whole iAP2 link, until it finished. The old file is removed at start. */
#define RG_PPS      "/dev/shmem/carplaymenu-routeguidance"
#define RG_PPS_OLD  "/var/iap/pps/routeguidance"
#define RG_PPS_SIZE 2048 /* the record: the text, then NULs (publish) */
#define SYN        0x80
#define ACK        0x40
static int transport_fd = -1, logfd = -2, ctl_sess = -1, inited, injected;
static u32 delta, last_out_seq, last_in_seq;
static volatile int lock_word;
/* Sleeps instead of spinning: mm-ipod's threads have different fixed priorities, and a higher one looping on sched_yield() never let
   the holder (the reader thread, writing StartRouteGuidanceUpdates under the lock) run again: mm-ipod at 100% CPU, link stalled. */
extern int usleep(unsigned);
static void lock(void) {
  while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE))
    usleep(1000);
}
static void unlock(void) {
  __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
}
/* the decoded state (st) between the reader thread (on_rg) and the KOMBI worker (rg_view_get) */
static volatile int st_word;
static void st_lock(void) {
  while (__atomic_exchange_n(&st_word, 1, __ATOMIC_ACQUIRE))
    usleep(1000);
}
static void st_unlock(void) {
  __atomic_store_n(&st_word, 0, __ATOMIC_RELEASE);
}
/* write() on the other fds goes to the next one in the chain, not straight to libc: librghook.so comes FIRST in LD_PRELOAD (the
   CarPlayMenu plugin's script, src/cpm.cpp), and a library preloaded after it may need to see those writes. QNX libc has no useful
   RTLD_NEXT: the next one is looked up in the LD_PRELOAD libraries after this one (dlsym on each one's handle). */
long write(int, const void *, size_t);
static void *next_sym(const char *n, void *self) {
  const char *e = getenv("LD_PRELOAD");
  char path[256];
  int seen = 0;
  while (e && *e) {
    unsigned k = 0;
    while (e[k] && e[k] != ':' && k + 1 < sizeof path) {
      path[k] = e[k];
      k++;
    }
    path[k] = 0;
    e += k;
    if (*e == ':')
      e++;
    if (!k)
      continue;
    if (!seen) {
      unsigned m = 0;
      while (path[m])
        m++;
      if (m >= 12 && !strcmp(path + m - 12, "librghook.so"))
        seen = 1;
      continue;
    }
    void *h = dlopen(path, 2);
    void *f = h ? dlsym(h, n) : 0;
    if (f && f != self)
      return f;
  }
  return libc(n);
}
/* ---- "Restart CarPlay" asked from the menu (1.4.4) -----------------------------------------------------------------------------------
   The menu plugin runs in HmiMain3D, which may not be allowed to signal mm-ipod (EPERM: in the car the button restarted nothing, and
   1.4.1's reboot from the HMI did nothing either). This library is inside mm-ipod (with "CarPlay directions in HUD"), and a process
   may always signal itself. So the plugin also writes RG_RESTART_REQ, "CPR1 <ms>" (CLOCK_MONOTONIC, the same clock in every process),
   and a thread here, started with the iAP2 link, looks at it every 300 ms: a request made after this mm-ipod started and less than
   10 s old ends it (SIGTERM, SIGKILL 2 s later if it is still there), and Connectivity starts the next session. Everything is looked up
   in libc at run time (nothing new is linked); without it (the computer's tests) there is no thread. */
#define RG_RESTART_REQ "/dev/shmem/carplaymenu-cprestart"
struct rg_ts {
  long sec, nsec;
};
static int (*r_clock)(int, struct rg_ts *);
static u32 rg_start_ms;
static u32 rg_now_ms(void) {
  struct rg_ts t = { 0, 0 };
  if (!r_clock || r_clock(2 /* CLOCK_MONOTONIC */, &t) != 0)
    return 0;
  return (u32)(t.sec * 1000 + t.nsec / 1000000);
}
/* 1 = the request text c asks this mm-ipod (started at start, now = now) to end */
int rg_restart_due(const char *c, u32 start, u32 now) {
  if (!c || c[0] != 'C' || c[1] != 'P' || c[2] != 'R' || c[3] != '1' || c[4] != ' ')
    return 0;
  u32 ms = 0, nd = 0;
  for (const char *q = c + 5; *q >= '0' && *q <= '9' && nd < 10; q++, nd++)
    ms = ms * 10 + (u32)(*q - '0');
  return nd && (int)(ms - start) > 0 && (int)(now - ms) >= 0 && (int)(now - ms) < 10000;
}
static void log_text(const char *s);
int rg_file_read(const char *path, char *buf, unsigned cap);
int rg_lower_priority(int max);
static void *restart_watch(void *a) {
  (void)a;
  rg_lower_priority(10);
  int (*k)(int, int) = (int (*)(int, int))libc("kill");
  int (*gp)(void) = (int (*)(void))libc("getpid");
  int (*sl)(unsigned) = (int (*)(unsigned))libc("usleep");
  if (!k || !gp || !sl)
    return 0;
  for (;;) {
    sl(300000);
    char b[48];
    if (rg_file_read(RG_RESTART_REQ, b, sizeof b) > 0 && rg_restart_due(b, rg_start_ms, rg_now_ms())) {
      char m[96];
      snprintf(m, sizeof m, "restart: asked by the menu (Restart CarPlay): mm-ipod pid %d ends (SIGTERM)\n", gp());
      log_text(m);
      k(gp(), 15);
      sl(2000000);
      log_text("restart: still running 2 s after SIGTERM: SIGKILL\n");
      k(gp(), 9);
    }
  }
  return 0;
}
static void restart_watch_once(void) {
  static volatile int done;
  if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
    return;
  int (*pc)(int *, const void *, void *(*)(void *), void *) = (int (*)(int *, const void *, void *(*)(void *), void *))libc(
      "pthread_create");
  int t;
  if (!pc || !r_clock || pc(&t, 0, restart_watch, 0) != 0)
    log_text("restart: no watcher thread: Restart CarPlay works only where the HMI may signal mm-ipod\n");
}

static void rg_clear(void);
static void init(void) {
  if (inited)
    return;
  inited = 1;
  r_read = libc("read");
  r_write = next_sym("write", (void *)write);
  r_open = libc("open");
  r_close = libc("close");
  r_unlink = libc("unlink");
  r_clock = (int (*)(int, struct rg_ts *))libc("clock_gettime");
  rg_start_ms = rg_now_ms();
  rg_clear();
}
static volatile int log_opening;
static void log_text(const char *s) {
  if (logfd == -2) { /* opened (and truncated) once: the KOMBI / MOST worker may log first, with the route guidance thread */
    while (__atomic_exchange_n(&log_opening, 1, __ATOMIC_ACQUIRE))
      usleep(1000);
    if (logfd == -2) {
      const char *p = getenv("RGHOOK_LOG");
      logfd = r_open(p ? p : "/var/iap/rghook.log", 0x301, 0644);
    }
    __atomic_store_n(&log_opening, 0, __ATOMIC_RELEASE);
  }
  if (logfd >= 0)
    r_write(logfd, s, strlen(s));
}
/* A switch file in the plugin's settings folder ($RGHOOK_CFG in tests; /var may not be linked yet when mm-ipod starts): 1 = present. */
static int cfg_switch(const char *name) {
  init();
  const char *cfg = getenv("RGHOOK_CFG");
  for (int i = 0; i < 3; i++) {
    const char *d = i == 0 ? cfg : i == 1 ? "/var/opt/hmi/data/rw/carplaymenu" : "/fs/sda1/opt/hmi/data/ID5/carplaymenu";
    if (!d || !*d)
      continue;
    char p[192];
    snprintf(p, sizeof p, "%s/%s", d, name);
    int fd = r_open(p, 0);
    if (fd >= 0) {
      r_close(fd);
      return 1;
    }
  }
  return 0;
}
/* Switch debug_log: the per-message lines (every route guidance message, the lanes, the control message ids) only with it. Read once. */
int rg_debug_log(void) {
  static volatile int on = -1;
  if (on < 0)
    on = cfg_switch("debug_log");
  return on;
}
/* Test switch keep_start_marker: the start marker (type 11) stays shown even when the iPhone already lists the first turn after it
   (cur_view). Read once. */
static int keep_start_marker(void) {
  static volatile int on = -1;
  if (on < 0)
    on = cfg_switch("keep_start_marker");
  return on;
}
static u8 checksum(const u8 *b, u32 n) {
  u32 s = 0;
  while (n--)
    s += *b++;
  return (u8)(0x100 - (s & 0xff));
}
static u16 be16(const u8 *p) {
  return (u16)(p[0] << 8 | p[1]);
}
static void wbe16(u8 *p, u32 v) {
  p[0] = (u8)(v >> 8);
  p[1] = (u8)v;
}

/* ---------------- identification patch ---------------- */
static u32 tlv(u8 *o, u32 id, const void *d, u32 n) {
  wbe16(o, 4 + n);
  wbe16(o + 2, id);
  if (n)
    memcpy(o + 4, d, n);
  return 4 + n;
}
static u32 tlv16(u8 *o, u32 id, u32 v) {
  u8 b[2];
  wbe16(b, v);
  return tlv(o, id, b, 2);
}
static u32 rg_component(u8 *o) {
  u8 in[256];
  u32 k = 0;
  static const char nm[] = "Head-Up Display";
  k += tlv16(in + k, 0, RG_COMP_ID);
  k += tlv(in + k, 1, nm, sizeof nm); /* identifier, name (UTF-8, NUL-terminated) */
  k += tlv16(in + k, 2, 0x100);
  k += tlv16(in + k, 3, 0x100);
  k += tlv16(in + k, 4, 0x100);
  k += tlv16(in + k, 5, 0x100); /* max road/destination/after-road/description lengths */
  k += tlv16(in + k, 6, 6);     /* max guidance maneuver storage */
  k += tlv16(in + k, 7, 0x100);
  k += tlv16(in + k, 8, 6); /* lane guidance description length, storage */
  return tlv(o, 30, in, k);
}
/* msg = control message (40 40 len id params); returns new length (written to out) */
static u32 patch_ident(const u8 *msg, u32 n, u8 *out) {
  u32 o = 6, i = 6, have = 0;
  memcpy(out, msg, 6);
  while (i + 4 <= n) {
    u32 ln = be16(msg + i), id = be16(msg + i + 2);
    if (ln < 4 || i + ln > n)
      break;
    if (id == 30)
      have = 1;
    if (id == 6 || id == 7) {
      static const u16 snd[] = { 0x5200, 0x5203 }, rcv[] = { 0x5201, 0x5202, 0x5204 };
      const u16 *a = id == 6 ? snd : rcv;
      u32 na = id == 6 ? 2 : 3;
      memcpy(out + o, msg + i, ln);
      u32 add = 0;
      for (u32 j = 0; j < na; j++) {
        u32 dup = 0;
        for (u32 q = 4; q + 1 < ln; q += 2)
          if (be16(msg + i + q) == a[j])
            dup = 1;
        if (!dup) {
          wbe16(out + o + ln + add, a[j]);
          add += 2;
        }
      }
      wbe16(out + o, ln + add);
      o += ln + add;
    } else {
      memcpy(out + o, msg + i, ln);
      o += ln;
    }
    i += ln;
  }
  if (!have)
    o += rg_component(out + o);
  wbe16(out + 2, o);
  return o;
}

/* ---------------- route guidance decode ---------------- */
/* iOS sends the maneuvers (0x5202) and the lane sets (0x5204) of the whole route, each index once (CarPlay keeps what it has sent
   and does not send it again: mib2q-carplay-rgi, rgd_tlv.h / rgd_hook.c), in increasing order and long before they are needed. They
   are kept by their index (1.3.1). Before, they went into index % 32 / % 8 slots: on a longer route a later index took the slot of
   the current one, so the arrow came from another maneuver, and a lane set that came again took its slot back from the later one:
   the lane row went on and off. A new index takes a free slot, else the one needed least (key_slot). */
#define RG_MANS     128
#define RG_LANESETS 48
struct rgs_man {
  u32 used, idx, type, dist, side, jtype, nang;
  s32 exit_angle;
  u32 angle_src; /* exit_angle: 0 the iPhone's (field 11), 1 the description's clock direction, 2 the exit number on the junction */
  s16 ang[8];
  char road[128], desc[128], exit[16];
};
struct rgs_lanes {
  u32 used, idx, n;
  struct {
    u32 pos, status, na;
    s32 hl;
    s16 ang[4];
  } l[8];
};
static struct {
  u32 route_state, maneuver_state, dist, dist_units, rem, rem_units, eta, cur_idx, has_cur, next_idx, has_next;
  char road[128], dest[128], dist_s[32], rem_s[32], src[64];
  struct rgs_man man[RG_MANS];
  /* lane guidance: RouteGuidanceUpdate 16 LaneGuidanceIndex, 18 LaneGuidanceShowing; LaneGuidanceInformation (0x5204) sets, by index */
  u32 lane_idx, has_lane_idx, lane_show, has_lane_show;
  struct rgs_lanes lanes[RG_LANESETS];
} st;
static const struct rgs_man no_man; /* a maneuver not received (all zero) */
struct rgs_key {
  u32 used, idx;
};
#define KEY_AT(base, size, i) ((struct rgs_key *)((u8 *)(base) + (size) * (i)))
static int key_find(void *base, u32 n, u32 size, u32 idx) {
  for (u32 i = 0; i < n; i++)
    if (KEY_AT(base, size, i)->used && KEY_AT(base, size, i)->idx == idx)
      return (int)i;
  return -1;
}
/* The slot for index idx: its own, a free one, else one already passed (the lowest index below the current one: iOS counts on, and
   a new route sends its own again), else the farthest ahead if idx is nearer than it. -1 = not kept (idx is farther than all, or
   behind the current one). The current index and the one after it are never taken. */
static int key_slot(void *base, u32 n, u32 size, u32 idx, int has_cur, u32 cur, int has_next, u32 next) {
  int f = key_find(base, n, size, idx), passed = -1, ahead = -1;
  if (f >= 0)
    return f;
  for (u32 i = 0; i < n; i++) {
    const struct rgs_key *k = KEY_AT(base, size, i);
    if (!k->used)
      return (int)i;
    if ((has_cur && k->idx == cur) || (has_next && k->idx == next))
      continue;
    if (has_cur && k->idx < cur) {
      if (passed < 0 || k->idx < KEY_AT(base, size, passed)->idx)
        passed = (int)i;
    } else if (ahead < 0 || k->idx > KEY_AT(base, size, ahead)->idx)
      ahead = (int)i;
  }
  if (passed >= 0)
    return passed;
  if (has_cur && idx < cur)
    return -1; /* behind the current one */
  return ahead >= 0 && idx < KEY_AT(base, size, ahead)->idx ? ahead : -1;
}
static const struct rgs_man *man_get(u32 idx) {
  int i = key_find(st.man, RG_MANS, sizeof st.man[0], idx);
  return i >= 0 ? &st.man[i] : &no_man;
}
static const struct rgs_lanes *lanes_get(u32 idx) {
  int i = key_find(st.lanes, RG_LANESETS, sizeof st.lanes[0], idx);
  return i >= 0 ? &st.lanes[i] : 0;
}
static void cpstr(char *d, u32 cap, const u8 *s, u32 n) {
  u32 k = 0;
  while (k < n && k + 1 < cap && s[k]) {
    d[k] = (char)s[k];
    k++;
  }
  d[k] = 0;
}
static u32 uint_be(const u8 *p, u32 n) {
  u32 v = 0;
  for (u32 i = 0; i < n && i < 4; i++)
    v = v << 8 | p[n > 4 ? n - 4 + i : i];
  return v;
} /* BE, low 32 bits */
/* The distance in the iPhone's units, from metres (for a maneuver the iPhone sent no distance text for yet; cur_view). Units: 0 km,
   1 mi, 2 m, 3 yd, 4 ft. Integer only (no soft-float in the car's libc): one decimal for km and mi. */
static void fmt_dist(char *o, u32 cap, u32 m, u32 units) {
  if (units == 1 || units == 3 || units == 4) {
    u32 ft = (m * 3281 + 500) / 1000, yd = (m * 1094 + 500) / 1000;
    if (units == 3 && yd < 500)
      snprintf(o, cap, "%u yd", yd < 100 ? (yd + 5) / 10 * 10 : (yd + 25) / 50 * 50);
    else if (units != 3 && ft < 1000)
      snprintf(o, cap, "%u ft", ft < 100 ? (ft + 5) / 10 * 10 : (ft + 25) / 50 * 50);
    else {
      u32 t = (m * 10 + 804) / 1609; /* tenths of a mile, rounded */
      snprintf(o, cap, "%u.%u mi", t / 10, t % 10);
    }
  } else if (m < 1000)
    snprintf(o, cap, "%u m", m < 100 ? (m + 5) / 10 * 10 : (m + 25) / 50 * 50);
  else {
    u32 t = (m + 50) / 100;
    snprintf(o, cap, "%u.%u km", t / 10, t % 10);
  }
}
/* The maneuver to show and its distance (issue #9, F25 X3: "the first direction after a reroute feels missed"). After a reroute the
   iPhone's current list (0x5201 field 13) is [0, 1]: the start marker (type 11, 0 m) and the first real turn; it moves on to [1, 2]
   only later, so the HUD showed the start marker, sometimes twice, instead of the turn. When the current maneuver is the start
   marker, the car is at it (30 m or less) and the iPhone already lists, and has sent, the next one, that one is shown: with its step
   length (0x5202 field 5, from the start marker to it) as the distance, in the iPhone's units, or with the iPhone's own distance when
   the step length is unknown. Test switch keep_start_marker: the start marker as before. Called under st_lock (or from on_rg). */
static u32 cur_view(u32 *dist, char *ds, u32 cap) {
  const struct rgs_man *C = man_get(st.cur_idx), *N = man_get(st.next_idx);
  *dist = st.dist;
  snprintf(ds, cap, "%s", st.dist_s);
  if (!st.has_next || st.next_idx == st.cur_idx || !C->used || C->type != 11 || st.dist > 30 || !N->used || N->type == 11 ||
      keep_start_marker())
    return st.cur_idx;
  u32 m = N->dist;
  if (m) {
    *dist = m;
    fmt_dist(ds, cap, m, st.dist_units);
  }
  static u32 logged = 0xffffffff;
  if (rg_debug_log() && logged != (st.cur_idx << 16 | st.next_idx)) {
    logged = st.cur_idx << 16 | st.next_idx;
    char b[120];
    snprintf(b, sizeof b, "RG start marker %u at %u m: showing maneuver %u (type %u, %u m)\n", st.cur_idx, st.dist, st.next_idx,
             N->type, *dist);
    log_text(b);
  }
  return st.next_idx;
}
static void publish(void) {
  const char *p = getenv("RGHOOK_PPS");
  char b[RG_PPS_SIZE];
  if (!p)
    p = RG_PPS;
  u32 cur = st.cur_idx, dist = st.dist;
  char ds[32];
  if (st.has_cur)
    cur = cur_view(&dist, ds, sizeof ds);
  else
    snprintf(ds, sizeof ds, "%s", st.dist_s);
  int k =
      snprintf(b, sizeof b,
               "@routeguidance\nroute_state:n:%u\nmaneuver_state:n:%u\ndist_to_maneuver:n:%u\ndist_to_maneuver_units:n:%u\n"
               "dist_to_maneuver_string::%s\ndistance_remaining:n:%u\ndistance_remaining_string::%s\ncurrent_road::%s\ndestination:"
               ":%s\nsource::%s\n",
               st.route_state, st.maneuver_state, dist, st.dist_units, ds, st.rem, st.rem_s, st.road, st.dest, st.src);
  if (st.has_cur) {
    const struct rgs_man *M = man_get(cur);
    /* maneuver_step_length: the maneuver's step length (0x5202 field 5, metres), the HUD effects' range (hud.h) */
    k += snprintf(b + k, sizeof b - k,
                  "maneuver_index:n:%u\nmaneuver_type:n:%u\nmaneuver_exit_angle:n:%d\nmaneuver_driving_side:n:%u\nmaneuver_road::%"
                  "s\nmaneuver_description::%s\nmaneuver_step_length:n:%u\n",
                  cur, M->type, M->exit_angle, M->side, M->road, M->desc, M->dist);
    /* JunctionType (0 intersection, 1 roundabout) and the angles of the junction's roads, for the side roads on the HUD (hudshapes.h) */
    if (M->angle_src) /* not the iPhone's own: where it came from (for the logs; hud.h does not read it) */
      k += snprintf(b + k, sizeof b - k, "maneuver_exit_angle_from::%s\n", M->angle_src == 1 ? "description" : "exit_number");
    k += snprintf(b + k, sizeof b - k, "maneuver_junction_type:n:%u\nmaneuver_junction_angles::", M->jtype);
    for (u32 q = 0; q < M->nang && k < (int)sizeof b - 16; q++)
      k += snprintf(b + k, sizeof b - k, q ? ",%d" : "%d", (int)M->ang[q]);
    k += snprintf(b + k, sizeof b - k, "\n");
  }
  /* The lanes the iPhone shows now, left to right: "status/highlight/angle,angle;..." (status 0 not good, 1 good, 2 preferred;
     highlight 1000 = none; angles as the exit angle, 0 straight on, positive to the right). Only while it shows them. */
  if (st.has_lane_idx) {
    k += snprintf(b + k, sizeof b - k, "lane_guidance_index:n:%u\nlane_guidance_showing:n:%u\n", st.lane_idx,
                  st.has_lane_show ? st.lane_show : 1);
    const struct rgs_lanes *S = lanes_get(st.lane_idx);
    if ((!st.has_lane_show || st.lane_show) && S && S->n) {
      k += snprintf(b + k, sizeof b - k, "lanes::");
      for (u32 q = 0; q < S->n && k < (int)sizeof b - 48; q++) {
        const __typeof__(st.lanes[0].l[0]) *L = &S->l[q];
        k += snprintf(b + k, sizeof b - k, "%s%u/%d/", q ? ";" : "", L->status, (int)L->hl);
        for (u32 a = 0; a < L->na; a++)
          k += snprintf(b + k, sizeof b - k, a ? ",%d" : "%d", (int)L->ang[a]);
      }
      k += snprintf(b + k, sizeof b - k, "\n");
    }
  }
  /* Only when it changed (1.3.1): iOS sends the whole route's maneuvers and lane sets at its start, and every message used to rewrite
     the file from the reader thread, which then reads the link again only when the file is written. */
  static char last[sizeof b];
  static int last_k = -1;
  if (k > (int)sizeof b - 1)
    k = (int)sizeof b - 1;
  if (k == last_k) {
    int q = 0;
    while (q < k && last[q] == b[q])
      q++;
    if (q == k)
      return;
  }
  /* One fixed record, written over the previous one (1.4.4, the file in RAM): QNX's /dev/shmem has no rename(), and O_TRUNC leaves a
     moment in which a reader finds the file empty (NBT-AAD found both on the head unit: main.cpp writeUiState).
     So the whole record goes in one write() at offset 0, padded with NULs to RG_PPS_SIZE: hud.h reads up to the first NUL, and a reader
     sees the old record or the new one, never a cut or empty one. */
  static char rec[RG_PPS_SIZE];
  memcpy(rec, b, (u32)k);
  memset(rec + k, 0, sizeof rec - (u32)k);
  int fd = r_open(p, 0x101 /* O_WRONLY | O_CREAT, no O_TRUNC */, 0644);
  if (fd < 0)
    return;
  r_write(fd, rec, sizeof rec);
  r_close(fd);
  memcpy(last, b, (u32)k);
  last_k = k;
}
/* LaneGuidanceInformation (0x5204): 1 LaneGuidanceIndex, 2 LaneInformation (one per lane, repeated), 3 description. Inside a
   LaneInformation: 0 position (u16, a new lane), 1 status (u8), 2 angles (s16 each, the TLV repeated or a list), 3 highlight angle (s16,
   1000 = none). Layout: libesoiap2's parseLaneInformation, as read by the community MIB2 implementation (rgd_tlv.c). Kept sorted by
   position, at most 8 lanes and 4 angles each. */
static void on_lanes(u32 idx, const u8 *p, u32 n) {
  u32 i = 0, cur = 0xffffffff;
  char b[200];
  int k, slot = key_slot(st.lanes, RG_LANESETS, sizeof st.lanes[0], idx, st.has_lane_idx, st.lane_idx, 0, 0);
  static struct rgs_lanes spare; /* one not kept: decoded for the log only */
  struct rgs_lanes *S = slot >= 0 ? &st.lanes[slot] : &spare;
  memset(S, 0, sizeof *S);
  S->used = slot >= 0;
  S->idx = idx;
  while (i + 4 <= n) {
    u32 ln = be16(p + i), id = be16(p + i + 2);
    if (ln < 4 || i + ln > n)
      break;
    if (id == 2) {
      const u8 *g = p + i + 4;
      u32 gn = ln - 4, j = 0;
      cur = 0xffffffff;
      while (j + 4 <= gn) {
        u32 sl = be16(g + j), sid = be16(g + j + 2);
        if (sl < 4 || j + sl > gn)
          break;
        const u8 *d = g + j + 4;
        u32 dn = sl - 4;
        if (sid == 0 || cur == 0xffffffff) {
          if (S->n >= 8)
            break;
          cur = S->n++;
          S->l[cur].hl = 1000;
          S->l[cur].pos = sid == 0 && dn ? uint_be(d, dn) : cur;
        }
        if (sid == 1 && dn)
          S->l[cur].status = d[0];
        else if (sid == 2)
          for (u32 q = 0; q + 2 <= dn && S->l[cur].na < 4; q += 2)
            S->l[cur].ang[S->l[cur].na++] = (s16)be16(d + q);
        else if (sid == 3 && dn >= 2)
          S->l[cur].hl = (s16)be16(d);
        j += sl;
      }
    }
    i += ln;
  }
  for (u32 a = 1; a < S->n; a++)
    for (u32 c = a; c && S->l[c - 1].pos > S->l[c].pos; c--) { /* left to right */
      __typeof__(st.lanes[0].l[0]) t = S->l[c];
      S->l[c] = S->l[c - 1];
      S->l[c - 1] = t;
    }
  k = snprintf(b, sizeof b, slot >= 0 ? "lanes %u:" : "lanes %u (not kept):", idx);
  for (u32 q = 0; q < S->n && k < 160; q++) {
    k += snprintf(b + k, sizeof b - k, " %u/%d/", S->l[q].status, (int)S->l[q].hl);
    for (u32 a = 0; a < S->l[q].na; a++)
      k += snprintf(b + k, sizeof b - k, a ? ",%d" : "%d", (int)S->l[q].ang[a]);
  }
  snprintf(b + k, sizeof b - k, "\n");
  if (rg_debug_log())
    log_text(b);
}
/* The exit number in a maneuver description ("Take exit 23B onto I-95": "23B"): the word "exit" followed by a token that starts
   with a digit. "" when there is none ("Take the exit", "exit onto"). */
static void exit_no(const char *d, char *o, u32 cap) {
  o[0] = 0;
  for (u32 i = 0; d[i]; i++) {
    if ((d[i] | 32) != 'e' || (d[i + 1] | 32) != 'x' || (d[i + 2] | 32) != 'i' || (d[i + 3] | 32) != 't')
      continue;
    if (i && (((d[i - 1] | 32) >= 'a' && (d[i - 1] | 32) <= 'z') || (d[i - 1] >= '0' && d[i - 1] <= '9')))
      continue;
    u32 j = i + 4;
    while (d[j] == ' ' || d[j] == '#')
      j++;
    if (d[j] < '0' || d[j] > '9')
      continue;
    u32 k = 0;
    while (((d[j] >= '0' && d[j] <= '9') || ((d[j] | 32) >= 'a' && (d[j] | 32) <= 'z')) && k + 1 < cap)
      o[k++] = d[j++];
    o[k] = 0;
    return;
  }
}
/* ExitInfo text -> the exit number only ("Exit 23B" -> "23B", "23B" -> "23B"); "" when it has no digit */
static void exit_from(char *o, u32 cap, const char *t) {
  exit_no(t, o, cap);
  if (o[0])
    return;
  while (*t == ' ' || *t == '#')
    t++;
  u32 k = 0;
  while (((*t >= '0' && *t <= '9') || ((*t | 32) >= 'a' && (*t | 32) <= 'z')) && k + 1 < cap)
    o[k++] = *t++;
  o[k] = 0;
  if (o[0] < '0' || o[0] > '9')
    o[0] = 0;
}
/* ---- an exit angle the iPhone did not send (1.4.4, ghackt issue #4, 2026-11-21: KakaoNavi on a 2211 F30, HUD-only 1.4.1) ----------------
   The roundabout picture turns by JunctionElementExitAngle (0x5202 field 11). An app that sends none (or 0) got a roundabout drawn
   straight through while the phone showed the 9 or 2 o'clock exit. Only then, and only for the pictures that are drawn by their angle
   (roundabouts: types 6, 7, 28..46 or junction type 1; and 0, 3, 5, drawn by the angle too), the angle is taken from:
     1  the description's clock direction, as Korean navigation apps write it ("9시 방향": 9 o'clock = -90, 2 = +60, 12 = 0) or
        "<n> o'clock"; for a non-roundabout type only with 방향 / 출구 ("direction" / "exit") after it;
     2  roundabout exit n (types 28..46) on the junction's roads (field 10): the n-th road met going round, anticlockwise in right-hand
        traffic (the highest angle first, the entry behind left out), clockwise in left-hand traffic.
   Else it stays 0, as before. */
static int rb_type(u32 t) {
  return t == 6 || t == 7 || (t >= 28 && t <= 46);
}
static int u8at(const char *s, u32 i, u8 a, u8 b, u8 c) {
  return (u8)s[i] == a && (u8)s[i + 1] == b && (u8)s[i + 2] == c;
}
/* 1..12 just before position e (digits, one blank allowed), 0 = none */
static u32 clock_number(const char *s, u32 e) {
  if (e && s[e - 1] == ' ')
    e--;
  u32 v = 0, m = 1, nd = 0;
  while (e && nd < 2 && s[e - 1] >= '0' && s[e - 1] <= '9') {
    v += (u32)(s[e - 1] - '0') * m;
    m = m * 10;
    e--;
    nd++;
  }
  if (!nd || (e && s[e - 1] >= '0' && s[e - 1] <= '9'))
    return 0;
  return v >= 1 && v <= 12 ? v : 0;
}
static int clock_angle(const char *s, int roundabout, int *out) {
  u32 n = 0;
  while (s[n])
    n++;
  for (u32 i = 0; i + 3 <= n; i++) {
    u32 v = 0;
    if (u8at(s, i, 0xEC, 0x8B, 0x9C)) { /* 시 */
      u32 j = i + 3;
      if (j < n && s[j] == ' ')
        j++;
      int dir = j + 6 <= n && ((u8at(s, j, 0xEB, 0xB0, 0xA9) && u8at(s, j + 3, 0xED, 0x96, 0xA5)) || /* 방향 */
                               (u8at(s, j, 0xEC, 0xB6, 0x9C) && u8at(s, j + 3, 0xEA, 0xB5, 0xAC)));  /* 출구 */
      if (roundabout || dir)
        v = clock_number(s, i);
    } else if (i + 7 <= n && s[i] == 'o' && s[i + 1] == '\'' && s[i + 2] == 'c' && s[i + 3] == 'l' && s[i + 4] == 'o' &&
               s[i + 5] == 'c' && s[i + 6] == 'k')
      v = clock_number(s, i);
    else if (i + 9 <= n && s[i] == 'o' && u8at(s, i + 1, 0xE2, 0x80, 0x99) && s[i + 4] == 'c' && s[i + 5] == 'l' &&
             s[i + 6] == 'o' && s[i + 7] == 'c' && s[i + 8] == 'k') /* o’clock */
      v = clock_number(s, i);
    if (v) {
      int a = (int)v * 30;
      *out = a > 180 ? a - 360 : a == 360 ? 0 : a;
      return 1;
    }
  }
  return 0;
}
static int nth_exit(const struct rgs_man *M, u32 nth, int *out) {
  int r[8];
  u32 k = 0;
  for (u32 i = 0; i < M->nang && k < 8; i++)
    if (M->ang[i] > -160 && M->ang[i] < 160) /* not the entry, behind */
      r[k++] = M->ang[i];
  if (!nth || nth > k)
    return 0;
  for (u32 i = 1; i < k; i++) /* the order met going round: right first (anticlockwise), left first in left-hand traffic */
    for (u32 j = i; j > 0 && (M->side == 1 ? r[j] < r[j - 1] : r[j] > r[j - 1]); j--) {
      int t = r[j];
      r[j] = r[j - 1];
      r[j - 1] = t;
    }
  *out = r[nth - 1];
  return 1;
}
static void exit_fallback(struct rgs_man *M) {
  if (M->exit_angle)
    return;
  int rb = rb_type(M->type) || M->jtype == 1, a = 0;
  if (!rb && M->type != 0 && M->type != 3 && M->type != 5)
    return;
  if (clock_angle(M->desc, rb, &a)) {
    M->exit_angle = a;
    M->angle_src = 1;
  } else if (M->type >= 28 && M->type <= 46 && nth_exit(M, M->type - 27, &a)) {
    M->exit_angle = a;
    M->angle_src = 2;
  }
}

static void on_rg_locked(u32 mid, const u8 *p, u32 n);
static void kick(void) {
#ifdef RGHOOK_KOMBI
  rgk_kick();
#endif
#ifdef RGHOOK_MOST
  rgh_kick();
#endif
}
static void on_rg(u32 mid, const u8 *p, u32 n) {
  st_lock();
  on_rg_locked(mid, p, n);
  st_unlock();
  kick();
}
static void on_rg_locked(u32 mid, const u8 *p, u32 n) {
  char b[600];
  int k = snprintf(b, sizeof b, "RG 0x%04X", mid);
  u32 idx = 0xffffffff;
  u32 i = 0;
  while (i + 4 <= n) {
    u32 ln = be16(p + i), id = be16(p + i + 2);
    if (ln < 4 || i + ln > n)
      break;
    const u8 *d = p + i + 4;
    u32 dn = ln - 4;
    int str = dn > 1 && d[dn - 1] == 0;
    for (u32 q = 0; str && q + 1 < dn; q++)
      if (d[q] < 0x20 && d[q] != '\n')
        str = 0;
    if (k < 520) {
      if (str)
        k += snprintf(b + k, sizeof b - k, " %u=\"%s\"", id, (const char *)d);
      else {
        k += snprintf(b + k, sizeof b - k, " %u=", id);
        for (u32 q = 0; q < dn && q < 12 && k < 560; q++)
          k += snprintf(b + k, sizeof b - k, "%02x", d[q]);
      }
    }
    if (mid == 0x5201) {
      switch (id) {
        case 1:
          st.route_state = uint_be(d, dn);
          break;
        case 2:
          st.maneuver_state = uint_be(d, dn);
          break;
        case 3:
          cpstr(st.road, sizeof st.road, d, dn);
          break;
        case 4:
          cpstr(st.dest, sizeof st.dest, d, dn);
          break;
        case 5:
          st.eta = uint_be(d, dn);
          break;
        case 7:
          st.rem = uint_be(d, dn);
          break;
        case 8:
          cpstr(st.rem_s, sizeof st.rem_s, d, dn);
          break;
        case 9:
          st.rem_units = uint_be(d, dn);
          break;
        case 10:
          st.dist = uint_be(d, dn);
          break;
        case 11:
          cpstr(st.dist_s, sizeof st.dist_s, d, dn);
          break;
        case 12:
          st.dist_units = uint_be(d, dn);
          break;
        case 13: /* ManeuverCurrentList: the maneuvers the iPhone shows now, u16 each; the first is the current one */
          if (dn >= 2) {
            st.cur_idx = be16(d);
            st.has_cur = 1;
            st.has_next = dn >= 4;
            st.next_idx = dn >= 4 ? be16(d + 2) : 0;
          }
          break;
        case 19:
          cpstr(st.src, sizeof st.src, d, dn);
          break;
        case 16:
          if (dn >= 1) {
            st.lane_idx = uint_be(d, dn);
            st.has_lane_idx = 1;
          }
          break;
        case 18:
          if (dn >= 1) {
            st.lane_show = uint_be(d, dn) != 0;
            st.has_lane_show = 1;
          }
          break;
      }
    }
    if (mid == 0x5202 || mid == 0x5204) {
      if (id == 1)
        idx = uint_be(d, dn);
    }
    i += ln;
  }
  if (mid == 0x5204 && idx != 0xffffffff)
    on_lanes(idx, p, n);
  if (mid == 0x5202 && idx != 0xffffffff) {
    int slot = key_slot(st.man, RG_MANS, sizeof st.man[0], idx, st.has_cur, st.cur_idx, st.has_next, st.next_idx);
    static struct rgs_man spare; /* one not kept */
    struct rgs_man *M = slot >= 0 ? &st.man[slot] : &spare;
    memset(M, 0, sizeof *M);
    M->used = slot >= 0;
    M->idx = idx;
    if (slot < 0 && rg_debug_log()) {
      char nb[64];
      snprintf(nb, sizeof nb, "RG maneuver %u not kept (store full)\n", idx);
      log_text(nb);
    }
    i = 0;
    while (i + 4 <= n) {
      u32 ln = be16(p + i), id = be16(p + i + 2);
      if (ln < 4 || i + ln > n)
        break;
      const u8 *d = p + i + 4;
      u32 dn = ln - 4;
      if (id == 2)
        cpstr(M->desc, sizeof M->desc, d, dn);
      else if (id == 3)
        M->type = uint_be(d, dn);
      else if (id == 4)
        cpstr(M->road, sizeof M->road, d, dn);
      else if (id == 5)
        M->dist = uint_be(d, dn);
      else if (id == 11)
        M->exit_angle = (s16)uint_be(d, dn);
      else if (id == 9)
        M->jtype = uint_be(d, dn);
      else if (id == 10)
        for (u32 q = 0; q + 2 <= dn && M->nang < 8; q += 2)
          M->ang[M->nang++] = (s16)be16(d + q); /* JunctionElementAngle, one per road */
      else if (id == 8)
        M->side = uint_be(d, dn); /* DrivingSide: 0 right-hand traffic, 1 left-hand */
      else if (id != 1 && id != 6 && id != 7 && dn >= 2 && dn <= 18 && M->exit[0] == 0) {
        /* any other text field with a digit in it is taken as the ExitInfo the iPhone sends when SupportsExitInfo was asked for
           ("23B", "Exit 23B"): printable, ends in NUL (binary and number fields do not) */
        u32 q = 0, dig = 0;
        while (q + 1 < dn && d[q] >= 0x20 && d[q] < 0x7f) {
          if (d[q] >= '0' && d[q] <= '9')
            dig = 1;
          q++;
        }
        if (q + 1 == dn && d[q] == 0 && dig)
          exit_from(M->exit, sizeof M->exit, (const char *)d);
      }
      i += ln;
    }
    exit_fallback(M);
    if (M->angle_src && k < 560)
      k += snprintf(b + k, sizeof b - k, " (exit angle %d from the %s)", (int)M->exit_angle,
                    M->angle_src == 1 ? "description" : "exit number");
  }
  snprintf(b + k, sizeof b - k, "\n");
  if (rg_debug_log())
    log_text(b);
  static u32 logged_state = 0xffffffff; /* without debug_log: only the route's state changes (started, ended) */
  if (mid == 0x5201 && st.route_state != logged_state) {
    logged_state = st.route_state;
    if (!rg_debug_log()) {
      snprintf(b, sizeof b, "RG route_state %u\n", st.route_state);
      log_text(b);
    }
  }
  publish();
}

/* ---------------- link layer ---------------- */
static void fix_packet(u8 *pk, u32 n) {
  wbe16(pk + 2, n);
  pk[8] = checksum(pk, 8);
  if (n > 9)
    pk[n - 1] = checksum(pk + 9, n - 10);
}
static void inject_start(void) {
  u8 m[64], pk[80];
  u32 k = 6;
  k += tlv16(m + k, 0, RG_COMP_ID);
  k += tlv(m + k, 1, 0, 0);
  k += tlv(m + k, 2, 0, 0);
  k += tlv(m + k, 3, 0, 0); /* component, request SourceName, SourceSupportsRouteGuidance, SupportsExitInfo */
  m[0] = 0x40;
  m[1] = 0x40;
  wbe16(m + 2, k);
  wbe16(m + 4, 0x5200);
  pk[0] = 0xff;
  pk[1] = 0x5a;
  pk[4] = ACK;
  pk[5] = (u8)(last_out_seq + 1);
  pk[6] = (u8)last_in_seq;
  pk[7] = (u8)ctl_sess;
  memcpy(pk + 9, m, k);
  fix_packet(pk, 9 + k + 1);
  r_write(transport_fd, pk, 9 + k + 1);
  last_out_seq = (last_out_seq + 1) & 0xff;
  delta = (delta + 1) & 0xff;
  injected = 1;
  char b[96];
  snprintf(b, sizeof b, "inject StartRouteGuidanceUpdates seq=%u (delta now %u)\n", last_out_seq, delta);
  log_text(b);
}

static void log_mid(const char *dir, u32 mid, u32 ml);
static int ident_ok, auth_ok; /* see packet_done */
static u8 out_buf[70000];
/* Packets the driver writes in pieces (1.3.1). Until then a packet whose header and rest came in different write() calls went out
   with the driver's own sequence number once StartRouteGuidanceUpdates had been injected: the phone took it for the previous packet
   again and dropped it, and the driver resent it the same way.
     out_left   the rest of a packet whose header an earlier write() carried (renumbered there): copied as it is
     out_hold   a header cut by the end of a write() (fewer than 9 bytes): kept and sent with the next one, which carries its rest */
static u32 out_left, out_hold_len;
static u8 out_hold[16], out_in[70000 + 16];
long write(int fd, const void *pv, size_t n) {
  init();
  const u8 *p = (const u8 *)pv;
  if (transport_fd < 0 && getenv("RGHOOK_DEV") == 0 && n >= 2 && p[0] == 0xff && (p[1] == 0x5a || p[1] == 0x55)) {
    transport_fd = fd;
    char b[64];
    snprintf(b, sizeof b, "transport fd %d\n", fd);
    log_text(b);
    restart_watch_once();
  }
  if (fd != transport_fd || n == 0)
    return r_write(fd, pv, n);
  if (n > sizeof out_buf - 2048) { /* not seen: as it is (after a header kept from the last write) */
    lock();
    if (out_hold_len)
      r_write(fd, out_hold, out_hold_len);
    out_left = out_hold_len = 0;
    unlock();
    return r_write(fd, pv, n);
  }
  lock();
  u32 left0 = out_left, hold0 = out_hold_len, m = (u32)n;
  u8 hold_was[sizeof out_hold];
  memcpy(hold_was, out_hold, hold0);
  if (hold0) { /* the header kept from the last write() first */
    memcpy(out_in, out_hold, hold0);
    memcpy(out_in + hold0, p, n);
    p = out_in;
    m += hold0;
    out_hold_len = 0;
  }
  u32 i = 0, o = 0;
  if (out_left) {
    i = o = m < out_left ? m : out_left;
    memcpy(out_buf, p, i);
    out_left -= i;
  }
  while (i < m) {
    if (p[i] == 0xff && i + 9 > m && (i + 1 == m || p[i + 1] == 0x5a)) { /* a header cut by the end of this write */
      out_hold_len = m - i;
      memcpy(out_hold, p + i, out_hold_len);
      break;
    }
    if (i + 9 <= m && p[i] == 0xff && p[i + 1] == 0x5a && checksum(p + i, 8) == p[i + 8]) {
      u32 ln = be16(p + i + 2);
      if (ln < 9) {
        memcpy(out_buf + o, p + i, m - i);
        o += m - i;
        break;
      }
      if (i + ln > m) { /* continued in the next write(): the header is renumbered now, the rest goes as it is */
        u8 *pk = out_buf + o;
        memcpy(pk, p + i, m - i);
        if (pk[4] & SYN) {
          delta = 0;
          injected = 0;
          ident_ok = auth_ok = 0;
        } else {
          pk[5] = (u8)(pk[5] + delta);
          if (ln > 9)
            last_out_seq = pk[5];
          pk[8] = checksum(pk, 8);
        }
        if (ln > 9 + 6 && (int)pk[7] == ctl_sess && m - i >= 15 && pk[9] == 0x40 && pk[10] == 0x40 && be16(pk + 13) == 0x1D01)
          log_text("IdentificationInformation written in pieces: not patched (no route guidance this session)\n");
        out_left = ln - (m - i);
        o += m - i;
        break;
      }
      u8 *pk = out_buf + o;
      memcpy(pk, p + i, ln);
      u32 nl = ln;
      if (pk[4] & SYN) {
        delta = 0;
        injected = 0;
        ctl_sess = -1;
        ident_ok = auth_ok = 0;
        for (u32 q = 9 + 10; q + 3 <= ln - 1; q += 3)
          if (pk[q + 1] == 0 && ctl_sess < 0)
            ctl_sess = pk[q];
        char b[64];
        snprintf(b, sizeof b, "link SYN, control session %d\n", ctl_sess);
        log_text(b);
      } else {
        if (ln > 9 + 6 && (int)pk[7] == ctl_sess && pk[9] == 0x40 && pk[10] == 0x40 && be16(pk + 13) == 0x1D01) {
          static u8 mb[8192];
          u32 ml = be16(pk + 11);
          if (ml > ln - 10)
            ml = ln - 10;
          u32 nm = patch_ident(pk + 9, ml, mb);
          memcpy(pk + 9, mb, nm);
          nl = 9 + nm + 1;
          char b[96];
          snprintf(b, sizeof b, "IdentificationInformation patched %u -> %u bytes\n", ml, nm);
          log_text(b);
        }
        if (ln > 9 + 6 && (int)pk[7] == ctl_sess && pk[9] == 0x40 && pk[10] == 0x40)
          log_mid("out", be16(pk + 13), be16(pk + 11));
        pk[5] = (u8)(pk[5] + delta);
        if (nl > 9)
          last_out_seq = pk[5];
        fix_packet(pk, nl);
      }
      o += nl;
      i += ln;
    } else {
      out_buf[o++] = p[i++];
    }
  }
  if (!o) { /* all of it kept (a header's first bytes): it goes with the next write */
    unlock();
    return (long)n;
  }
  long r = r_write(fd, out_buf, o);
  if (r <= 0) { /* nothing went out: the driver writes the same bytes again */
    out_left = left0;
    out_hold_len = hold0;
    memcpy(out_hold, hold_was, hold0);
  } else if ((u32)r < o) {
    /* A short write (the transport's buffer full, a non-blocking descriptor): the renumbered bytes are only here, so they are
       finished here (up to ~1 s); handing the count back would have the driver resend its own, unrenumbered bytes. Until 1.3.1 the
       rest was dropped and the phone got a cut packet. */
    u32 w = (u32)r;
    for (int tries = 0; w < o && tries < 500; tries++) {
      long k = r_write(fd, out_buf + w, o - w);
      if (k > 0)
        w += (u32)k;
      else
        usleep(2000);
    }
    if (w < o)
      log_text("write: transport stalled, part of a packet not sent\n");
  }
  unlock();
  return r <= 0 ? r : (long)n;
}

/* Incoming bytes go to the driver in the same read() (its BT transport waits for the device's "data available" notification:
   bytes kept here would only be delivered with the next packet). Packet headers are rewritten in place. Only the start of a packet
   that cannot be decided yet is held back (held_buf; the rest of that packet is on its way, so the descriptor becomes readable
   again): a header split across reads (2 or 9 bytes), a control packet's first 15 bytes (its message id), a route guidance packet
   until it is whole (replaced as a whole, unit_need). Held bytes always go out first: when they do not fit the driver's buffer with
   anything after them, read() completes them in held_buf (1.3.1; before, new bytes overtook them and the driver got the stream out
   of order). Payload bytes are copied aside (pkt_buf) to decode the control message once the packet is complete. */
static u8 held_buf[16384], stage_buf[16384], pkt_buf[70000], hdr[9];
static u32 held_len, pkt_left, pkt_buf_len, pkt_len;
static int pkt_ctl;
/* decided bytes that did not fit a driver buffer of fewer than 16 bytes (the substitute is 16): the next read() returns them */
static u8 ready_buf[64];
static u32 ready_len;
/* The 2017 driver leaks the receive buffer of every message it does not know ("Invalid packet"): after ~20 route guidance
   messages it stops reading the link. Route guidance packets are therefore decoded here and handed to the driver as a minimal
   message it subscribed to and handles normally (default: an empty PowerUpdate 0xAE01; $RGHOOK_SUBST, hex, changes it).
   Same sequence number, so the driver acknowledges it and the link numbering is untouched. */
static u32 subst_id(void) {
  static u32 v;
  if (!v) {
    const char *s = getenv("RGHOOK_SUBST");
    v = 0xAE01;
    if (s) {
      u32 x = 0;
      for (; *s; s++) {
        int c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
          break;
        x = x << 4 | (u32)d;
      }
      if (x)
        v = x;
    }
  }
  return v;
}
static int is_rg(u32 mid) {
  return mid == 0x5201 || mid == 0x5202 || mid == 0x5204;
}
static u32 nlogged; /* with debug_log, control message ids in both directions go to the log, the first 400 (debugging a phone) */
static void log_mid(const char *dir, u32 mid, u32 ml) {
  if (nlogged >= 400 || !rg_debug_log())
    return;
  nlogged++;
  char b[48];
  snprintf(b, sizeof b, "%s %04x len %u\n", dir, mid, ml);
  log_text(b);
}
/* StartRouteGuidanceUpdates waits for both IdentificationAccepted (0x1D02) and AuthenticationSucceeded (0xAA05): a real iPhone (2026-11-11,
   MINI F54, wireless) identifies first and authenticates afterwards, and ignored the request sent between the two (no reply at all). */
static void packet_done(void) {
  int accepted = 0;
  if (pkt_ctl && pkt_buf_len >= 7 && pkt_buf[0] == 0x40 && pkt_buf[1] == 0x40) {
    u32 mid = be16(pkt_buf + 4), ml = be16(pkt_buf + 2);
    if (ml > pkt_buf_len - 1)
      ml = pkt_buf_len - 1;
    log_mid("in ", mid, ml);
    if (mid == 0x1D02) {
      ident_ok = 1;
      log_text("IdentificationAccepted\n");
    } else if (mid == 0xAA05) {
      auth_ok = 1;
      log_text("AuthenticationSucceeded\n");
    } else if (mid == 0xAA06)
      log_text("AuthenticationFailed\n");
    else if (mid == 0x1D03)
      log_text("IdentificationRejected\n");
    else if ((mid == 0x5201 || mid == 0x5202 || mid == 0x5204) && ml >= 6)
      on_rg(mid, pkt_buf + 6, ml - 6);
  }
  accepted = ident_ok && auth_ok;
  if (accepted && !injected) {
    lock();
    if (!out_left && !out_hold_len) /* not between the pieces of a driver packet: tried again with the next message */
      inject_start();
    unlock();
#ifdef RGHOOK_MOST
    /* The MOST client starts with the CarPlay session, not with the first route message: its loop also takes the HUD test channel
       (hud-probe, tools/hud_probe.sh) and "HUD debug log", which in a HUD-only install had no client while no route guided. */
    rgh_kick();
#endif
  }
}
/* How many bytes of the packet starting at p (p[0] == 0xff, n bytes there) scan_in needs before it can hand it on: 2 (is it ff 5a?),
   the 9-byte header, 15 for a control packet's message id, all of a route guidance packet (replaced as a whole); 0 = the n bytes
   decide it. A route guidance packet longer than held_buf cannot be held: it goes on as it is (scan_in). */
static u32 unit_need(const u8 *p, u32 n) {
  if (n < 2)
    return 2;
  if (p[1] != 0x5a)
    return 0;
  if (n < 9)
    return 9;
  u32 ln = be16(p + 2);
  if (checksum(p, 8) != p[8] || ln < 9 + 6 + 1 || (int)p[7] != ctl_sess || (p[4] & SYN))
    return 0;
  if (n < 15)
    return 15;
  if (p[9] != 0x40 || p[10] != 0x40 || !is_rg(be16(p + 13)))
    return 0;
  return n < ln ? ln : 0;
}
static u32 scan_in(u8 *b,
                   u32 n) { /* returns how many bytes to hand to the driver (the rest is an incomplete packet start kept in held_buf) */
  u32 i = 0;
  while (i < n) {
    if (pkt_left) {
      u32 m = n - i < pkt_left ? n - i : pkt_left;
      if (pkt_buf_len + m <= sizeof pkt_buf) {
        memcpy(pkt_buf + pkt_buf_len, b + i, m);
        pkt_buf_len += m;
      }
      pkt_left -= m;
      i += m;
      if (!pkt_left)
        packet_done();
      continue;
    }
    if (b[i] != 0xff) {
      i++;
      continue;
    }
    u32 need = unit_need(b + i, n - i);
    if (need && need <= sizeof held_buf) { /* the rest is on its way: keep this start until it can be decided */
      held_len = n - i;
      memcpy(held_buf, b + i, held_len);
      return i;
    }
    if (b[i + 1] != 0x5a || checksum(b + i, 8) != b[i + 8] || be16(b + i + 2) < 9) {
      i++;
      continue;
    }
    u8 *pk = b + i;
    u32 ln = be16(pk + 2);
    int ctl = (int)pk[7] == ctl_sess && !(pk[4] & SYN) && ln > 9;
    if (ctl && ln >= 9 + 6 + 1 && i + ln <= n && pk[9] == 0x40 && pk[10] == 0x40 && is_rg(be16(pk + 13))) {
      /* a whole route guidance packet (one longer than held_buf that arrived in pieces goes on as it is, below) */
      u32 mid = be16(pk + 13), ml = be16(pk + 11);
      if (ml > ln - 10)
        ml = ln - 10;
      if (ml >= 6)
        on_rg(mid, pk + 15, ml - 6);
      if (delta)
        pk[6] = (u8)(pk[6] - delta);
      last_in_seq = pk[5];
      wbe16(pk + 9 + 2, 6);
      wbe16(pk + 9 + 4, subst_id());
      fix_packet(pk, 16); /* -> 40 40 00 06 <subst>, 16-byte packet */
      memmove(pk + 16, pk + ln, n - (i + ln));
      n -= ln - 16;
      i += 16;
      continue;
    }
    if (!(pk[4] & SYN)) {
      if (delta) {
        pk[6] = (u8)(pk[6] - delta);
        pk[8] = checksum(pk, 8);
      } /* acknowledgement of our renumbered packets -> driver's numbering */
      if (ln > 9)
        last_in_seq = pk[5];
    }
    pkt_ctl = ctl;
    pkt_buf_len = 0;
    pkt_left = ln > 9 ? ln - 9 : 0;
    memcpy(hdr, pk, 9);
    i += 9;
  }
  return n;
}
/* The route guidance state belongs to one iPhone session: a new mm-ipod (Connectivity restarts it for every session) starts without
   the previous file, so the HUD (src/hud.h) never keeps an old route; within a session the iPhone ends a route itself
   (RouteGuidanceUpdate state 2 Arrived / 0 NoRouteSet). close() is not hooked: mm-ipod calls it while the libraries are still loading,
   before libc can be looked up (it crashed). */
static void rg_clear(void) {
  const char *p = getenv("RGHOOK_PPS");
  st_lock();
  memset(&st, 0, sizeof st);
  st_unlock();
  r_unlink(p ? p : RG_PPS);
  if (!p)
    r_unlink(RG_PPS_OLD); /* an older librghook's, on the disk */
}

/* ---------------- for the KOMBI sender (rgview.h) ---------------- */
static void copy_z(char *d, u32 cap, const char *s) {
  u32 k = 0;
  while (s[k] && k + 1 < cap) {
    d[k] = s[k];
    k++;
  }
  d[k] = 0;
}
int rg_view_get(struct rg_view *v) {
  init();
  memset(v, 0, sizeof *v);
  st_lock();
  int have = st.has_cur;
  u32 rs = st.route_state;
  v->rs = rs;
  v->hold = rs == 3 || rs == 4;
  if (have) {
    u32 dist, cur = cur_view(&dist, v->dist_s, sizeof v->dist_s); /* the start marker skipped as in publish() */
    const struct rgs_man *M = man_get(cur);
    v->active = rs == 1 || rs == 5 || rs == 6;
    v->index = cur;
    v->known = M->used != 0;
    v->type = M->type;
    v->side = M->side;
    v->angle = M->exit_angle;
    if (M->exit[0])
      copy_z(v->exitno, sizeof v->exitno, M->exit);
    else
      exit_no(M->desc, v->exitno, sizeof v->exitno);
    v->roadfb = M->road[0] ? 0 : 1;
    v->dist = dist;
    v->units = st.dist_units;
    copy_z(v->road, sizeof v->road, M->road[0] ? M->road : st.road);
    if (st.has_lane_idx) { /* the lanes the iPhone shows now, as publish() writes them */
      const struct rgs_lanes *S = lanes_get(st.lane_idx);
      v->lane_index = st.lane_idx;
      if ((!st.has_lane_show || st.lane_show) && S)
        for (u32 q = 0; q < S->n && q < 8; q++) {
          struct rg_lane *L = &v->lanes[v->nl++];
          L->status = S->l[q].status;
          L->hl = S->l[q].hl;
          L->na = S->l[q].na;
          for (u32 a = 0; a < L->na && a < 4; a++)
            L->ang[a] = S->l[q].ang[a];
        }
    }
  }
  st_unlock();
  return have;
}
int rg_file_read(const char *path, char *buf, unsigned cap) {
  init();
  int fd = r_open(path, 0);
  if (fd < 0)
    return -1;
  long n = r_read(fd, buf, cap ? cap - 1 : 0);
  r_close(fd);
  if (n < 0)
    n = 0;
  if (cap)
    buf[n] = 0;
  return (int)n;
}
int rg_file_write(const char *path, const char *text) {
  init();
  int fd = r_open(path, 0x301, 0644);
  if (fd < 0)
    return -1;
  r_write(fd, text, strlen(text));
  r_close(fd);
  return 0;
}
void rg_file_unlink(const char *path) {
  init();
  r_unlink(path);
}
long rg_fd_read(int fd, void *buf, unsigned n) {
  init();
  return r_read(fd, buf, n);
}
#if defined(RGHOOK_KOMBI) || defined(RGHOOK_MOST)
/* The watcher (ksh, P_NOWAITO so it outlives mm-ipod; mm-ipod lives for one CarPlay session): polls our pid every 3 s, then has
   libcpprobe.so clear what a marker still names us for. RGHOOK_NO_WATCH: none (tests). */
extern int spawnle(int, const char *, const char *, ...);
extern int getpid(void);
void rg_watch_once(void) {
  static volatile int done;
  if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
    return;
  if (getenv("RGHOOK_NO_WATCH"))
    return;
  static const char lib[] = "/var/opt/hmi/data/rw/Plugins/CarPlayMenu/libcpprobe.so";
  const char *mk = getenv("RGHOOK_MARKER");
  char sc[512];
  int pid = getpid();
  snprintf(sc, sizeof sc,
           "while kill -0 %d 2>/dev/null; do sleep 3; done; [ -f %s ] && CPM_CLEAR=%d CPM_KOMBI_MARKER=%s LD_PRELOAD=%s /bin/cat"
           " < /dev/null > /dev/null 2>&1",
           pid, lib, pid, mk && *mk ? mk : RG_KOMBI_MARKER, lib);
  static char *const envp[] = {(char *)"PATH=/bin:/usr/bin:/proc/boot", 0};
  char b[96];
  if (spawnle(3 /* P_NOWAITO */, "/bin/ksh", "ksh", "-c", sc, (char *)0, envp) == -1) {
    log_text("watcher not started: an arrow left when mm-ipod ends stays until the next session\n");
    return;
  }
  snprintf(b, sizeof b, "watcher started for pid %d (clears an arrow left when mm-ipod ends)\n", pid);
  log_text(b);
}
#endif
/* The calling thread's priority down to at most max (QNX getprio / setprio of pid 0 = the calling thread, looked up at run time so
   nothing new is linked). Returns the priority it had, -1 = unknown (not QNX). */
int rg_lower_priority(int max) {
  void *h = dlopen("libc.so.3", 2);
  int (*gp)(int) = h ? (int (*)(int))dlsym(h, "getprio") : 0;
  int (*sp)(int, int) = h ? (int (*)(int, int))dlsym(h, "setprio") : 0;
  if (!gp || !sp)
    return -1;
  int p = gp(0);
  if (p > max)
    sp(0, max);
  return p;
}
void rg_log_line(const char *s) {
  char b[300];
  snprintf(b, sizeof b, "%s\n", s);
  log_text(b);
}
long read(int fd, void *buf, size_t n) {
  init();
  if (fd != transport_fd || transport_fd < 0 || n == 0)
    return r_read(fd, buf, n);
  u8 *b = (u8 *)buf;
  if (ready_len) { /* decided bytes a tiny buffer could not take: before anything else */
    u32 m = ready_len < n ? ready_len : (u32)n;
    memcpy(b, ready_buf, m);
    memmove(ready_buf, ready_buf + m, ready_len - m);
    ready_len -= m;
    return (long)m;
  }
  for (;;) {
    u32 h = held_len;
    if (h >= n) {
      /* The held packet start does not fit the driver's buffer with anything after it (its reads are smaller than a route
         guidance packet, as on a wireless link): it is completed in held_buf, reading only the bytes it lacks, so nothing after it
         is taken from the descriptor and it still goes first. Until 1.3.1 the new bytes went out ahead of it. */
      u32 need = unit_need(held_buf, h);
      if (need > h && need <= sizeof held_buf) { /* (a longer one goes on as it is: scan_in holds none) */
        long k = r_read(fd, held_buf + h, need - h);
        if (k <= 0)
          return k; /* no new data (timeout/EAGAIN): the held bytes stay */
        held_len = h + (u32)k;
        continue;
      }
      memcpy(stage_buf, held_buf, h); /* decided: through scan_in from a copy (it may hold a new start in held_buf) */
      held_len = 0;
      u32 out = scan_in(stage_buf, h), m = out < n ? out : (u32)n;
      if (out - m > sizeof ready_buf) /* cannot happen: at most 16 bytes come out of a unit read up to its need */
        out = m + (u32)sizeof ready_buf;
      memcpy(b, stage_buf, m);
      memcpy(ready_buf, stage_buf + m, out - m);
      ready_len = out - m;
      if (m)
        return (long)m;
      continue;
    }
    if (h) {
      memcpy(b, held_buf, h);
      held_len = 0;
    }
    long k = r_read(fd, b + h, n - h);
    if (k <= 0) {
      held_len = h; /* still in held_buf */
      return k;
    } /* no new data (timeout/EAGAIN): say so, keep the held_buf bytes */
    u32 out = scan_in(b, h + (u32)k);
    if (out)
      return (long)out; /* nothing but a packet start so far: its rest is on the way, wait for it */
  }
}
