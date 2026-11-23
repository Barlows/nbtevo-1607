// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// cprestart.h — "Restart CarPlay" (menu action 144, 1.4.3): ends the running CarPlay session so that Connectivity starts a fresh one.
//
// The restart is NBT-AAD's (nbt-aad, nbt::mmr::Launcher::stop / findProcess / exePath / exePathIs*, read from its binary): processes
// are found by scanning /proc and taking each one's executable path, the path compared by its end (a trailing line end or blank
// ignored); one is stopped with SIGTERM, up to 10 checks 50 ms apart, then SIGKILL, up to 10 checks 20 ms apart. The executable path
// is nbt-aad's exePath: /proc/<pid>/exefile, else the path pidin shows, from /proc/<pid>/as with devctl DCMD_PROC_MAPDEBUG_BASE
// (0x40100803, a procfs_debuginfo with PATH_MAX after it, the path at +8: the values in nbt-aad's binary; its UsbDirect finds the
// head unit's devc-serusb-aoap that way only). NBT-AAD uses it on its own processes (mm-renderer); here it is aimed at CarPlay's
// session process instead: **mm-ipod** (the iAP2 stack), which Connectivity starts for every CarPlay session (AGENTS.md
// "Processes"). Its path ends in /mm-ipod both as the system's /opt/conn/bin/carplay/mm-ipod and as the copy the route-guidance
// wrapper runs (carplaymenu/bin/mm-ipod, same name). Nothing else is touched: not NBT-AAD's nbt-aad or mm-renderer (Android Auto),
// not CarPlay's acpd, mdnsd or mm-renderer (HMood's CONN_CARPLAY package), never Connectivity (stopping it leaves its watchdog
// registration and the unit resets a minute later). Afterwards it watches up to kWaitS seconds for the new mm-ipod and logs it.
// What a CarPlay restart applies: everything mm-ipod reads at its start (the HUD-only install's whole HUD: CarPlay directions,
// Car and HUD type, HUD effects, effect colour, debug log; librghook's switches). Not what the HMI or Connectivity read at theirs
// (the full menu's options: Restart to apply).
// 1.4.4: when the HMI may not signal mm-ipod (EPERM) or sees none, the plugin also asks mm-ipod to end itself: RG_RESTART_REQ
// ("CPR1 <ms>", CLOCK_MONOTONIC), which librghook (rghook.c restart_watch, inside mm-ipod while "CarPlay directions in HUD" is on)
// reads every 300 ms; a process may always signal itself. (1.4.4 also: the as path, the errno lines, the detached thread.)
// Every step goes to the plugin log with the errno of a refused signal. Runs in a thread of its own (the HMI's handler returns at
// once; the caller detaches it); one at a time.
// Host tests (tests/test_cprestart.py): CPR_HOST, with CPR_PROC naming a fake /proc whose <pid>/exefile files name real processes;
// a <pid>/as file there stands for the devctl answer (8 bytes, then the path).
#pragma once

#ifdef CPR_HOST
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#else
extern "C" {
void *opendir(const char *);
void *readdir(void *);
int closedir(void *);
long strtol(const char *, char **, int);
int kill(int, int);
int usleep(unsigned);
int open(const char *, int, ...);
long read(int, void *, unsigned long);
int close(int);
int snprintf(char *, unsigned long, const char *, ...);
int devctl(int, int, void *, unsigned long, int *);
int *__get_errno_ptr(void);
int pthread_detach(int);
}
#endif

#ifndef CPR_PROC
#define CPR_PROC "/proc"
#endif
#ifndef CPR_REQ
#define CPR_REQ "/dev/shmem/carplaymenu-cprestart"  // rghook.c RG_RESTART_REQ
#endif

namespace cpr {

#ifdef CPR_HOST
typedef DIR Dir;
inline int lastErrno() {
  return errno;
}
inline unsigned nowMs() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
inline int sendSignal(int pid, int sig) {  // CPR_FAKE_EPERM: as if the HMI might not signal it
  if (getenv("CPR_FAKE_EPERM") && sig != 0) {
    errno = EPERM;
    return -1;
  }
  return kill(pid, sig);
}
// the devctl answer of the fake /proc: the <pid>/as file's bytes (nbt-aad's command and size checked, as procfs would)
inline int procDevctl(int fd, int cmd, void *buf, unsigned long n, int *) {
  if (cmd != 0x40100803 || n != 1040)
    return 22;
  memset(buf, 0, n);
  return read(fd, buf, n - 1) > 8 ? 0 : 22;
}
#else
typedef void Dir;
inline int lastErrno() {
  return *__get_errno_ptr();
}
inline unsigned nowMs() {
  return hud::nowMs();  // CLOCK_MONOTONIC (hud.h, included before)
}
inline int sendSignal(int pid, int sig) {
  return kill(pid, sig);
}
inline int procDevctl(int fd, int cmd, void *buf, unsigned long n, int *info) {
  return devctl(fd, cmd, buf, n, info);
}
#endif
void (*g_log)(const char *) = 0;
#ifndef CPR_WAIT_S
#define CPR_WAIT_S 30
#endif
const unsigned kWaitS = CPR_WAIT_S;            // how long the new session is watched for (seconds)
const int kMapDebugBase = 0x40100803;          // DCMD_PROC_MAPDEBUG_BASE (nbt-aad: __DIOF(_DCMD_PROC, 3, procfs_debuginfo))
const unsigned long kMapDebugSize = 16 + 1024; // procfs_debuginfo (u64 vaddr, path) + PATH_MAX, as nbt-aad asks
volatile int g_busy;

inline void say(const char *m) {
  if (g_log)
    g_log(m);
}

// the name of a /proc entry: QNX's struct dirent pads d_ino and d_offset to 64 bits (d_ino, d_ino_hi, d_offset, d_offset_hi,
// d_reclen, d_namelen, d_name), so d_name is at 20 whichever readdir: where nbt-aad reads it on the NBT Evo
inline const char *entryName(void *e) {
#ifdef CPR_HOST
  return ((struct dirent *)e)->d_name;
#else
  return (const char *)e + 20;
#endif
}

// nbt-aad's exePathIs*: the path without a trailing line end or blank, compared by its end
inline bool exeIsMmIpod(const char *p) {
  static const char kEnd[] = "/mm-ipod";
  unsigned n = 0;
  while (p[n])
    n++;
  while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
    n--;
  const unsigned k = sizeof kEnd - 1;
  if (n < k)
    return false;
  for (unsigned i = 0; i < k; i++)
    if (p[n - k + i] != kEnd[i])
      return false;
  return true;
}

// nbt-aad's exePath: the executable path of pid into exe (cap bytes). 1 = from /proc/<pid>/exefile, 2 = from /proc/<pid>/as
// (DCMD_PROC_MAPDEBUG_BASE), 0 = no such process (or neither can be read)
inline int exePath(int pid, char *exe, unsigned cap) {
  char path[64];
  snprintf(path, sizeof path, "%s/%d/exefile", CPR_PROC, pid);
  int fd = open(path, 0);
  if (fd >= 0) {
    long n = read(fd, exe, cap - 1);
    close(fd);
    if (n > 0) {
      exe[n] = 0;
      return 1;
    }
  }
  snprintf(path, sizeof path, "%s/%d/as", CPR_PROC, pid);
  fd = open(path, 0);
  if (fd < 0)
    return 0;
  static char map[kMapDebugSize];  // the restart thread only (one at a time)
  int r = procDevctl(fd, kMapDebugBase, map, sizeof map, 0);
  close(fd);
  if (r != 0)
    return 0;
  map[sizeof map - 1] = 0;
  unsigned k = 0;
  while (map[8 + k] && k + 1 < cap) {
    exe[k] = map[8 + k];
    k++;
  }
  exe[k] = 0;
  return k ? 2 : 0;
}

// is pid (still) mm-ipod (nbt-aad's pidIsRenderer, for mm-ipod); how = exePath's answer
inline bool pidIsMmIpod(int pid, int *how = 0, char *exe = 0, unsigned cap = 0) {
  char own[1024];
  if (!exe) {
    exe = own;
    cap = sizeof own;
  }
  int h = exePath(pid, exe, cap);
  if (how)
    *how = h;
  if (!h)
    return false;
#ifdef CPR_HOST
  if (kill(pid, 0) != 0)  // the host's fake /proc keeps the file: the process itself decides
    return false;
#endif
  return exeIsMmIpod(exe);
}

// every running mm-ipod (nbt-aad's findProcess: the numeric /proc entries, their executable path); logged with how it was found
// when log is set
inline int findMmIpod(int *pids, int max, bool log = false) {
  Dir *d = (Dir *)opendir(CPR_PROC);
  if (!d) {
    if (log) {
      char m[96];
      snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: %s not readable (errno %d)", CPR_PROC, lastErrno());
      say(m);
    }
    return 0;
  }
  int n = 0;
  while (n < max) {
    void *e = readdir(d);
    if (!e)
      break;
    char *end = 0;
    long pid = strtol(entryName(e), &end, 10);
    if (pid <= 1 || !end || *end)  // not procnto (1), as nbt-aad's UsbDirect
      continue;
    char exe[256];
    int how = 0;
    if (pidIsMmIpod((int)pid, &how, exe, sizeof exe)) {
      pids[n++] = (int)pid;
      if (log) {
        unsigned k = 0;
        while (exe[k] && exe[k] != '\n' && exe[k] != '\r')
          k++;
        exe[k] = 0;
        char m[360];
        snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: found mm-ipod pid %d: %s (%s)", (int)pid, exe,
                 how == 1 ? "exefile" : "as");
        say(m);
      }
    }
  }
  closedir(d);
  return n;
}

// one signal, its refusal logged with errno (1 = EPERM: not ours to signal; 3 = ESRCH: already gone). 0 sent, 1 gone, -1 refused
inline int signalPid(int pid, int sig) {
  if (sendSignal(pid, sig) == 0)
    return 0;
  int e = lastErrno();
  if (e == 3)
    return 1;
  char m[140];
  snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: %s to mm-ipod pid %d refused (errno %d%s)", sig == 9 ? "SIGKILL" : "SIGTERM",
           pid, e, e == 1 ? ", EPERM: the HMI may not signal it" : "");
  say(m);
  return -1;
}

// nbt-aad's Launcher::stop: SIGTERM, 10 checks 50 ms apart; still there: SIGKILL, 10 checks 20 ms apart. 0 stopped by SIGTERM,
// 1 by SIGKILL, 2 still there, 3 the signals were refused
inline int stopPid(int pid) {
  int s = signalPid(pid, 15);
  if (s == 1)
    return 0;
  for (int i = 0; s == 0 && i < 10; i++) {
    usleep(50000);
    if (!pidIsMmIpod(pid))
      return 0;
  }
  if (s == 0) {
    char m[120];
    snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: mm-ipod pid %d ignored SIGTERM: SIGKILL", pid);
    say(m);
  }
  int k = signalPid(pid, 9);
  if (k == 1)
    return 1;
  if (k < 0)
    return s < 0 ? 3 : 2;
  for (int i = 0; i < 10; i++) {
    usleep(20000);
    if (!pidIsMmIpod(pid))
      return 1;
  }
  return 2;
}

// the request to mm-ipod itself (librghook's restart_watch): 0 written
inline int requestSelfExit() {
  char r[32];
  int k = snprintf(r, sizeof r, "CPR1 %u\n", nowMs());
#ifdef CPR_HOST
  int fd = open(CPR_REQ, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#else
  int fd = open(CPR_REQ, 0x301 /* O_WRONLY | O_CREAT | O_TRUNC (QNX) */, 0666);
#endif
  if (fd < 0)
    return -1;
  long w = write(fd, r, (unsigned long)k);
  close(fd);
  return w == k ? 0 : -1;
}
// waits up to ms for every pid in old to be gone; 1 = all gone
inline int waitGone(const int *old, int n, unsigned ms) {
  for (unsigned t = 0; t <= ms; t += 100) {
    int left = 0;
    for (int i = 0; i < n; i++)
      left += pidIsMmIpod(old[i]);
    if (!left)
      return 1;
    usleep(100000);
  }
  return 0;
}

inline void *restartThread(void *) {
  int old[8];
  int n = findMmIpod(old, 8, true);
  char m[160];
  if (!n) {
    // none visible: none running, or this process may not read the others' paths; mm-ipod with librghook ends itself if it runs
    if (requestSelfExit() == 0)
      say("CarPlayMenu: Restart CarPlay: no mm-ipod visible from the HMI: asked a running mm-ipod to end itself (librghook)");
    else
      say("CarPlayMenu: Restart CarPlay: no CarPlay session running (no mm-ipod): connect the iPhone");
    g_busy = 0;
    return 0;
  }
  int stopped = 0, refused = 0;
  for (int i = 0; i < n; i++) {
    snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: stopping mm-ipod pid %d (SIGTERM)", old[i]);
    say(m);
    int r = stopPid(old[i]);
    snprintf(m, sizeof m,
             r == 3   ? "CarPlayMenu: Restart CarPlay: mm-ipod pid %d not stopped (signals refused)"
             : r == 2 ? "CarPlayMenu: Restart CarPlay: mm-ipod pid %d still listed after SIGKILL"
                      : "CarPlayMenu: Restart CarPlay: mm-ipod pid %d stopped",
             old[i]);
    say(m);
    stopped += r < 2;
    refused += r >= 2;
  }
  if (refused) {  // the HMI may not stop it: mm-ipod's own librghook does
    if (requestSelfExit() != 0) {
      say("CarPlayMenu: Restart CarPlay: request file not written: CarPlay not restarted");
      g_busy = 0;
      return 0;
    }
    say("CarPlayMenu: Restart CarPlay: asked mm-ipod to end itself (librghook, CarPlay directions in HUD)");
    if (waitGone(old, n, 3000)) {
      say("CarPlayMenu: Restart CarPlay: mm-ipod ended itself");
      stopped = n;
    } else
      say("CarPlayMenu: Restart CarPlay: mm-ipod still running: librghook not in it (CarPlay directions in HUD off?), not restarted");
  }
  if (!stopped) {
    say("CarPlayMenu: Restart CarPlay: CarPlay not restarted (no mm-ipod could be stopped)");
    g_busy = 0;
    return 0;
  }
  for (unsigned t = 0; t < kWaitS * 2; t++) {  // Connectivity's new session
    usleep(500000);
    int now[8];
    int k = findMmIpod(now, 8);
    for (int i = 0; i < k; i++) {
      bool fresh = true;
      for (int j = 0; j < n; j++)
        if (now[i] == old[j])
          fresh = false;
      if (fresh) {
        snprintf(m, sizeof m, "CarPlayMenu: Restart CarPlay: CarPlay session started again (mm-ipod pid %d, after %u.%u s)", now[i],
                 (t + 1) / 2, (t + 1) & 1 ? 5u : 0u);
        say(m);
        g_busy = 0;
        return 0;
      }
    }
  }
  snprintf(m, sizeof m,
           "CarPlayMenu: Restart CarPlay: no new CarPlay session within %u s: unplug and reconnect the iPhone (or restart the head unit)",
           kWaitS);
  say(m);
  g_busy = 0;
  return 0;
}

}  // namespace cpr
