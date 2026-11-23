// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
//
// CarPlay directions on an Ethernet KOMBI (G-series, J29): the KOMBI's own route-guidance service BMW.IC.Guidance, SOME/IP 0xB006 over
// TCP 160.48.199.96:30502, draws the arrow, distance, street and lane row in the cluster and the windscreen HUD. The MOST HUDTerminal
// path (hud.h, in the HMI) is for F-series clusters. The KOMBI is driven from librghook-kombi.so in mm-ipod (rgkombi.cpp), which the
// installer puts in place of librghook.so when its probe (libcpprobe.so, probe.cpp) finds the KOMBI; it works without the menu plugin.
//
// A port of NBT-AAD's car-verified KOMBI stack (milestone-7/src/cluster: j29_graph.hpp, j29_client.cpp, j29_display.cpp; the same
// stack ftojke/BMW-iDrive6-CarPlay-HUD reuses on a G30) to this freestanding plugin: no STL, no floating point, no division, fixed
// buffers, socket calls looked up at run time. The graph is NBT-AAD's captured factory route start (kombitemplate.h, their
// j29_graph_template.hpp unchanged), replayed byte for byte with only the car-proven fields patched:
//   ManeuverDescription 0x0F  direction enum at body[33]
//   ManeuverDistance    0x1D  distance text (UTF-16BE at the FEFF BOM), the unit byte after it, the red bar at body[22] (255 = hidden)
//   TextResource        0x03  road name
//   lanes                     one lane model 0x1B (method 0x0010) before maneuver 0's 0x0F nodes, which then list it; the template's
//                             own lane models dropped; a 0x11 destination info with the car's route position keeps the row on
// Safety as in NBT-AAD: only whitelisted methods (never 0x0001 create / dispose: the factory owns the instance), our own client id,
// one reconnect per frame, a reply that is not a matching E_OK stops the output (rejected), the clear is the factory's 6-frame cancel
// on a fresh connection with the state-0 frame alone as a fallback.
// Not here (NBT-AAD watches sloginfo for it): stepping aside while the BMW navigation guides. Do not start a BMW route while CarPlay
// guides on such a car.
//
// Host tests: tests/test_kombi.py builds this with KOMBI_HOST (the Mac's own socket calls) against a fake KOMBI.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "kombitemplate.h"
#include "rgview.h"

#ifdef KOMBI_HOST
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#else
extern "C" {
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int usleep(unsigned);
int close(int);
int fcntl(int, int, ...);
void *memcpy(void *, const void *, __SIZE_TYPE__);
void *memset(void *, int, __SIZE_TYPE__);
}
#endif

namespace kombi {

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
namespace T = nbt::kombinav::tmpl;

void (*g_log)(const char *) = 0;
int (*g_debug)(void) = 0;  // switch debug_log (rg_debug_log): the per-maneuver lines only with it; none = off

// ---- KOMBI vocabulary (BMW.IC.Guidance; values from NBT-AAD: LEFT 6 / RIGHT 7 / DESTINATION 10 car-verified, the rest by name) -------
enum {
  kStraight = 1,
  kHalfLeft = 2,
  kHalfRight = 3,
  kTightLeft = 4,
  kTightRight = 5,
  kLeft = 6,
  kRight = 7,
  kUturnLeft = 8,
  kUturnRight = 9,
  kDestination = 10,
  kRoundaboutCw = 14,
  kRoundaboutCcw = 15,
  kHwyInLeft = 25,
  kHwyInRight = 26,
  kHwyOutLeft = 28,
  kHwyOutRight = 29,
  kLaneChangeLeft = 32,
  kLaneChangeRight = 33
};
enum {
  kUnitKm = 0,
  kUnitMi = 1,
  kUnitM = 3,
  kUnitYd = 4,
  kUnitFt = 5
};
enum {
  kLaneTake = 1,
  kLaneOther = 2,
  kLaneMax = 8
};

// iAP2 maneuver type (RouteGuidanceManeuverUpdate 0x5202, as hudshapes.h draws them) -> KOMBI direction. side: 1 = left-hand traffic.
// angle: the exit angle, positive = right; ramps without a side in the type go by it, else by the traffic side.
inline int direction(u32 type, int angle, u32 side) {
  bool lht = side == 1;
  int uturn = lht ? kUturnRight : kUturnLeft, round = lht ? kRoundaboutCw : kRoundaboutCcw;
  bool rampLeft = angle != 0 ? angle < 0 : lht;
  switch (type) {
    case 1:
    case 20:
      return kLeft;
    case 2:
    case 21:
      return kRight;
    case 4:
    case 18:
    case 26:
      return uturn;
    case 6:
    case 7:
    case 19:
      return round;
    case 8:
      return rampLeft ? kHwyOutLeft : kHwyOutRight;
    case 9:
      return rampLeft ? kHwyInLeft : kHwyInRight;
    case 10:
    case 12:
    case 24:
    case 25:
    case 27:
      return kDestination;
    case 13:
    case 49:
      return kHalfLeft;
    case 14:
    case 50:
      return kHalfRight;
    case 22:
      return kHwyOutLeft;
    case 23:
      return kHwyOutRight;
    case 47:
      return kTightLeft;
    case 48:
      return kTightRight;
    case 52:
      return kLaneChangeLeft;
    case 53:
      return kLaneChangeRight;
    default:
      return type >= 28 && type <= 46 ? round : kStraight;  // 28..46 roundabout exits; straight, follow, ferry, start, unknown
  }
}
// a lane's direction (degrees, positive = right) -> KOMBI lane arrow
inline int laneArrow(int a) {
  bool left = a < 0;
  int m = left ? -a : a;
  if (m <= 20)
    return kStraight;
  if (m <= 60)
    return left ? kHalfLeft : kHalfRight;
  if (m <= 120)
    return left ? kLeft : kRight;
  if (m <= 170)
    return left ? kTightLeft : kTightRight;
  return left ? kUturnLeft : kUturnRight;
}
// iAP2 distance units 0 km, 1 mi, 2 m, 3 yd, 4 ft -> the KOMBI unit byte; or by the iPhone's unit text
inline int unitOf(u32 iap2) {
  static const u8 k[5] = { kUnitKm, kUnitMi, kUnitM, kUnitYd, kUnitFt };
  return iap2 < 5 ? k[iap2] : -1;
}
inline int unitOfText(const char *u) {
  if (!u || !u[0])
    return -1;
  if (u[0] == 'k' && u[1] == 'm' && !u[2])
    return kUnitKm;
  if (u[0] == 'm' && u[1] == 'i' && !u[2])
    return kUnitMi;
  if (u[0] == 'm' && !u[1])
    return kUnitM;
  if (u[0] == 'y' && u[1] == 'd' && !u[2])
    return kUnitYd;
  if (u[0] == 'f' && u[1] == 't' && !u[2])
    return kUnitFt;
  return -1;
}

// ---- bytes ------------------------------------------------------------------------------------------------------------------------
inline void putU32(u8 *b, u32 v) {
  b[0] = (u8)(v >> 24);
  b[1] = (u8)(v >> 16);
  b[2] = (u8)(v >> 8);
  b[3] = (u8)v;
}
inline u32 getU32(const u8 *b) {
  return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}
// IEEE doubles of whole metres 0..2^31 without floating point (the head unit's libc has no soft-float helpers): big-endian bytes
inline u32 f64ToU(const u8 *p) {
  u32 hi = getU32(p), lo = getU32(p + 4);
  if (hi >> 31)
    return 0;
  int e = (int)((hi >> 20) & 0x7ff) - 1023;
  if (e < 0)
    return 0;
  if (e > 30)
    return 0x7fffffff;
  u32 m = (hi & 0xfffff) | 0x100000;  // the top 21 bits of the 53-bit mantissa
  int s = 52 - e;                     // 22..52
  return s >= 32 ? m >> (s - 32) : (m << (32 - s)) | (lo >> s);
}
inline void uToF64(u8 *p, u32 v) {
  u32 hi = 0, lo = 0;
  if (v) {
    int k = 31;
    while (!(v >> k))
      k--;
    u32 f = v ^ (1u << k);
    int s = 52 - k;  // fraction bits, shifted to bit 52 - k
    hi = (u32)(k + 1023) << 20;
    if (s >= 32)
      hi |= f << (s - 32);
    else {
      hi |= f >> (32 - s);
      lo = f << s;
    }
  }
  putU32(p, hi);
  putU32(p + 4, lo);
}
inline u8 modelType(const u8 *b, u32 n) {
  return n > 8 ? b[8] : 0;
}
inline u32 modelIndex(const u8 *b, u32 n) {
  return n >= 8 ? getU32(b + 4) & 0xffffff : 0;
}

inline bool methodAllowed(u16 m) {  // never 0x0001 (create / dispose)
  switch (m) {
    case 0x0002:
    case 0x0003:
    case 0x0004:
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000E:
    case 0x0010:
      return true;
  }
  return false;
}

// UTF-8 -> UTF-16BE, at most maxChars characters (cap bytes); U+FFFD for bad bytes, surrogate pairs above U+FFFF
inline u32 utf16be(const char *s, u32 maxChars, u8 *o, u32 cap) {
  u32 n = 0, ch = 0;
  for (u32 i = 0; s[i] && ch < maxChars;) {
    u8 c = (u8)s[i];
    u32 cp = 0xfffd, k = 1;
    if (c < 0x80)
      cp = c;
    else if ((c & 0xe0) == 0xc0) {
      k = 2;
      cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      k = 3;
      cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      k = 4;
      cp = c & 0x07;
    }
    for (u32 j = 1; j < k; j++) {
      u8 d = (u8)s[i + j];
      if ((d & 0xc0) != 0x80) {
        cp = 0xfffd;
        k = 1;
        break;
      }
      cp = cp << 6 | (d & 0x3f);
    }
    i += k;
    if (cp >= 0x10000) {
      if (n + 4 > cap)
        break;
      cp -= 0x10000;
      u32 h = 0xd800 | cp >> 10, l = 0xdc00 | (cp & 0x3ff);
      o[n++] = (u8)(h >> 8);
      o[n++] = (u8)h;
      o[n++] = (u8)(l >> 8);
      o[n++] = (u8)l;
    } else {
      if (n + 2 > cap)
        break;
      o[n++] = (u8)(cp >> 8);
      o[n++] = (u8)cp;
    }
    ch++;
  }
  return n;
}

// ---- a batch of frames (method + body), in one arena ----------------------------------------------------------------------------
enum {
  kMaxFrames = 112,
  kArena = 12288,
  kBody = 1024
};
struct Batch {
  u32 n, used;
  bool bad;
  u16 method[kMaxFrames];
  u32 off[kMaxFrames], len[kMaxFrames];
  u8 data[kArena];
  void clear() {
    n = used = 0;
    bad = false;
  }
  bool add(u16 m, const u8 *b, u32 l) {
    if (n >= kMaxFrames || used + l > kArena) {
      bad = true;
      return false;
    }
    method[n] = m;
    off[n] = used;
    len[n] = l;
    memcpy(data + used, b, l);
    used += l;
    n++;
    return true;
  }
  const u8 *body(u32 i) const {
    return data + off[i];
  }
};

// Replaces the UTF-16BE string at the first FEFF BOM (from body[18]); its length byte sits just before the BOM (2 + 2 * chars + 2).
// *end = the offset right after the new string's NUL. False: no string, or the text does not fit.
inline bool patchText(u8 *b, u32 &n, const char *text, u32 maxChars, u32 *end) {
  for (u32 i = 18; i + 1 < n; i++) {
    if (b[i] != 0xfe || b[i + 1] != 0xff)
      continue;
    u32 slen = i - 1, tail = i + b[slen];
    if (tail > n)
      return false;
    u8 enc[200];
    u32 e = utf16be(text, maxChars, enc, sizeof enc);
    if (2 + e + 2 > 255)
      return false;
    u32 nn = slen + 1 + 2 + e + 2 + (n - tail);
    if (nn > kBody)
      return false;
    u8 t[kBody];
    u32 k = 0;
    memcpy(t, b, slen);
    k = slen;
    t[k++] = (u8)(2 + e + 2);
    t[k++] = 0xfe;
    t[k++] = 0xff;
    memcpy(t + k, enc, e);
    k += e;
    t[k++] = 0;
    t[k++] = 0;
    memcpy(t + k, b + tail, n - tail);
    k += n - tail;
    memcpy(b, t, k);
    n = k;
    putU32(b + 9, n - 17);
    if (end)
      *end = i + 2 + e + 2;
    return true;
  }
  return false;
}

struct Lanes {
  u32 n;
  u8 kind[kLaneMax], arrow[kLaneMax];
};
inline bool sameLanes(const Lanes &a, const Lanes &b) {
  if (a.n != b.n)
    return false;
  for (u32 i = 0; i < a.n; i++)
    if (a.kind[i] != b.kind[i] || a.arrow[i] != b.arrow[i])
      return false;
  return true;
}

// ---- the lane model 0x1B (method 0x0010), NBT-AAD's decoding of 126 factory frames -------------------------------------------
//   body = handle | model id (0x1B000000) | 0x1B | len - 17 | 1 | model id | 0x1B | n | lanes | m | boundaries | d0 | d1 | d2 | 0
//   lane = 1 | kind | 1 | 1 + arrow | 0 (no hint bytes) | 0;  boundary left of each lane + a 0: 0 at the edge / between different
//   arrows, 2 between the same arrow. d0 = d1 = the route position (metres to the destination) where the section starts, d2 its length.
const u32 kLaneModel0 = 0x1B000000;
enum {
  kLaneSectionM = 120,
  kLaneMarginM = 100
};
inline u32 encodeLaneModel(u8 *o, u32 handle, const Lanes &l, u32 d0, u32 d2) {
  u8 body[400];
  u32 k = 0;
  putU32(body + k, kLaneModel0);
  k += 4;
  body[k++] = 0x1B;
  u32 at = k;
  k += 4;
  for (u32 i = 0; i < l.n; i++) {
    body[k++] = 1;
    body[k++] = l.kind[i];
    body[k++] = 1;
    putU32(body + k, 1);
    k += 4;
    body[k++] = l.arrow[i];
    putU32(body + k, 0);
    k += 4;
    putU32(body + k, 0);
    k += 4;
  }
  putU32(body + at, k - at - 4);
  putU32(body + k, l.n + 1);
  k += 4;
  for (u32 i = 0; i < l.n; i++)
    body[k++] = i == 0 || l.arrow[i] != l.arrow[i - 1] ? 0 : 2;
  body[k++] = 0;
  uToF64(body + k, d0);
  k += 8;
  uToF64(body + k, d0);
  k += 8;
  uToF64(body + k, d2);
  k += 8;
  body[k++] = 0;
  u32 n = 0;
  putU32(o + n, handle);
  n += 4;
  putU32(o + n, kLaneModel0);
  n += 4;
  o[n++] = 0x1B;
  putU32(o + n, k);
  n += 4;
  putU32(o + n, 1);
  n += 4;
  memcpy(o + n, body, k);
  n += k;
  return n;
}
// a 0x0F node with its lane-model id list (body[54:58] = byte length, the ids after it) replaced
inline bool withLaneRefs(u8 *b, u32 &n, const u32 *refs, u32 nr) {
  if (modelType(b, n) != 15 || n < 62 || b[21] != 15)
    return false;
  u32 len = getU32(b + 54);
  if (len & 3 || n < 58 + len + 4)
    return false;
  u8 t[kBody];
  u32 k = 54;
  if (54 + 4 + 4 * nr + (n - 58 - len) > kBody)
    return false;
  memcpy(t, b, 54);
  putU32(t + k, 4 * nr);
  k += 4;
  for (u32 i = 0; i < nr; i++) {
    putU32(t + k, refs[i]);
    k += 4;
  }
  memcpy(t + k, b + 58 + len, n - 58 - len);
  k += n - 58 - len;
  memcpy(b, t, k);
  n = k;
  putU32(b + 9, n - 17);
  return true;
}

// ---- the route graph --------------------------------------------------------------------------------------------------------
struct Graph {
  u32 handle;
  int bar;  // bar: the red distance bar, 255 hidden (without a BMW route its percentage means nothing)
  int posFrame;
  u32 tmplPos;  // the template's 0x11 destination info with the car position (4132 m), -1 = none
  bool anchor;
  u32 m0, span;  // maneuver 0's route position and the section span of the last graph with lanes

  void init(u32 h = 0xC8, int barPct = 255) {
    handle = h;
    bar = barPct;
    posFrame = -1;
    tmplPos = 0;
    anchor = false;
    m0 = span = 0;
    for (u32 i = 0; i < T::phase_b_count; i++) {
      const T::Frame &t = T::phase_b[i];
      if (t.method != 0x0009 || t.len < 30 || t.body[8] != 17)
        continue;
      u32 p = f64ToU(t.body + 22);
      if (p) {
        posFrame = (int)i;
        tmplPos = p;
      }
    }
  }

  // maneuver 0's fields in one frame (b, n: a copy of a template body). dist 0 = keep the template's; unit < 0 = keep its unit byte.
  bool patch(u16 m, u8 *b, u32 &n, int dir, const char *road, const char *dist, int unit) const {
    putU32(b, handle);
    if ((m != 0x000A && m != 0x0003) || modelIndex(b, n) != 0)
      return true;
    u8 t = modelType(b, n);
    if (m == 0x000A && t == 15) {
      if (n <= 33 || b[21] != 15)
        return false;
      b[33] = (u8)dir;
    } else if (m == 0x000A && t == 29) {
      if (bar >= 0) {
        if (n <= 22 || b[21] != 29 || (b[22] != 0 && b[22] != 100 && b[22] != bar))
          return false;
        b[22] = (u8)bar;
      }
      if (dist && dist[0]) {
        u32 end = 0;
        if (!patchText(b, n, dist, 8, &end))
          return false;
        if (unit >= 0) {
          if (end >= n || (b[end] != 0x01 && b[end] != 0x05))
            return false;
          b[end] = (u8)unit;
        }
      }
    } else if (m == 0x0003 && t == 3 && road && road[0]) {
      if (!patchText(b, n, road, 40, 0))
        return false;
    }
    return true;
  }

  // (d0, d2) of maneuver 0's junction section at route position p0: it ends at the maneuver and starts kLaneMarginM before the car
  // position claimed (p0 + span)
  void laneSection(u32 p0, u32 &d0, u32 &d2) {
    u32 sp = tmplPos > p0 ? tmplPos - p0 : kLaneSectionM;
    anchor = true;
    m0 = p0;
    span = sp;
    d0 = p0 + sp + kLaneMarginM;
    d2 = sp + kLaneMarginM;
  }

  // The whole route-start graph (97 frames in factory order; 93 + 1 with lanes) with maneuver 0 patched
  bool start(int dir, const char *road, const char *dist, int unit, const Lanes *lanes, Batch &out) {
    out.clear();
    bool withLanes = lanes && lanes->n, hasPos = false;
    u32 pos = 0;
    static u8 b[kBody], lm[kBody];
    for (u32 i = 0; i < T::phase_b_count; i++) {
      const T::Frame &t = T::phase_b[i];
      if (t.len > kBody)
        return false;
      u32 n = (u32)t.len;
      memcpy(b, t.body, n);
      if (!patch(t.method, b, n, dir, road, dist, unit))
        return false;
      if (withLanes) {
        if (t.method == 0x0010)
          continue;  // the template's own lane models
        if (t.method == 0x000A && modelType(b, n) == 29 && modelIndex(b, n) == 0 && n >= 31) {
          pos = f64ToU(b + 23);
          hasPos = true;
        }
        if (t.method == 0x000A && modelType(b, n) == 15) {
          if (modelIndex(b, n) == 0) {
            if (!hasPos)
              return false;
            u32 d0, d2;
            laneSection(pos, d0, d2);
            out.add(0x0010, lm, encodeLaneModel(lm, handle, *lanes, d0, d2));
            u32 ours = kLaneModel0;
            if (!withLaneRefs(b, n, &ours, 1))
              return false;
          } else if (n >= 58 && getU32(b + 54)) {
            if (!withLaneRefs(b, n, 0, 0))
              return false;
          }
        }
      }
      out.add(t.method, b, n);
    }
    return !out.bad;
  }

  // In-place update of maneuver 0 on a route already shown: the factory's own maneuver-0 update block (text resource .. state 2)
  bool maneuver(int dir, const char *road, const char *dist, int unit, const Lanes *lanes, Batch &all, Batch &out) {
    out.clear();
    if (!start(dir, road, dist, unit, lanes, all))
      return false;
    u32 from = all.n;
    for (u32 i = 0; i < all.n; i++)
      if (all.method[i] == 0x0003 && modelType(all.body(i), all.len[i]) == 3 && modelIndex(all.body(i), all.len[i]) == 0)
        from = i;
    bool desc = false, dist0 = false;
    for (u32 i = from; i < all.n; i++) {
      const u8 *b = all.body(i);
      u32 n = all.len[i];
      out.add(all.method[i], b, n);
      if (all.method[i] == 0x000A && modelIndex(b, n) == 0) {
        desc = desc || modelType(b, n) == 15;
        dist0 = dist0 || modelType(b, n) == 29;
      }
      if (all.method[i] == 0x0009 && modelType(b, n) == 33)
        break;  // state 2 closes the block
    }
    if (!desc || !dist0 || !out.n || out.method[out.n - 1] != 0x0009 || out.bad) {
      out.clear();
      return false;
    }
    return true;
  }

  // one distance update: the template's last maneuver-0 ManeuverDistance frame, patched
  bool distance(const char *dist, int unit, Batch &out) const {
    out.clear();
    static u8 b[kBody];
    for (u32 i = T::phase_b_count; i-- > 0;) {
      const T::Frame &t = T::phase_b[i];
      if (t.method != 0x000A || t.len > kBody)
        continue;
      u32 n = (u32)t.len;
      memcpy(b, t.body, n);
      if (modelType(b, n) != 29 || modelIndex(b, n) != 0)
        continue;
      if (!patch(0x000A, b, n, 0, 0, dist, unit))
        return false;
      return out.add(0x000A, b, n);
    }
    return false;
  }

  // one 0x11 destination info carrying the car's route position (the KOMBI re-checks the lane section on each)
  bool position(u32 posM, Batch &out) const {
    out.clear();
    if (posFrame < 0)
      return false;
    const T::Frame &t = T::phase_b[posFrame];
    u8 b[kBody];
    u32 n = (u32)t.len;
    memcpy(b, t.body, n);
    putU32(b, handle);
    uToF64(b + 22, posM);
    return out.add(t.method, b, n);
  }
  // the route position for the lane feed: maneuver 0 + the metres to it, kept inside the last lane section
  bool feedPosition(bool hasStep, u32 stepM, u32 &pos) const {
    if (!anchor)
      return false;
    pos = m0 + (!hasStep || stepM > span ? span : stepM);
    return true;
  }

  // the factory cancel (guidance state 7 -> 5 -> 0, empty maneuver list, init, destination clear); state0: only its state-0 frame
  void cancel(Batch &out, bool state0Only = false) const {
    out.clear();
    u8 b[kBody];
    for (u32 i = 0; i < T::cancel_count; i++) {
      const T::Frame &t = T::cancel[i];
      u32 n = (u32)t.len;
      memcpy(b, t.body, n);
      putU32(b, handle);
      if (state0Only && !(t.method == 0x0009 && n > 30 && b[30] == 0))
        continue;
      out.add(t.method, b, n);
    }
  }
};

// ---- SOME/IP over TCP --------------------------------------------------------------------------------------------------------
// The socket calls: in the HMI from libsocket (looked up, so the plugin has no new imports and an absent library only disables this).
// Constants as QNX 6.5's io-pkt (BSD), which the Mac shares except O_NONBLOCK.
#ifdef KOMBI_HOST
struct Sock {
  bool ok = true;
  int socket_(int a, int b, int c) {
    return ::socket(a, b, c);
  }
  int connect_(int f, const void *a, unsigned l) {
    return ::connect(f, (const sockaddr *)a, l);
  }
  long send_(int f, const void *b, unsigned long n, int fl) {
    return ::send(f, b, n, fl);
  }
  long recv_(int f, void *b, unsigned long n, int fl) {
    return ::recv(f, b, n, fl);
  }
  int setsockopt_(int f, int l, int o, const void *v, unsigned n) {
    return ::setsockopt(f, l, o, v, n);
  }
  int getsockopt_(int f, int l, int o, void *v, unsigned *n) {
    socklen_t k = *n;
    int r = ::getsockopt(f, l, o, v, &k);
    *n = k;
    return r;
  }
  int getpeername_(int f, void *a, unsigned *n) {
    socklen_t k = *n;
    int r = ::getpeername(f, (sockaddr *)a, &k);
    *n = k;
    return r;
  }
  bool load() {
    return true;
  }
};
enum {
  kNonblock = O_NONBLOCK
};
struct Tv {
  struct timeval t;
  void set(u32 ms) {
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
  }
};
inline void sleepMs(u32 ms) {
  usleep(ms * 1000);
}
#else
struct Sock {
  bool ok;
  int (*socket_)(int, int, int);
  int (*connect_)(int, const void *, unsigned);
  long (*send_)(int, const void *, unsigned long, int);
  long (*recv_)(int, void *, unsigned long, int);
  int (*setsockopt_)(int, int, int, const void *, unsigned);
  int (*getsockopt_)(int, int, int, void *, unsigned *);
  int (*getpeername_)(int, void *, unsigned *);
  bool load() {
    if (ok)
      return true;
    void *h = 0;  // libsocket.so.3 on NBT Evo (HmiMain3D links it), older QNX .2
    const char *const kLibs[3] = { "libsocket.so.3", "libsocket.so.2", "libsocket.so" };
    for (u32 i = 0; i < 3 && !h; i++)
      h = dlopen(kLibs[i], 0x0002 /* RTLD_NOW */);
    if (!h)
      return false;
    socket_ = (int (*)(int, int, int))dlsym(h, "socket");
    connect_ = (int (*)(int, const void *, unsigned))dlsym(h, "connect");
    send_ = (long (*)(int, const void *, unsigned long, int))dlsym(h, "send");
    recv_ = (long (*)(int, void *, unsigned long, int))dlsym(h, "recv");
    setsockopt_ = (int (*)(int, int, int, const void *, unsigned))dlsym(h, "setsockopt");
    getsockopt_ = (int (*)(int, int, int, void *, unsigned *))dlsym(h, "getsockopt");
    getpeername_ = (int (*)(int, void *, unsigned *))dlsym(h, "getpeername");
    ok = socket_ && connect_ && send_ && recv_ && setsockopt_ && getsockopt_ && getpeername_;
    return ok;
  }
};
enum {
  kNonblock = 0x80
};  // QNX O_NONBLOCK
struct Tv {
  struct {
    long sec, usec;
  } t;
  void set(u32 ms) {
    t.sec = (long)(ms / 1000);
    t.usec = (long)(ms % 1000) * 1000;
  }
};
inline void sleepMs(u32 ms) {
  usleep(ms * 1000);
}
#endif
#ifdef KOMBI_HOST
// the tests' host: its own values (the Mac's are QNX's; Linux's differ)
enum {
  kAfInet = AF_INET,
  kSockStream = SOCK_STREAM,
  kSolSocket = SOL_SOCKET,
  kSoSndTimeo = SO_SNDTIMEO,
  kSoRcvTimeo = SO_RCVTIMEO,
  kSoError = SO_ERROR,
  kIpprotoTcp = IPPROTO_TCP,
  kTcpNodelay = TCP_NODELAY,
  kMsgPeek = MSG_PEEK,
  kMsgDontwait = MSG_DONTWAIT,
  kFGetfl = F_GETFL,
  kFSetfl = F_SETFL
};
#else
enum {
  kAfInet = 2,
  kSockStream = 1,
  kSolSocket = 0xffff,
  kSoSndTimeo = 0x1005,
  kSoRcvTimeo = 0x1006,
  kSoError = 0x1007,
  kIpprotoTcp = 6,
  kTcpNodelay = 1,
  kMsgPeek = 0x2,
  kMsgDontwait = 0x80,
  kFGetfl = 3,
  kFSetfl = 4
};
#endif
#if defined(KOMBI_HOST) && defined(__linux__)
struct SockAddr {
  u16 family;  // Linux sockaddr_in: no length byte, a 16-bit family
  u8 port[2];
  u8 addr[4];
  u8 zero[8];
  void setFamily() {
    family = kAfInet;
  }
};
#else
struct SockAddr {
  u8 len, family;
  u8 port[2];
  u8 addr[4];
  u8 zero[8];
  void setFamily() {
    len = sizeof *this;
    family = kAfInet;
  }
};  // BSD sockaddr_in (QNX and the Mac)
#endif

const u16 kClientId = 0x7E01;  // our own SOME/IP client id (NBT-AAD's); the factory head unit uses its own
enum {
  kMaxReply = 4096,
  kMaxCookies = 4
};

struct Client {
  Sock s;
  u8 ip[4];
  u16 port;
  u32 timeoutMs;
  int fd;
  u16 session;
  char detail[96];

  void init(const u8 *addr = 0, u16 p = 30502, u32 tmo = 3000) {
    static const u8 kDef[4] = { 160, 48, 199, 96 };
    memcpy(ip, addr ? addr : kDef, 4);
    port = p;
    timeoutMs = tmo;
    fd = -1;
    session = 0;
    detail[0] = 0;
  }
  void why(const char *t) {
    u32 i = 0;
    while (t[i] && i + 1 < sizeof detail) {
      detail[i] = t[i];
      i++;
    }
    detail[i] = 0;
  }
  void closeFd() {
    if (fd >= 0)
      close(fd);
    fd = -1;
  }
  u16 next() {
    session = (u16)(session == 0xfffe ? 1 : session + 1);
    return session;
  }

  // A TCP connection within ms: non-blocking connect, then getpeername until it is connected (no poll(): its flags differ per system)
  int connectFd(u32 ms) {
    if (!s.load()) {
      why("no socket library");
      return -1;
    }
    int f = s.socket_(kAfInet, kSockStream, 0);
    if (f < 0) {
      why("socket failed");
      return -1;
    }
    int fl = fcntl(f, kFGetfl, 0);
    fcntl(f, kFSetfl, fl | kNonblock);
    SockAddr a;
    memset(&a, 0, sizeof a);
    a.setFamily();
    a.port[0] = (u8)(port >> 8);
    a.port[1] = (u8)port;
    memcpy(a.addr, ip, 4);
    s.connect_(f, &a, sizeof a);
    bool up = false;
    for (u32 t = 0; t <= ms; t += 20) {
      SockAddr p;
      unsigned pl = sizeof p;
      if (s.getpeername_(f, &p, &pl) == 0) {
        up = true;
        break;
      }
      int err = 0;
      unsigned el = sizeof err;
      if (s.getsockopt_(f, kSolSocket, kSoError, &err, &el) == 0 && err)
        break;
      sleepMs(20);
    }
    if (!up) {
      close(f);
      why("connect failed or timed out");
      return -1;
    }
    fcntl(f, kFSetfl, fl & ~kNonblock);
    Tv tv;
    tv.set(timeoutMs);
    s.setsockopt_(f, kSolSocket, kSoRcvTimeo, &tv.t, sizeof tv.t);
    s.setsockopt_(f, kSolSocket, kSoSndTimeo, &tv.t, sizeof tv.t);
    int one = 1;
    s.setsockopt_(f, kIpprotoTcp, kTcpNodelay, &one, sizeof one);
    return f;
  }
  // Is the KOMBI's guidance port there? (detection; the connection is closed again)
  bool probe(u32 ms) {
    int f = connectFd(ms);
    if (f < 0)
      return false;
    close(f);
    return true;
  }

  bool sendAll(int f, const u8 *p, u32 n) {
    while (n) {
      long w = s.send_(f, p, n, 0);
      if (w <= 0)
        return false;
      p += w;
      n -= (u32)w;
    }
    return true;
  }
  bool recvExact(int f, u8 *p, u32 n) {
    while (n) {
      long r = s.recv_(f, p, n, 0);
      if (r <= 0)
        return false;
      p += r;
      n -= (u32)r;
    }
    return true;
  }
  // one message (16-byte header + payload); 0 ok, 1 connection problem, 2 bad length
  int readMessage(int f, u8 *m, u32 &n) {
    if (!recvExact(f, m, 16))
      return 1;
    u32 len = getU32(m + 4);
    if (len < 8 || len + 8 > kMaxReply)
      return 2;
    n = len + 8;
    return len > 8 && !recvExact(f, m + 16, len - 8) ? 1 : 0;
  }
  static bool isCookie(const u8 *m) {
    return m[0] == 0xff && m[1] == 0xff && m[2] == 0x80 && m[3] == 0x00;
  }
  // queued KOMBI messages (magic cookies while idle) dropped before a request; false = the connection is unusable
  bool drain() {
    for (;;) {
      u8 h[16];
      long r = s.recv_(fd, h, 16, kMsgPeek | kMsgDontwait);
      if (r == 0) {
        why("connection closed by the KOMBI");
        return false;
      }
      if (r < 0)
        return true;  // nothing waiting
      static u8 m[kMaxReply];
      u32 n;
      if (readMessage(fd, m, n)) {
        why("drain failed");
        return false;
      }
    }
  }
  // one request and its reply (cookies skipped). false + rejected: the KOMBI answered with something other than a matching E_OK
  bool sendFrame(int f, u16 method, const u8 *body, u32 len, u16 sess, bool &rejected) {
    rejected = false;
    if (!methodAllowed(method)) {
      rejected = true;
      why("method not whitelisted");
      return false;
    }
    static u8 req[16 + kBody + 16];
    if (len > kBody) {
      rejected = true;
      why("frame too long");
      return false;
    }
    req[0] = 0xB0;
    req[1] = 0x06;
    req[2] = (u8)(method >> 8);
    req[3] = (u8)method;
    putU32(req + 4, 8 + len);
    req[8] = (u8)(kClientId >> 8);
    req[9] = (u8)kClientId;
    req[10] = (u8)(sess >> 8);
    req[11] = (u8)sess;
    req[12] = 1;
    req[13] = 1;
    req[14] = 0;
    req[15] = 0;
    memcpy(req + 16, body, len);
    if (!sendAll(f, req, 16 + len)) {
      why("send failed");
      return false;
    }
    static u8 m[kMaxReply];
    u32 n = 0;
    for (u32 c = 0;; c++) {
      int e = readMessage(f, m, n);
      if (e == 1) {
        why("no reply / connection closed");
        return false;
      }
      if (e == 2) {
        why("invalid SOME/IP length");
        return false;
      }
      if (!isCookie(m))
        break;
      if (c >= kMaxCookies) {
        why("only magic cookies received");
        return false;
      }
    }
    u16 svc = (u16)(m[0] << 8 | m[1]), rm = (u16)(m[2] << 8 | m[3]), cid = (u16)(m[8] << 8 | m[9]), rs = (u16)(m[10] << 8 | m[11]);
    if (svc != 0xB006 || rm != method || cid != kClientId || rs != sess || m[14] != 0x80 || m[15] != 0x00) {
      rejected = true;
      why("reply is not a matching E_OK");
      return false;
    }
    return true;
  }
  enum Result {
    kOk = 0,
    kLink = 1,
    kRejected = 2
  };
  // the frames in order on the persistent connection; a connection error gets one reconnect and that frame again (idempotent updates)
  Result send(const Batch &b) {
    for (u32 i = 0; i < b.n; i++) {
      bool done = false;
      for (int attempt = 1; attempt <= 2 && !done; attempt++) {
        if (fd < 0 && (fd = connectFd(timeoutMs)) < 0) {
          if (attempt == 2)
            return kLink;
          continue;
        }
        bool rej = false;
        if (drain() && sendFrame(fd, b.method[i], b.body(i), b.len[i], next(), rej)) {
          done = true;
          break;
        }
        if (rej)
          return kRejected;
        closeFd();
        if (attempt == 2)
          return kLink;
      }
    }
    return kOk;
  }
  bool runPhase(const Batch &b, u16 s0) {
    int f = connectFd(timeoutMs);
    if (f < 0)
      return false;
    bool ok = true;
    for (u32 i = 0; i < b.n && ok; i++) {
      bool rej;
      ok = sendFrame(f, b.method[i], b.body(i), b.len[i], (u16)(s0 + i), rej);
    }
    close(f);
    return ok;
  }
  // the factory cancel on a fresh connection, else its state-0 frame alone
  bool cancel(const Graph &g) {
    static Batch full, s0;
    g.cancel(full);
    g.cancel(s0, true);
    closeFd();
    return runPhase(full, 400) || runPhase(s0, 500);
  }
};

// ---- what the cluster shows: NBT-AAD's display driver (j29_display.cpp) for the iPhone's guidance ---------------------------
struct Man {
  int dir;
  int unit;
  char road[96];
  char dist[16];
  Lanes lanes;
  bool hasStep;
  bool roadFb;  // the street is the car's current road (the maneuver names none): a change of it alone redraws nothing
  u32 metres;
};

inline void copyText(char *d, u32 cap, const char *s) {
  u32 i = 0;
  while (s[i] && s[i] != '\n' && i + 1 < cap) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}
// ---- the street, made to fit the windscreen HUD (1.3.5) ----------------------------------------------------------------------
// The KOMBI draws the street in its own font, in the cluster and in the windscreen HUD. The guidance service takes the text alone
// (TextResource 0x03: a language tag and a UTF-16 string; no font, no size, no second text for the HUD), and the HUD's field is the
// narrower one: it cuts the end off at its right edge (ghackt, J29 / A90: 1.2.9 with up to 40 characters; 1.3.3 with "Exit 104  ..."
// lines of up to 22). How wide the field is has not been published (BMW: the HUD "can only show a certain number of text characters"),
// so the street is fitted by its width, not its length: in the advance widths of a grotesque sans like the KOMBI's (Helvetica's,
// 1/1000 em: a digit 556, "i" 222, "W" 944), into a field g_roadDigits digits wide less half a digit for the KOMBI font's own widths.
// g_roadDigits: the file kombi_road_max (8..60, rgkombi.cpp): the whole digits the HUD shows of kombi_road_ruler's row; 0 =
// kRoadDigits. 1.3.3 sent up to 16.7 digits ("Exit 104  Main Street"), and those were cut off.
const u32 kDigitW = 556;
const u32 kRoadDigits = 14;
const u32 kRoadMinW = 4 * kDigitW;  // the least room for a road after an exit number
u32 g_roadDigits = 0;
bool g_ruler = false;  // kombi_road_ruler: this row instead of every street, to count how many digits the HUD shows whole
const char kRuler[] = "1234567890123456789012345678901234567890";
inline u32 roadWidth() {
  return (g_roadDigits ? g_roadDigits : kRoadDigits) * kDigitW - kDigitW / 2;
}
// The advance width of the character that starts with byte c (UTF-8): ASCII from the table, other letters 600, CJK and symbols 1000,
// a continuation byte 0
inline u32 leadW(u8 c) {
  static const u16 kW[95] = {
    278,  278, 355, 556, 556, 889, 667, 222, 333, 333, 389, 584, 278, 333, 278, 278,  // space ! " # $ % & ' ( ) * + , - . /
    556,  556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,  // 0-9 : ; < = > ?
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,  // @ A-O
    667,  778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,  // P-Z [ \ ] ^ _
    222,  556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,  // ` a-o
    556,  556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584,       // p-z { | } ~
  };
  if (c >= 0x20 && c < 0x7f)
    return kW[c - 0x20];
  return c < 0xc0 ? 0 : c < 0xe0 ? 600 : 1000;
}
// the width of n bytes of UTF-8 text, in 1/1000 em
inline u32 textW(const char *s, u32 n) {
  u32 w = 0;
  for (u32 i = 0; i < n; i++)
    w += leadW((u8)s[i]);
  return w;
}
inline u32 zlen(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}
// The short forms USPS writes in addresses (Publication 28): the street suffixes as ghackt listed them for 1.3.5, with Route and
// Turnpike; the directions; and the words that also stand in front of a name. Matched as whole words in any case. WAY, LOOP, MALL,
// OVAL, PARK, ROW, RUN and SPUR are their own short forms and stay as the iPhone writes them.
const char *const kSuffix[][2] = {
  { "Avenue", "AVE" },     { "Boulevard", "BLVD" }, { "Circle", "CIR" },       { "Court", "CT" },         { "Drive", "DR" },
  { "Lane", "LN" },        { "Place", "PL" },       { "Road", "RD" },          { "Square", "SQ" },        { "Street", "ST" },
  { "Terrace", "TER" },    { "Alley", "ALY" },      { "Beach", "BCH" },        { "Bend", "BND" },         { "Bluff", "BLF" },
  { "Bottom", "BTM" },     { "Bridge", "BRG" },     { "Brook", "BRK" },        { "Burg", "BG" },          { "Bypass", "BYP" },
  { "Causeway", "CSWY" },  { "Center", "CTR" },     { "Canyon", "CYN" },       { "Cape", "CPE" },         { "Crossing", "XING" },
  { "Crest", "CRST" },     { "Creek", "CRK" },      { "Crescent", "CRES" },    { "Dale", "DL" },          { "Dam", "DM" },
  { "Divide", "DV" },      { "Estate", "EST" },     { "Expressway", "EXPY" },  { "Extension", "EXT" },    { "Falls", "FLS" },
  { "Ferry", "FRY" },      { "Field", "FLD" },      { "Fort", "FT" },          { "Freeway", "FWY" },      { "Garden", "GDN" },
  { "Gateway", "GTWY" },   { "Glen", "GLN" },       { "Grove", "GRV" },        { "Harbor", "HBR" },       { "Heights", "HTS" },
  { "Highway", "HWY" },    { "Hill", "HL" },        { "Hollow", "HOLW" },      { "Inlet", "INLT" },       { "Island", "IS" },
  { "Junction", "JCT" },   { "Key", "KY" },         { "Knoll", "KNL" },        { "Lake", "LK" },          { "Landing", "LNDG" },
  { "Light", "LGT" },      { "Lock", "LCK" },       { "Manor", "MNR" },        { "Meadow", "MDW" },       { "Mill", "ML" },
  { "Mission", "MSN" },    { "Mount", "MT" },       { "Mountain", "MTN" },     { "Neck", "NCK" },         { "Orchard", "ORCH" },
  { "Parkway", "PKWY" },   { "Point", "PT" },       { "Port", "PRT" },         { "Prairie", "PR" },       { "Ranch", "RNCH" },
  { "Rapids", "RPDS" },    { "Rest", "RST" },       { "Ridge", "RDG" },        { "River", "RIV" },        { "Shoal", "SHL" },
  { "Shore", "SHR" },      { "Spring", "SPG" },     { "Springs", "SPGS" },     { "Station", "STA" },      { "Stravenue", "STRA" },
  { "Stream", "STRM" },    { "Summit", "SMT" },     { "Trace", "TRCE" },       { "Trail", "TRL" },        { "Trailer", "TRLR" },
  { "Tunnel", "TNL" },     { "Route", "RTE" },      { "Turnpike", "TPKE" },
};
const char *const kDirection[][2] = {
  { "North", "N" },      { "South", "S" },      { "East", "E" },      { "West", "W" },      { "Northeast", "NE" }, { "Northwest", "NW" },
  { "Southeast", "SE" }, { "Southwest", "SW" }, { "Northbound", "NB" }, { "Southbound", "SB" }, { "Eastbound", "EB" },
  { "Westbound", "WB" },
};
const char *const kPrefix[][2] = { { "Mount", "MT" }, { "Fort", "FT" }, { "Point", "PT" }, { "Saint", "St" } };
// w[0..n) is s, ASCII letters in any case
inline bool wordIs(const char *w, u32 n, const char *s) {
  u32 i = 0;
  for (; i < n && s[i]; i++) {
    u8 a = (u8)w[i], b = (u8)s[i];
    if (a >= 'A' && a <= 'Z')
      a = (u8)(a + 32);
    if (b >= 'A' && b <= 'Z')
      b = (u8)(b + 32);
    if (a != b)
      return false;
  }
  return i == n && !s[i];
}
template <u32 N> inline int findWord(const char *const (&tab)[N][2], const char *w, u32 n) {
  for (u32 i = 0; i < N; i++)
    if (wordIs(w, n, tab[i][0]))
      return (int)i;
  return -1;
}
// The short form of w[0..n) in tab, if it is narrower than the word: PRT is wider than Port, CRST than Crest, INLT than Inlet, TRCE
// than Trace, and HOLW as wide as Hollow; those words stay. 0 = none.
template <u32 N> inline const char *shortOf(const char *const (&tab)[N][2], const char *w, u32 n) {
  int i = findWord(tab, w, n);
  return i >= 0 && textW(tab[i][1], zlen(tab[i][1])) < textW(w, n) ? tab[i][1] : 0;
}
inline bool isDirection(const char *w, u32 n) {
  for (u32 i = 0; i < sizeof kDirection / sizeof kDirection[0]; i++)
    if (wordIs(w, n, kDirection[i][0]) || wordIs(w, n, kDirection[i][1]))
      return true;
  return false;
}
// words 0..nw-1 joined by single spaces, each in its short form if it has one; ends[i] = where word i ends. Bytes written (< cap).
inline u32 joinWords(const char *const *w, const u32 *wl, const char *const *rep, u32 nw, char *t, u32 cap, u32 *ends) {
  u32 n = 0;
  for (u32 i = 0; i < nw; i++) {
    const char *s = rep[i] ? rep[i] : w[i];
    u32 l = rep[i] ? zlen(rep[i]) : wl[i];
    if (n && n + 1 < cap)
      t[n++] = ' ';
    for (u32 q = 0; q < l && n + 1 < cap; q++)
      t[n++] = s[q];
    ends[i] = n;
  }
  t[n] = 0;
  return n;
}
// The street made to fit in width (1/1000 em, textW); out holds cap bytes. A street that fits is sent as the iPhone wrote it (its
// spaces made single). A wider one is shortened the way USPS writes addresses, as far as it takes:
//   1. the street suffix (the last word that is not a direction, a number or one or two letters: "Highway 101" HWY 101, "Fifth
//      Avenue South" Fifth AVE S) and the directions (North N, Westbound WB; not one that is the name itself: "North Avenue" North AVE)
//   2. the words that also stand in front of a name, wherever they are (Mount MT, Fort FT, Point PT, Saint St), and every direction
//   3. trailing words dropped, never below the first word that is not a direction ("N Martin")
//   4. what is left cut after its last character that fits
inline void fitRoad(const char *in, char *out, u32 cap, u32 width) {
  const char *w[24], *rep[24];
  u32 wl[24], ends[24], nw = 0;
  for (const char *p = in; nw < 24;) {
    while (*p == ' ')
      p++;
    if (!*p || *p == '\n')
      break;
    u32 l = 0;
    while (p[l] && p[l] != ' ' && p[l] != '\n')
      l++;
    w[nw] = p;
    wl[nw] = l;
    rep[nw++] = 0;
    p += l;
  }
  char t[128];
  u32 n = joinWords(w, wl, rep, nw, t, sizeof t, ends);
  if (textW(t, n) > width || n >= cap) {
    int suf = -1;
    for (int i = (int)nw - 1; i >= 0; i--) {
      if (wl[i] <= 2 || (w[i][0] >= '0' && w[i][0] <= '9') || isDirection(w[i], wl[i]))
        continue;
      if (findWord(kSuffix, w[i], wl[i]) >= 0) {  // the suffix, even one that stays (Port): it is not the name
        rep[i] = shortOf(kSuffix, w[i], wl[i]);
        suf = i;
      }
      break;
    }
    u32 names = 0;
    for (u32 i = 0; i < nw; i++)
      names += (int)i != suf && !isDirection(w[i], wl[i]);
    for (u32 i = 0; i < nw && names; i++)
      if (const char *a = shortOf(kDirection, w[i], wl[i]))
        rep[i] = a;
    n = joinWords(w, wl, rep, nw, t, sizeof t, ends);
    if (textW(t, n) > width) {
      for (u32 i = 0; i < nw; i++) {
        if (!rep[i])
          rep[i] = shortOf(kPrefix, w[i], wl[i]);
        if (!rep[i])
          rep[i] = shortOf(kDirection, w[i], wl[i]);
      }
      n = joinWords(w, wl, rep, nw, t, sizeof t, ends);
    }
    u32 first = 1, keep = nw;
    while (first < nw && isDirection(w[first - 1], wl[first - 1]))
      first++;
    while (keep > first && textW(t, ends[keep - 1]) > width)
      keep--;
    n = keep ? ends[keep - 1] : 0;
    u32 sum = 0, i = 0;
    for (; i < n; i++) {  // a continuation byte adds 0: the cut falls before a character, never inside one
      u32 cw = leadW((u8)t[i]);
      if (sum + cw > width)
        break;
      sum += cw;
    }
    n = i;
  }
  if (n >= cap)
    n = cap - 1;
  while (n && ((u8)t[n] & 0xc0) == 0x80)  // never half a character
    n--;
  while (n && t[n - 1] == ' ')
    n--;
  for (u32 i = 0; i < n; i++)
    out[i] = t[i];
  out[n] = 0;
}
// iAP2 maneuver types that leave a road at a numbered exit, when the iPhone names one: off ramps (8, 22, 23), keep left / right (13,
// 14), slight left / right (49, 50) and highway changes (51..53)
inline bool exitManeuver(u32 type) {
  return type == 8 || type == 13 || type == 14 || type == 22 || type == 23 || (type >= 49 && type <= 53);
}
// the iPhone's guidance (rgview.h) -> the KOMBI's maneuver: direction, street (fitRoad; never "": the template's own street would
// stay; an exit's number in front), distance number + unit from the iPhone's text ("450 m"; without text whole metres, "0 m"
// with nothing), lanes (none rather than a lane the KOMBI cannot draw)
inline void manOf(const rg_view &v, Man &m) {
  memset(&m, 0, sizeof m);
  m.dir = direction(v.type, v.angle, v.side);
  m.roadFb = v.roadfb != 0;
  u32 width = roadWidth();
  {
    char r[96];
    fitRoad(v.road, r, sizeof r, width);
    copyText(m.road, sizeof m.road, r[0] ? r : " ");
  }
  char num[16] = "", unit[8] = "";
  const char *t = v.dist_s;
  while (*t == ' ')
    t++;
  if (*t) {
    u32 k = 0;
    while (*t && *t != ' ' && k < 8)
      num[k++] = *t++;
    num[k] = 0;
    while (*t == ' ')
      t++;
    copyText(unit, sizeof unit, t);
    m.unit = unitOfText(unit);
    if (m.unit < 0)
      m.unit = unitOf(v.units);
  } else if (v.dist) {
    u32 d = v.dist, k = 0;
    char r[12];
    do {
      r[k++] = (char)('0' + d % 10);
      d /= 10;
    } while (d && k < sizeof r);
    for (u32 i = 0; i < k; i++)
      num[i] = r[k - 1 - i];
    num[k] = 0;
    m.unit = kUnitM;
  }
  if (num[0] < '0' || num[0] > '9') {  // no distance (the arrival, a start marker): "0 m" (with "-" the cluster showed "-m")
    num[0] = '0';
    num[1] = 0;
    m.unit = kUnitM;
  }
  copyText(m.dist, sizeof m.dist, num);
  if (m.unit < 0)
    m.unit = kUnitM;
  // An exit (an off ramp, or a keep / slight / highway change the iPhone gives an exit number): "Exit 104  Jefferson Davis HWY". The
  // exit number always whole, then the road fitted into the width left (none with less than kRoadMinW)
  if (v.exitno[0] && exitManeuver(v.type)) {
    char t[96];
    u32 k = 0;
    for (const char *q = "Exit "; *q; q++)
      t[k++] = *q;
    for (const char *q = v.exitno; *q && k < 20; q++)
      t[k++] = *q;
    u32 used = textW(t, k) + 2 * leadW(' ');
    if (!v.roadfb && v.road[0] && v.road[0] != ' ' && used + kRoadMinW <= width) {  // the maneuver's road only, not the car's
      char r[96];
      fitRoad(v.road, r, sizeof t - k - 2, width - used);
      if (r[0]) {
        t[k++] = ' ';
        t[k++] = ' ';
        for (const char *q = r; *q && k + 1 < sizeof t; q++)
          t[k++] = *q;
      }
    }
    t[k] = 0;
    copyText(m.road, sizeof m.road, t);
  }
  if (g_ruler)  // kombi_road_ruler: the row of digits whatever the maneuver, to read the HUD's width off it
    copyText(m.road, sizeof m.road, kRuler);
  m.hasStep = true;
  m.metres = v.dist;
  if (v.nl && v.nl <= kLaneMax) {
    m.lanes.n = v.nl;
    for (u32 i = 0; i < v.nl; i++) {
      const rg_lane &L = v.lanes[i];
      int a = L.hl != 1000 ? L.hl : L.na ? L.ang[0] : 1000;
      if (a == 1000) {
        m.lanes.n = 0;
        break;
      }
      m.lanes.kind[i] = L.status ? kLaneTake : kLaneOther;
      m.lanes.arrow[i] = (u8)laneArrow(a);
    }
  }
}

// The lane row on the same maneuver (1.3.1). iOS's lane data drops out for a moment (the lane index moves on before its lane set
// arrives, LaneGuidanceShowing off between two lane events) and a row that went was sent again when it came back, each change at
// most every 3 s: on the cluster the row went off and on. A row that goes now stays kLaneGapMs, and a new row is sent once it has
// been the same on two ticks in a row (500 ms apart).
const u32 kLaneGapMs = 2500;
struct Output {
  Graph g;
  Client c;
  Batch b, all;
  bool shown, inplaceOk, failed, posValid, clearFailed;
  int lastDir, lastUnit;
  char lastRoad[96], lastDist[16];
  Lanes lastLanes;
  u32 tGraph, tPos, lastPos, retryAt;
  Lanes candLanes;  // a new lane row seen once
  bool candValid, lanesGone;
  u32 tLanesGone;

  void init() {
    g.init();
    c.init();
    reset();
    failed = clearFailed = false;
    retryAt = 0;
  }
  void reset() {
    shown = false;
    inplaceOk = true;
    posValid = false;
    lastDir = lastUnit = -1;
    lastRoad[0] = lastDist[0] = 0;
    lastLanes.n = 0;
    tGraph = tPos = lastPos = 0;
    candValid = lanesGone = false;
    candLanes.n = 0;
    tLanesGone = 0;
  }
  void log(const char *fmt, const char *a = "", const char *b2 = "") {
    if (!g_log)
      return;
    char m[200];
    u32 k = 0;
    for (const char *p = fmt; *p && k + 1 < sizeof m; p++) {
      if (p[0] == '%' && p[1] == 's') {
        const char *s = a;
        a = b2;
        b2 = "";
        while (*s && k + 1 < sizeof m)
          m[k++] = *s++;
        p++;
      } else
        m[k++] = *p;
    }
    m[k] = 0;
    g_log(m);
  }
  static bool eq(const char *x, const char *y) {
    while (*x && *x == *y) {
      x++;
      y++;
    }
    return *x == *y;
  }
  static void cpy(char *d, const char *s, u32 cap) {
    u32 i = 0;
    while (s[i] && i + 1 < cap) {
      d[i] = s[i];
      i++;
    }
    d[i] = 0;
  }

  int send(const char *what, const Batch &x) {
    Client::Result r = c.send(x);
    if (r != Client::kOk)
      log("CarPlayMenu: KOMBI: %s FAILED: %s", what, c.detail);
    return r;
  }
  // a link problem: the next tick tries again after 5 s, with the whole graph
  void linkLost(u32 now) {
    reset();
    c.closeFd();
    retryAt = now + 5000;
  }

  bool showManeuver(const Man &m, u32 now) {
    const Lanes *lp = m.lanes.n ? &m.lanes : 0;
    bool done = false;
    if (shown && inplaceOk && g.maneuver(m.dir, m.road, m.dist, m.unit, lp, all, b)) {
      int r = send("in-place maneuver", b);
      if (r == Client::kOk)
        done = true;
      else if (r == Client::kRejected) {
        log("CarPlayMenu: KOMBI: in-place update rejected: whole graphs from now on");
        inplaceOk = false;
      } else {
        linkLost(now);
        return false;
      }
    }
    if (!done) {
      if (!g.start(m.dir, m.road, m.dist, m.unit, lp, b)) {
        log("CarPlayMenu: KOMBI: internal error: unexpected template layout");
        failed = true;
        return false;
      }
      int r = send("route graph", b);
      if (r == Client::kRejected) {
        log("CarPlayMenu: KOMBI rejected the route graph: CarPlay arrows off until the HMI restarts");
        failed = true;
        return false;
      }
      if (r != Client::kOk) {
        linkLost(now);
        return false;
      }
    }
    shown = true;
    tGraph = now;
    posValid = false;
    lastDir = m.dir;
    lastUnit = m.unit;
    cpy(lastRoad, m.road, sizeof lastRoad);
    cpy(lastDist, m.dist, sizeof lastDist);
    lastLanes = m.lanes;
    if (m.lanes.n)
      feed(m, now, true);
    return true;
  }
  // the lane row's position feed: at most once a second and only when it moved, so the KOMBI keeps the row on
  void feed(const Man &m, u32 now, bool force) {
    if (!force && now - tPos < 1000)
      return;
    u32 pos;
    if (!g.feedPosition(m.hasStep, m.metres, pos))
      return;
    if (!force && posValid && pos == lastPos)
      return;
    if (!g.position(pos, b))
      return;
    if (send("position", b) != Client::kOk) {
      linkLost(now);
      return;
    }
    tPos = now;
    posValid = true;
    lastPos = pos;
  }
  void clear() {
    if (shown) {
      bool ok = c.cancel(g);
      clearFailed = !ok;
      log(ok ? "CarPlayMenu: KOMBI: cleared" : "CarPlayMenu: KOMBI: clear FAILED: %s", c.detail);
    }
    reset();
  }

  // one tick of the worker: have = the iPhone guides (m filled), else whatever is shown is cleared
  void tick(bool have, const Man &m, u32 now) {
    if (failed)
      return;
    if (!have) {
      if (shown)
        clear();
      return;
    }
    if (retryAt && (int)(now - retryAt) < 0)
      return;
    retryAt = 0;
    // A new street alone redraws the whole graph (the cluster blinks): only the maneuver's own street does, and not within 4 s of
    // the last graph; the car's current road (the maneuver names none) changes as it drives and redraws nothing.
    bool same = shown && m.dir == lastDir && (eq(m.road, lastRoad) || m.roadFb || now - tGraph < 4000);
    static Man mm;  // m with the lane row as it is to be shown (kLaneGapMs, two ticks)
    mm = m;
    if (same)
      steadyLanes(mm, now);
    else
      candValid = lanesGone = false;
    if (!same || (!sameLanes(mm.lanes, lastLanes) && now - tGraph >= 3000)) {
      if (showManeuver(mm, now) && g_debug && g_debug()) {
        char d[24];
        d[0] = '0' + (char)(m.dir / 10 % 10);
        d[1] = '0' + (char)(m.dir % 10);
        d[2] = 0;
        log("CarPlayMenu: KOMBI: maneuver direction %s, %s", d, m.dist);
      }
      return;
    }
    if (!eq(m.dist, lastDist) || m.unit != lastUnit) {
      if (!g.distance(m.dist, m.unit, b)) {
        log("CarPlayMenu: KOMBI: internal error: distance template");
        failed = true;
        return;
      }
      int r = send("distance", b);
      if (r == Client::kRejected) {
        failed = true;
        log("CarPlayMenu: KOMBI rejected a distance update: CarPlay arrows off until the HMI restarts");
        return;
      }
      if (r != Client::kOk) {
        linkLost(now);
        return;
      }
      cpy(lastDist, m.dist, sizeof lastDist);
      lastUnit = m.unit;
    }
    if (lastLanes.n)
      feed(m, now, false);
  }
  // the lane row to show for the maneuver already shown: the last one through a gap shorter than kLaneGapMs, a new one only when
  // seen on two ticks in a row (until then the last one)
  void steadyLanes(Man &m, u32 now) {
    if (!m.lanes.n) {
      candValid = false;
      if (!lastLanes.n)
        return;
      if (!lanesGone) {
        lanesGone = true;
        tLanesGone = now;
      }
      if (now - tLanesGone < kLaneGapMs)
        m.lanes = lastLanes;
      return;
    }
    lanesGone = false;
    if (sameLanes(m.lanes, lastLanes)) {
      candValid = false;
      return;
    }
    if (candValid && sameLanes(m.lanes, candLanes))
      return;  // seen twice: the new row goes out (at most every 3 s, tick)
    candLanes = m.lanes;
    candValid = true;
    m.lanes = lastLanes;
  }
};

// "a.b.c.d:port" -> an address (tests: RGHOOK_KOMBI, CPM_KOMBI_ADDR); false = keep the KOMBI's
inline bool parseAddr(const char *s, u8 *ip, u16 &port) {
  u32 v[5] = { 0, 0, 0, 0, 0 }, k = 0;
  for (; *s && k < 5; s++) {
    if (*s >= '0' && *s <= '9')
      v[k] = v[k] * 10 + (u32)(*s - '0');
    else if (*s == '.' || *s == ':')
      k++;
    else
      return false;
  }
  if (k != 4)
    return false;
  for (int i = 0; i < 4; i++)
    ip[i] = (u8)v[i];
  port = (u16)v[4];
  return true;
}

}  // namespace kombi
