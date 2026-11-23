# Changelog

## 1.4.6 (2026-11-21)

1.4.5 with the fix from the G30's rghook.log (ghackt issue #5): the reporter's car sends the directions to the Ethernet KOMBI, which
1.4.5's MOST fix does not reach.

### Fixed
- **Ethernet KOMBI (G-series): the arrow went off mid-route and came back only with the next maneuver** (ghackt issue #5, the
  reporter's rghook.log from a 2018 G30 530i). The iPhone's app kept switching the route state through the route: Route Set, then
  No Route Set, Loading, No Route Set, Loading ... (1, 0, 3, 0, 3, 0, 3, 1). The KOMBI keeps the arrow for 8 s of No Route Set, but
  counted those 8 s from the last Route Set, time in Loading included, so the arrow was cleared every few cycles ("guidance gap
  longer than 8 s: route ended", four times in the log). Now Loading / Locating within a route restarts the 8 s, and while in them
  the arrow is still updated from what the iPhone sends (distance, next maneuver; never a "0 m" from a message without a distance).
  Only Arrived, a No Route Set longer than 8 s, or switching the directions off clears it.
- **MOST HUD, the same app:** every No Route Set cleared the picture at once until the next maneuver. It now gets the same 8 s, as
  the KOMBI has had since 1.3.1.

### Changed files
- Rebuilt: `librghook-kombi.so`, `librghook-both.so`, `librghook-most.so`, `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`,
  `libcpprobe.so`. Installer 1.4.6. No new imports.
- Tests: `tests/test_rgkombi.py`: 12 s of 0 / 3 / 0 / 3 keeps the arrow, the distance in Loading still sent, no "0 m", Route Set
  again without a whole graph.

### Known limits
- Not tried in a car. The MOST HUD's 8 s grace is not covered by a test (the worker loop); the KOMBI's is.
- What the speed camera alert itself sends is not in the log (debug_log was off).

## 1.4.5 (2026-11-21)

1.4.4 with the fix for ghackt issue #5 (G30 HUD) on the MOST HUD.

### Fixed
- **HUD: guidance gone after a speed camera alert, and alerts missing with both HUD effects on** ([ghackt issue
  #5](https://github.com/ghackt/Carplay-advanced-installer/issues/5), 2018 G30 530i, NBT Evo ID6). The HUD already stepped aside
  while the BMW navigation draws a whole picture, but a warning drawn with only a few calls (a speed camera alert) went unnoticed:
  it hid the CarPlay picture, which came back only with the next maneuver, and the blink and fill effects kept painting over the
  warning. Now any drawing by another client while the CarPlay picture is up pauses ours (no effects, no distance updates) until
  that client has been quiet for 4 s, then the CarPlay picture is drawn again whole. A client that keeps drawing for more than 15 s
  is ignored for the rest of the session, as before, so the picture can never be kept away for good.

### Changed files
- Rebuilt: `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `librghook-most.so`, `librghook-both.so`, `libcpprobe.so` (all built with
  `hud.h`). Installer 1.4.5. No new imports.
- Tests:
  - `tests/test_hud_6wb_path.py`: a warning by another client covers the picture, which is drawn again 4 s after it; nothing is
    covered without a picture; a sender that never stops is ignored after 15 s.

### Known limits
- The speed camera fix for the MOST HUD (issue #5) is built from the report (no log yet). If the warning shows for longer than 4 s without being
  redrawn, CarPlay's picture comes back over it after 4 s. A log with *HUD debug log* on during an alert shows the warning's calls
  (`HUD seen ... fkt ...` lines).
- Not tried in a car.
- The installer's supported list is not involved: iDrive 6 and every HUD-only install do not check the HMI build.

## 1.4.4 (2026-11-20)

1.4.3 with Restart CarPlay moved and working, and the fixes below.

### Changed
- **Restart CarPlay** is now in the main *CarPlay Advanced Settings* menu **right under *Car and HUD type***, in the full and the
  HUD-only menu (in 1.4.3 it was the last row). A test build gave it a Media/Radio row of its own; in the car that row landed under
  NBT-AAD's *Open Android Auto* and restarted nothing. There is one Media/Radio entry only.
- **Restart CarPlay stops only mm-ipod**, CarPlay's session process (checked: it never targeted Android Auto). Not nbt-aad or
  mm-renderer, not CarPlay's acpd, mdnsd or mm-renderer, never Connectivity. It now finds mm-ipod from `/proc/<pid>/exefile` or,
  where that cannot be read, from `/proc/<pid>/as` the way nbt-aad does (devctl `DCMD_PROC_MAPDEBUG_BASE`, the values from its
  binary), and logs every step on a `Restart CarPlay:` line in `/dev/shmem/carplaymenu.log`: each mm-ipod found (pid, path, how),
  stopped, the new session, or why not (no session, a refused signal with its errno). Its thread is detached.
- **Restart CarPlay when the HMI may not stop mm-ipod.** In the car the button still restarted nothing. The HMI process (where the
  menu runs) is likely not allowed to signal mm-ipod (the 1.4.1 reboot from the HMI did nothing either). Now, when a signal is
  refused, or the HMI sees no mm-ipod at all, the menu leaves a request in RAM (`/dev/shmem/carplaymenu-cprestart`) and
  `librghook.so`, which runs inside mm-ipod while *CarPlay directions in HUD* is on, ends mm-ipod itself within 300 ms (a process may
  always signal itself); Connectivity then starts a new session. Only a request newer than that mm-ipod and less than 10 s old counts.
  It is logged in `/dev/shmem/carplaymenu.log` (`asked mm-ipod to end itself`, `mm-ipod ended itself`) and `/var/iap/rghook.log`
  (`restart: asked by the menu`).

### Fixed
- **Roundabouts drawn straight through on the HUD** ([ghackt issue #4](https://github.com/ghackt/Carplay-advanced-installer/issues/4):
  KakaoNavi showed the 9 or 2 o'clock exit). The roundabout picture turns by the exit angle the iPhone sends; an app that sends none
  (or 0) got the straight-through picture. Then the angle now comes from the maneuver's text when it names a clock direction, as
  Korean navigation apps do ("9시 방향" = 9 o'clock, also "2 o'clock"), or else, for "take exit n", from the n-th road of the
  junction going round (anticlockwise, clockwise in left-hand traffic). An angle the iPhone does send is used as before. Applies to
  the MOST HUD and the KOMBI.
- **Lag when swiping or changing pages in CarPlay.** The knob / touchpad hook in Connectivity (`libcppinch.so`) looked up the original
  touch function again for every touch event: a library lookup (`dlopen` + `dlsym`) for each move of a swipe on the touchscreen, and
  for each touchpad event. It now looks each one up once. Each knob rotation also opened two switch files on the disk; the switches of
  the input path are now read at most once a second, so a menu change still applies within a second.
- **mm-ipod's iAP2 link held up by our code:**
  - The iPhone's route guidance went to a file on the flash disk (`/var/iap/pps/routeguidance`), written by mm-ipod's iAP2 reader
    thread about once a second while guiding. A slow flash write held the iPhone's whole link. The file is now in RAM
    (`/dev/shmem/carplaymenu-routeguidance`), written in place as one fixed record, since `/dev/shmem` has no rename (as NBT-AAD
    found). The old file is removed, and an old route can no longer show after a reboot.
  - HUD-only install: the MOST HUD started on that same reader thread right after the iPhone authenticated (loading the DSI library,
    reading a dozen switch files, connecting to MOST), and its threads ran at that thread's priority. It now starts on a thread of
    its own at normal priority (10), as the KOMBI sender already did.
- **mm-ipod or Connectivity not starting because of a missing or broken file of ours:**
  - The wrappers that start mm-ipod and Connectivity with our libraries now start the process without the library if it is missing,
    and start the original binary if our copy of it is missing (before, CarPlay or Connectivity did not start).
  - The plugin's copy of mm-ipod / Connectivity is now written aside and renamed into place only when complete, and made again
    when its size is not the original's. A copy cut by a power cut, or one left from before a head unit software update, used to stay
    forever.

### Changed files
- Rebuilt: `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `libcppinch.so`, `librghook.so`,
  `librghook-most.so`, `librghook-kombi.so`, `librghook-both.so`, `libcpprobe.so` (its HUD reads the new file path),
  `CarPlayMenu.xml`, `CarPlayMenu-hud.xml`. Installer 1.4.4; its HUD-only
  mm-ipod wrapper as above; `tools/hud_sim.sh` writes the new file. `libcpreboot.so` and `restart.sh` are unchanged from 1.4.2.
- New libc imports in the plugin, all used by programs that run on the NBT Evo (nbt-aad, HmiMain3D): `devctl`, `lseek`, `rename`,
  `pthread_detach`, `__get_errno_ptr`.
- Tests:
  - `tests/test_cprestart.py`: a fake `/proc` with real processes. Only mm-ipod is found (from `exefile` and from `as`), SIGTERM,
    SIGKILL when SIGTERM is ignored, Connectivity untouched, the new session seen, no session; refused signals: mm-ipod ends itself
    on the request, or is reported when it has no librghook.
  - `tests/test_rghook_restart.py`: which requests end an mm-ipod (newer than its start, under 10 s, the clock's wrap).
  - `tests/test_pinch_paths.py`: the hook against a stand-in `libconn_carplay.so`. 300 touch events make 1 library lookup (300 in
    1.4.3), and 200 knob rotations read the switches once.
  - `tests/test_wrappers.py`: the three wrappers run with each file present and missing; the atomic copy.
  - `tests/test_menu_car_type.py`: one Media/Radio entry, Restart CarPlay under Car and HUD type in both menus.
  - `tests/test_hud_6wb_path.py`: the HUD reads the padded record as before.
  - `tests/test_rghook_roundabout.py`: the exit angle from the clock direction (Korean and English) and from the exit number, the
    iPhone's own angle kept, other pictures untouched.

### Known limits
- The restart from inside mm-ipod needs *CarPlay directions in HUD* on (librghook is in mm-ipod only then).
- Not tried in a car. That Connectivity starts a new session by itself while the iPhone stays connected is expected from its
  starting mm-ipod for every session, but not confirmed. If the log says `no new CarPlay session within 30 s`, unplug and reconnect
  the iPhone.
- The roundabout fix is built from the report's description (no log yet): if KakaoNavi words its roundabouts differently, the HUD
  still draws them straight. With the switch file `debug_log` in `/var/opt/hmi/data/rw/carplaymenu/`, `/var/iap/rghook.log` shows
  the text it sends (the `RG 0x5202` lines).
- The swipe-lag fix covers what our code added to each touch. With *Full-screen CarPlay* on, the iPhone also renders a bigger
  picture (1184 to 1440 wide), which this release does not change.

## 1.4.3 (2026-11-20)

### Changed
- **Restart head unit is now Restart CarPlay**, and it restarts CarPlay instead of the head unit. It is NBT-AAD's restart routine,
  taken from the nbt-aad program (`nbt::mmr::Launcher::stop` / `findProcess`), aimed at CarPlay instead of NBT-AAD:
  - **find:** CarPlay's session process **mm-ipod** is found the way nbt-aad finds its processes, by scanning `/proc` and matching
    the end of each `/proc/<pid>/exefile` path. That covers the system's mm-ipod and the wrapper's copy.
  - **stop:** SIGTERM, then up to 10 checks 50 ms apart; if it is still there, SIGKILL, then up to 10 checks 20 ms apart.
  - **new session:** Connectivity, which starts mm-ipod for every CarPlay session, starts a fresh one. The plugin watches up to 30 s
    for it and logs it.
  - **never Connectivity:** stopping it makes the head unit's watchdog reset the unit a minute later.
  - **runs in a thread of its own**, so the menu answers at once.

  It applies everything mm-ipod reads when it starts. In the HUD-only install that is the whole HUD: Car and HUD type, HUD effects,
  effect colour and the debug log, with no reboot. The full menu keeps *Restart to apply* to restart the head unit, with 1.4.2's ECU
  reset first. Every step is in `/dev/shmem/carplaymenu.log` (`Restart CarPlay: ...`).
- **HUD effects:** *Effect colour >* is now the first row, above *Fill toward the turn* and *Blink near the turn*, in both menus.

### Changed files
- New: `src/cprestart.h`. Rebuilt: `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `CarPlayMenu.xml`, `CarPlayMenu-hud.xml`,
  `CarPlayMenu_ALL_Texts.zip` (text 32 "Restart CarPlay"). `libcpreboot.so` and `restart.sh` are unchanged from 1.4.2.
- Tests: `tests/test_cprestart.py` (a fake `/proc` with real processes: only mm-ipod found, SIGTERM, SIGKILL when SIGTERM is
  ignored, Connectivity untouched, the new session seen, no session); the main menu and HUD effects order in
  `tests/test_menu_car_type.py`.

### Known limits
- In the car, this build's Restart CarPlay did nothing; see 1.4.4.
- Not tried in a car. That Connectivity starts a new session by itself while the iPhone stays connected is expected from its
  starting mm-ipod for every session, but not confirmed. If the log says `no new CarPlay session within 30 s`, unplug and reconnect
  the iPhone.

## 1.4.2 (2026-11-20)

### Changed
- **Car and HUD type** is now in the main *CarPlay Advanced Settings* menu, right after *HUD effects >*, in the full and the HUD-only
  menu (it was under HUD effects).
- **Restart head unit** is now the last row of the main menu, in both menus (it was at the end of Car and HUD type).

### Fixed
- **The head unit did not restart from the Restart head unit button.** 1.4.1 called the reboot (`sysmgr_reboot`) from a process
  the HMI started. That is not the installer's root shell, and the unit did not restart or acknowledge anything. The button now sends
  the restart the phone app CarPlay Advanced Installer 1.6.7 uses (taken from the app: `HeadUnitReset` / `HsfzCodec`). That is a
  diagnostic **ECU reset, UDS `11 01`, to the head unit (ECU 0x63) from tester 0xF4 in a BMW HSFZ frame on TCP 6801**. As in the
  app, the head unit must first identify itself (ReadDataByIdentifier 0xF187 or 0xF190). The reset is sent once and never repeated;
  `51 01` is the head unit's acknowledgement, and "response pending" (`7F 11 78`) is waited out. From inside the car it goes to the
  head unit's own HSFZ port: 127.0.0.1 first, then the unit's other IPv4 addresses. If there is no acknowledgement, or the unit is
  still running 15 s later, the old reboot call follows as a fallback. Every step goes to `/dev/shmem/carplaymenu.log`
  (`ECU reset: ...`).
- **Restart to apply** (full menu, `restart.sh`) sends the same ECU reset first.
- The installer's own restart (`--restart-hmi`, `restart`) is unchanged: `libcpreboot.so` sends the ECU reset only when the menu asks
  for it (`CPREBOOT_HSFZ=1`).

### Changed files
- Rebuilt: `libcpreboot.so`, `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `CarPlayMenu.xml`, `CarPlayMenu-hud.xml`; `restart.sh`
  edited.
- Tests: `tests/test_reboot_hsfz.py` (`reboot.c` built for the computer against a fake HSFZ server: acknowledged, second DID,
  no identity, refused, sent without an answer, no port, without the switch) and the main-menu layout in
  `tests/test_menu_car_type.py`.

### Known limits
- Not tried in a car. That the head unit's HSFZ port answers from inside the unit (127.0.0.1 or its own address) is expected from the
  app reaching it at the unit's address, but not confirmed. If the log shows `no HSFZ port with ECU 0x63 on this unit`, the old reboot
  call is what remains.

## 1.4.1 (2026-11-20)

### Added
- **Car and HUD type > Restart head unit.** A button at the end of the Car and HUD type submenu, in the full and the HUD-only
  menu, that reboots the head unit when a car type choice has not taken by itself. With the full install it does what *Restart to
  apply* does (`restart.sh`). The HUD-only install has no `restart.sh`, so there it starts the same reboot directly
  (`libcpreboot.so`: flush, then `sysmgr_reboot`). The boot-loop guard counts it as an asked-for restart, not a failed boot.

### Changed
- Rebuilt: `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `CarPlayMenu.xml`, `CarPlayMenu-hud.xml`, `CarPlayMenu_ALL_Texts.zip`
  (text 32 "Restart head unit").
- Tests: `test_restart_head_unit_row` in `tests/test_menu_car_type.py`.

### Known limits
- Not tried in a car. Like *Restart to apply*, it reboots at once, with no confirmation.

## 1.4.0 (2026-11-20)

1.3.11 plus the car type setting below. (The 1.3.12 made earlier from the 1.3.1-6wb merge was withdrawn and is not part of this.)

### Added
- **HUD effects > Car and HUD type.** A submenu, in the full and the HUD-only menu, to say which cluster and HUD the car has instead
  of letting the plugin guess:
  - *Automatic* (default): detection as before.
  - *BMW 6WA cluster* (also an analog cluster with a windscreen HUD, as in issue #27): the BMW picture box even on a unit that
    carries the MINI layout package, and the 6WA layout.
  - *BMW 6WB cluster*: the 6WB layout without asking the cluster, and no copies to a windscreen HUD.
  - *BMW 6WB cluster + HUD* (issues #2, #25, #26): the 6WB layout, drawn on the cluster (82/00) and copied to the windscreen
    HUD (82/02).
  - *MINI*: the MINI picture box and the 6WA layout.

  The menu writes the switch file `hud_car` (`auto`, `6wa`, `6wb`, `6wb_hud`, `mini`). It applies after a restart with the full
  menu, and at the next CarPlay connection with the HUD-only menu. `hud_layout`, `hud_box` / `hud_box_6wa` and `hud_inst` still
  decide over it. A measured picture buffer still fits the box as before. No effect on an Ethernet KOMBI (G-series, J29).

### Changed
- `hud-probe` lists `hud_car` and the plugin's car type line.
- Switch words may contain `_` (and `-` is read as `_`).
- Rebuilt: `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `librghook-most.so`, `librghook-both.so`, `libcpprobe.so`,
  `CarPlayMenu.xml`, `CarPlayMenu-hud.xml`, `CarPlayMenu_ALL_Texts.zip`.
- Tests: `tests/test_menu_car_type.py` (both menus, the plugin's table, texts, hud.h's words) and the car types in
  `test_hud_6wb_path.py`.

### Known limits
- Not tried in a car. The menu texts are English in every language, like the rest of the menu.

## 1.3.11 (2026-11-20)

From st1ng issue #27 (`ID5_1903-490-1837J Build 41`, Korea, HUD-only install with 1.3.0: the CarPlay directions show in the
factory HUD, but no street or road name). The F10 of issue #25 also logged `no street from the iPhone (maneuver_road and
current_road empty)`.

### Fixed
- **Empty street line when the iPhone sends no road name.** The street line now falls back from the road after the maneuver, to
  the current road, to the iPhone's maneuver description. librghook already received that description but the HUD never used it.
  The log says which one it used. Switch `hud_street` = `road` keeps road names only, as before.
- **Hangul.** Korean road names go as before (UTF-16 on 6WA/MINI; on a 6WB, the 1.3.9 coding). If the cluster or HUD refuses a
  text with Hangul, the texts go again with Hangul romanized (Revised Romanization, each word capitalised: 강남대로 becomes
  Gangnamdaero). On a 6WB that means ISO-8859-15. Switch `hud_text_enc` = `roman` always romanizes, for a HUD that accepts Hangul
  but draws nothing.

### Changed
- Rebuilt: `librghook-most.so`, `librghook-both.so`, `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `libcpprobe.so`.
- `src/hud.h`: the guidance file's path is a build macro (`HUD_PPS`, same default) so tests can feed it.
- Tests (`test_hud_6wb_path.py`): street fallback order, `hud_street road`, romanization, refused Hangul then romanized (6WA and
  6WB), accepted Hangul unchanged.

### Known limits
- The cause in issue #27 isn't confirmed without that car's `/var/iap/rghook.log` (HUD debug log on, during a route on named
  roads). Both reports were short test drives (a car park, 200 m), where Apple Maps often has no road name.
- That car's HmiMain3D (`233ac909…`) is a second 1903 Build 41 variant, not the one in `supported_table` (`027cf939…`). The
  HUD-only install doesn't need it; a full install there would go through the HMI scan.

## 1.3.10 (2026-11-20)

From st1ng issue #25 (BMW F10/F11, `ID5_1807-490-1809H Build 29`). With the phone app's Update28 (1.3.8) the reporter saw the
directions in the cluster and in the windscreen HUD: 1.3.8's 6WB and HUD fixes work in that car. The app still said "This software
is not in the supported list" and installed HUD-only.

### Why the full install still isn't offered
- ghackt's phone app (Carplay-advanced-installer) has its own built-in list of supported HmiMain3D / libconn_carplay.so hashes and
  chooses `--mode hud` or `--mode full --hmi <build>` before it runs this script. 1807 is not in that list yet, so adding it to
  this script's `supported_table` (1.3.7) does not change what the app chooses. The script itself accepts
  `install --mode full --kind id5 --hmi ID5_1807-490-1809H_Build_29`.
- The app needs a new entry: HmiMain3D `5e74cde50c01dbf9c104188fadf50c6f8642c5d0715038d150d85dda4e2112c7`, build
  `ID5_1807-490-1809H_Build_29`, kind `id5`, full mode. Its libconn_carplay.so hash has not been reported. If the app also
  requires that hash, it has to come from the car (`sh carplay-advanced.sh status`).

### Changed (installer only, the plugin libraries are unchanged from 1.3.9)
- A HUD-only install on a build that is in `supported_table` now says so in its log, with the command for the full install, so
  the reason shows in an app's log.
- A HUD-only install on iDrive 5 no longer ends with "Fullscreen, knob tilts and the touchpad are native on iDrive 6". It now says
  they need the full install. The iDrive 6 wording is unchanged.
- Tests: `test_hud_only_note_for_a_supported_build`, `test_hud_only_end_message_by_kind`.

## 1.3.9 (2026-11-20)

From issue #26 (BMW with a 6WB cluster and a windscreen HUD, `ID5_2007-510-2013A Build 26`, `hud_inst` "0 2"): Chinese street
names showed as question marks, and the HUD cut the arrow off.

### Fixed
- **Chinese (and other non-Latin) street names on a 6WB showed as "?????"** (`setText`). Up to 1.3.8 the plugin itself replaced
  every character outside ISO-8859-15 with `?` on a 6WB. Now Latin text keeps ISO-8859-15, and any other text goes as UTF-16 (MOST
  coding 00, as on the 6WA and MINI). If the cluster refuses that, it tries UTF-8 and then Latin only (the rest left out, never
  `?`), drawing the texts again each time. Switch `hud_text_enc` = `utf16`, `utf8` or `latin` fixes one coding.
- **The windscreen HUD cut the arrow off** (`box2Points`). The HUD on 82/02 showed only the top of the copied picture, and a
  straight line cut off the arrowhead at about y 90 of the plane, while the cluster showed it whole. The copies' picture now goes into
  its own box: switch `hud_box_2` (`x0 y0 x1 y1` in plane coordinates, or `full` for unchanged); else the box fitted to 82/02's
  picture buffer if it answers a size smaller than 221 x 158; else (88,12)-(144,82), above the cut. The cluster's own picture is
  unchanged.
- The plugin build had no `strncmp`; the switch words are compared by a small helper instead.

### Changed
- Rebuilt: `librghook-most.so`, `librghook-both.so`, `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `libcpprobe.so`.
- Tests (`test_hud_6wb_path.py`): codings and fallbacks (Latin, Chinese as UTF-16, refused UTF-16 / UTF-8, fixed by switch, 6WA
  unchanged), and the copies' box (default, fitted to a smaller 82/02, full-plane answer, `hud_box_2` both forms).

### Known limits
- Whether the 6WB's font has Chinese characters is not known. If names show as boxes or nothing, set `hud_text_enc` to `latin`.
- The HUD box is placed from one photo. If the arrow is still cut or too small, set `hud_box_2` (for example `80 10 152 100`) and
  report what shows. Both changes are untried in a car.

## 1.3.8 (2026-11-20)

From the HUD debug log of a BMW F10 with a 6WB cluster and a windscreen HUD (NBT Evo retrofit, `ID5_1807-490-1809H Build 29`,
HUD-only install, MOST; ghackt/Carplay-advanced-installer issue #2).

### Fixed
- **6WB cluster taken for a 6WA** (`detectLayout`, `layoutRetry`). This cluster answers ImageGetSize for the 6WB plane 0x1F
  (221 x 158) and also for handle 5 (126 x 126). 1.3.6 chose 6WB only when 5 did not answer, so it drew on the 6WA handles 1..6:
  the cluster accepted every call and showed nothing. Now 0x1F answering 221 x 158 means 6WB, whether 5 answers or not. The log
  also prints 0x1F's size (`layout probe: 0x1f is 221 x 158`). Confirmed in that car by forcing it with `hud_layout` = `6wb`.
- **Nothing sent to the windscreen HUD on a 6WB** (`mirror6wb`). Both HUDTerminals were on the cluster (82/00 and 82/02 at device
  0102), but copies to 82/02 only started after the BMW navigation was seen setting it up. A HUD-only install starts after that
  set-up, so it logged `copies 0`. Now, on a 6WB with 82/02 in the registry, every picture and text also goes to 82/02 with the
  same 6WB handles. 82/02's own handles replace them once its set-up is seen. `hud_inst` = `0` turns the copies off. A 6WA or a MINI
  is unchanged, because there 82/02 is the cluster's own terminal.

### Changed
- Rebuilt: `librghook-most.so`, `librghook-both.so`, `libCarPlayMenu.so`, `libCarPlayMenu-hud.so`, `libcpprobe.so`.
- Tests: `test_hud_6wb_path.py` has a fake F10 cluster that answers both sizes, plus checks that copies go to 82/02 (same handles,
  the size question kept on 82/00), that `hud_layout` starts them too, and that none are sent without 82/02, with `hud_inst` `0`,
  or on a 6WA.

### Known limits
- Whether 82/02 is this car's windscreen HUD, and whether it takes the 6WB handles, is untried. If it stays dark, run the
  installer's `hud-probe` (sizes 0x01..0x40 on both terminals, `6wb-hud` / `6wa-hud` test pictures) and send `/var/iap/rghook.log`.

## 1.3.7 (2026-11-20)

### Added
- **Support for HMI `NBTevo_ID5_1807_Release ID5_1807-490-1809H Build 29`** (BMW F11 NBT Evo retrofit, 6WB cluster with HUD;
  ghackt/Carplay-advanced-installer issue #2). The full install now offers every option on this build: Full-screen CarPlay, knob
  tilts, touchpad (tap as click, two-finger back), rotation as zoom and the HUD directions. Before, it got only the HUD-only install.
  - Its HMI is 006_017_006's code moved by 0x2f28: handleTouchInput 0x43ef54, controller send 0x1fd9fe4, track send 0x1fd9f44,
    proxies +0x4e60 / +0x52b0, CTouchData +4 / +8 / +0x14 / +0x18.
  - Knob tilts: `src/tilt_1807.json` (19 words). As in 1903, "pressed" is kept in r7, so the two words that store it use r7.
  - Full-screen CarPlay: `kFsCode1807`, the seven CARPLAY_FULLSCREEN (variant 0x9c71) reads forced on, as in 006.
  - The installer's HMI scan finds all of it, flag code included, and its output matches the built-in table (`scan` row).
- `supported_table`: HmiMain3D `5e74cde50c01dbf9c104188fadf50c6f8642c5d0715038d150d85dda4e2112c7` and its scan hash.
  `install --full` installs without asking on this build.
- Tests: `test_kbuilds_1807`, `test_installer_lists_1807`, and, with `CPA_HMI_1807` set to the stock HmiMain3D, `Build1807`
  (every word the plugin compares before it writes) and `HmiScan1807Test` (the scan gives exactly the built-in values).

### Changed
- `plugin/libCarPlayMenu.so` and `plugin/libCarPlayMenu-hud.so` rebuilt with the new build entry. The other files in `plugin/`
  are unchanged.
- `hud_reports`: the 1807 line notes that it has been a known build since 1.3.7.
- README and AGENTS.md: the supported-software tables list 1807-490 Build 29.

### Known limits
- This build's `libconn_carplay.so` hash has not been reported, so its entry is matched by HmiMain3D alone. Run
  `sh carplay-advanced.sh status` on the car and report the hash to add the row.
- Its layouts and `carplay.ini` have not been seen, so it uses 006's settings (`pbw 1, pbwBmw 1, ini 1`). If the layout does not
  match, the plugin logs that and changes nothing.
- Not tried in a car in full mode yet.
- The blank HUD from issue #2 is not addressed by this release. Needed: `/var/iap/rghook.log` with "HUD debug log" on, after a
  CarPlay route.
