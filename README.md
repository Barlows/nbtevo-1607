> ## Personal build for one car - not a general release
>
> This repository is a fork of [nbtevo-carplay-advanced](https://github.com/st1ng/nbtevo-carplay-advanced) kept for
> **one specific car**: a BMW F20 whose head unit reports **`NBTevo_ID5_1607_Release ID5_1607-490-1610T Build 153`**
> (iDrive 5, 8.8" 1280 x 480), with CarPlay active.
>
> It combines ghackt's 1.4.6 tree, the 1607 full-screen layout patch from
> [PR #18](https://github.com/st1ng/nbtevo-carplay-advanced/pull/18), and payloads for a personal Android installer
> that updates NBT-AAD as well.
>
> The releases here exist **only** to make installing on that one car repeatable. They are not tested on any other
> head unit, not a supported product, and not affiliated with the upstream authors. **For any other car use the
> upstream project**, and [NBT-AAD](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) for Android Auto.
>
> Installing modifies the head unit. Do it at your own risk, on a car you own.

# nbtevo-carplay-advanced

Apple CarPlay add-ons for the BMW / MINI **NBT Evo** head unit (iDrive 5 and iDrive 6): a **CarPlay Advanced Settings** menu in
Media/Radio, and the iPhone's **turn-by-turn directions in the cluster and the head-up display**.

It is an HMI plugin plus a few small libraries that hook into the head unit's own processes at run time. Apart from one line added
to the boot script, no system file is replaced or patched on disk, and one installer script puts it on (and takes it off) over the car's ENET connection, with backups of everything
it touches.

> **Disclaimer.** This project is for **educational purposes only**. Run it only on a car you own, at your own risk: you alone are
> responsible for whatever it does to your head unit, your car and its warranty, and for using it in line with the law where you
> drive. Never operate it while driving. It comes with no warranty of any kind (see [LICENSE](LICENSE)).

## What it does

### On iDrive 5: the CarPlay Advanced Settings menu

iDrive 5 HMIs give CarPlay a slot next to a side panel and send it only the knob's rotation and press. The menu plugin adds what
iDrive 6 has natively, and a bit more:

- **Full-screen CarPlay**: CarPlay over the whole display, without the side panel. MINI: the iPhone renders 1184 x 480 on the
  8.8" (tested in a car). BMW, BMW i and Rolls-Royce: 1440 x 540 on the 10.25", 1280 x 480 on the 8.8" (not tested in a car yet).
- **iDrive control**
  - **Knob tilts**: up / down / left / right move the CarPlay focus; a held tilt repeats.
  - **Touchpad**: the iDrive controller's touch surface as CarPlay's own touchpad. One finger scrolls, a tap selects
    (*Tap touchpad as click*), sensitivity Low / Medium / High, and *Two fingers tap to back* (two-finger slides also move the
    focus).
  - **Rotation as zoom**: turning the knob zooms the map (Apple Maps; Google Maps ignores it).
- **CarPlay directions in HUD** and **HUD effects** (below).
- **Restart to apply**: most switches take effect after a restart of the head unit; this does it from the menu.
- A **safe mode**: after three failed boots in a row with the plugin, nothing is applied until you choose Restart to apply; three more
  disable the plugin.

The menu:

```
Media/Radio > CarPlay Advanced Settings
  Full-screen CarPlay
  CarPlay directions in HUD
  HUD effects >        Effect colour > (Orange, Grey, White) / Fill toward the turn / Blink near the turn / HUD debug log
  Car and HUD type >   Automatic / BMW 6WA cluster / BMW 6WB cluster / BMW 6WB cluster + HUD / MINI
  Restart CarPlay
  iDrive control >     Enable knob tilts
                       Touchpad >   Enable touchpad / Tap touchpad as click / Touchpad sensitivity > (Low, Medium, High)
                                    / Two fingers tap to back
                       Enable rotation as zoom
  Restart to apply
```

On iDrive 6 the HMI already does full screen, knob tilts and the touchpad (when the car is coded for it), so there the installer
puts on the turn-by-turn part by default (`--hud`), with a **HUD-only menu** in the same place in Media/Radio:

```
Media/Radio > CarPlay Advanced Settings
  CarPlay directions in HUD
  HUD effects >        Effect colour > (Orange, Grey, White) / Fill toward the turn / Blink near the turn / HUD debug log
  Car and HUD type >   Automatic / BMW 6WA cluster / BMW 6WB cluster / BMW 6WB cluster + HUD / MINI
  Restart CarPlay
```

It changes nothing in the HMI itself, and its switches apply at the next CarPlay connection (no restart). The HMI shows it only when the
car is coded for apps (A4A); without that the directions still work, with their default settings. `--full` installs the whole menu
instead (not tried on iDrive 6 in a car).

### On iDrive 5 and 6: CarPlay turn-by-turn in the cluster and the HUD

The plugin reads the guidance the iPhone sends to the car (the maneuver, the distance, the street, the lanes) and shows it where the
BMW navigation would. How it gets there depends on the car's cluster; the installer finds out which one the car has.

| | F-series (MOST cluster: MINI F54/F55/F56, BMW 6WA, BMW 6WB digital cluster) | G-series and J29 (Ethernet KOMBI) |
|---|---|---|
| Who draws | **We do.** The plugin draws every picture itself, polygon by polygon, on the cluster's `HUDTerminal` over MOST. | **The car does.** The plugin sends the route data to the KOMBI's own guidance service (SOME/IP), and the cluster and the HUD draw it with their own graphics. |
| Pictures | Mapbox's maneuver and lane drawings, turned into HUD polygons (previews below). | The car's own arrows, distance and lane row. |
| Status | **Tested in a car** (MINI F54, HMI 1703): pictures, distance, street, HUD effects, lanes. **BMW 6WB** (since 1.1.1): its own layout, from [nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto)'s car-verified calls; not tested with this plugin in a car yet. | **Not tested in a car.** A port of NBT-AAD's car-verified KOMBI stack (see Credits), checked against a fake KOMBI only. |

**The BMW 6WB digital cluster** is an F-series MOST cluster too (the installer's "no Ethernet KOMBI ... MOST HUDTerminal" is the
right result for it), but its `HUDTerminal` has a different layout from the 6WA / MINI one: other instances for the guidance area,
the texts and the picture, ISO-8859-15 texts and a distance bar. Up to 1.1.0 the plugin only knew the 6WA layout, so a 6WB showed
nothing ([issue #6](https://github.com/st1ng/nbtevo-carplay-advanced/issues/6)). Now the plugin asks the cluster once when it starts and
logs the answer in `/dev/shmem/carplaymenu.log` (HUD-only installs: `/var/iap/rghook.log`): `HUD: cluster profile 6WB ...` or `6WA ...`. On a 6WB the arrow is drawn in the
junction-view area, the street, distance and unit in the cluster's own fields, and the distance bar in the last kilometre; the grey
context (roads not taken, lanes not to take) is drawn as outlines, because the 6WB's grey is not visible. If the detection ever picks
the wrong one, the file `hud_profile` in `/var/opt/hmi/data/rw/carplaymenu/` containing `6wb` (or `6wa`) forces it. The HUD effect
colours (Orange / Grey / White) are the F54's palette and are not checked on a 6WB.

**HUD effects** (MOST, menu *HUD effects*): the maneuver fills up with the effect colour toward the turn and / or blinks near it. The
start marker never fills or blinks.

**Car and HUD type** (MOST, main menu *Car and HUD type*, since 1.4.0; under HUD effects in 1.4.0 and 1.4.1): tells the plugin which cluster and HUD the car has
instead of letting it guess. *Automatic* (the default) keeps the detection. *BMW 6WA cluster* (also an analog cluster with a windscreen
HUD) uses the BMW picture box and the 6WA layout; *BMW 6WB cluster* the 6WB layout with no copies to a windscreen HUD; *BMW 6WB
cluster + HUD* the 6WB layout, drawn on the cluster (82/00) and copied to the windscreen HUD (82/02); *MINI* the MINI picture box.
It applies after a restart with the full menu, at the next CarPlay connection with the HUD-only menu. The switch files `hud_layout`,
`hud_box` and `hud_inst` still decide over it. It has no effect on an Ethernet KOMBI (G-series, J29), which the installer sets up.
*Restart CarPlay* (main menu, right under *Car and HUD type*, in both menus; since 1.4.3, there since 1.4.4; before that *Restart head unit*) restarts
CarPlay, not the head unit: it ends the running CarPlay session (the **mm-ipod** process, CarPlay's iAP2 stack) so that Connectivity
starts a fresh one. Nothing else is touched: not Android Auto (NBT-AAD), not CarPlay's video and network services, never
Connectivity (its watchdog would reset the unit). It is NBT-AAD's restart routine (the way nbt-aad finds and stops a process) aimed
at mm-ipod. With the HUD-only menu that applies every HUD setting (Car and HUD type, HUD effects, effect colour) without a reboot.
With the full menu, the options the HMI reads at its start still need *Restart to apply*, which restarts the head unit (since 1.4.2
with the same diagnostic ECU reset the phone app CarPlay Advanced Installer 1.6.7 sends). If no new session starts within 30 s,
unplug and reconnect the iPhone. Where the HMI may not stop mm-ipod itself (a refused signal, or no mm-ipod visible), the menu
asks mm-ipod to end itself, which `librghook.so` inside it does (since 1.4.4; needs *CarPlay directions in HUD* on). What it did is
in `/dev/shmem/carplaymenu.log`, every step on a `Restart CarPlay:` line: `found
mm-ipod pid ...` (and its path), `stopped`, `CarPlay session started again`, or why not (`no CarPlay session running`, a refused
signal with its errno).

The pictures as the plugin draws them (white = the way to go, grey = context). They are built by the plugin's own code from
[Mapbox's drawings](https://github.com/mapbox/mapbox-navigation-ios/tree/v0.40.0/MapboxNavigation) (ISC licence, see NOTICE):

![Maneuver pictures](docs/hud-maneuvers.svg)

![Lane strips](docs/hud-lanes.svg)

**Clusters that refuse the texts or show the arrow off-centre** (1.2.1, [issue #9](https://github.com/st1ng/nbtevo-carplay-advanced/issues/9),
BMW F25 X3): if the cluster refuses the street / distance texts, the plugin tries the other text encodings by itself (UTF-16,
ISO-8859-15, UTF-8) and stops sending texts if none is taken. Files in `/var/opt/hmi/data/rw/carplaymenu/` adjust a cluster by hand:
`hud_box` (`mini`, `6wa`, or `<x> <y> <size>`: where the arrow is drawn), `hud_text` (`utf16` / `iso` / `utf8` / `off`) and
`hud_text_handles` (`<street> <distance> <unit>`). `tools/hud_probe.sh` (run on the head unit) finds the values; since 1.2.1 it also
works in a HUD-only install, with CarPlay connected and no route. After a reroute the HUD now shows the first turn instead of the
route's start marker. Since 1.2.7 the F25 X3's cluster needs none of these files (tested in that car with 1.2.4 and the settings
`hud_box mini`, `hud_text_handles 0x0c 0x12 0x0f`, which 1.2.7 now picks by itself): it answers the picture-size question with
0 x 0, which now means the smaller box, and when every text form is refused on the usual fields, the plugin tries the F25's fields.
On that cluster the street field only appears while a route is guiding. A second `install --hud` on top of an earlier one no longer
stops with "mm-ipod is missing or already a link".

**Arrow cut off at the edge, or "1 5 km" instead of "1,5 km"** (1.2.3, [issue #12](https://github.com/st1ng/nbtevo-carplay-advanced/issues/12),
BMW F31): the plugin now asks the cluster how big its picture area is and, when the standard box does not fit, draws the arrow
inside it (the MINI F54, whose area is 120 x 72, gets exactly the box it had before). If your cluster does not answer, or the
arrow still looks off, `ksh hud_probe.sh box <x> <y> <size>` draws a test frame on the HUD, and `... box <x> <y> <size> save`
keeps it. Distances with a decimal comma ("1,5 km" on a Dutch, German or French iPhone) are now shown with a point ("1.5 km")
instead of a gap; the file `hud_comma` sends the comma itself, for a cluster whose font has one.

**Directions in the cluster but not on the windscreen HUD (BMW 6WB cars)** (1.2.6, [issue #1](https://github.com/st1ng/nbtevo-carplay-advanced/issues/1),
BMW F06): the plugin draws on the cluster's guidance terminal (82/00). A 6WB cluster also has a second terminal, 82/02, which is
probably the windscreen HUD. To also send the directions there, write `0 2` to `/var/opt/hmi/data/rw/carplaymenu/hud_inst` and
restart the head unit; `2` sends them to 82/02 only. `/dev/shmem/carplaymenu.log` then says whether 82/02 takes them (`mirror
82/02: ... taken, ... refused`). Not tried in a car yet.

**On a 6WB, do not use the F25 X3's settings** (1.2.11, [issue #10](https://github.com/st1ng/nbtevo-carplay-advanced/issues/10),
BMW F36): `hud_box mini` and `hud_text_handles 0x0c 0x12 0x0f` from issue #9 are for that car's **6WA** cluster. On a 6WB the
second one moved the street, distance and unit onto places the 6WB does not have. Since 1.2.11 the plugin ignores 6WA text places on
a 6WB and says so in the log (`hud_text_handles ... ignored`); `hud_box` was never used there. Delete both files
(`rm /var/opt/hmi/data/rw/carplaymenu/hud_box /var/opt/hmi/data/rw/carplaymenu/hud_text_handles`), restart, and if the directions
still do not show, post the `HUD:` lines of the log (`cluster profile 6WB ...` first).

**Long streets, and "Exit 104 ..." lines, cut off at the right edge of the windscreen HUD** (1.3.5, ghackt, Toyota GR Supra J29 /
A90: KOMBI with HUD). The KOMBI draws the street in its own font in the cluster and in the HUD, from one text: the plugin can neither
choose a smaller font nor send the HUD a shorter text, and the HUD's text field is the narrower one (its width is not published;
BMW only says its HUDs "can only show a certain number of text characters"). Up to 1.3.3 the street was cut to 22 characters, and
with capitals and digits that was still too wide ("Exit 104  Main Street" is 16.7 digits wide). Now the street is fitted by its
width, not by how many letters it has (an "i" is a quarter as wide as a "W"), into 14 digits' width:
- A street that fits is sent as the iPhone wrote it.
- A wider one is shortened the way USPS writes addresses (Publication 28): first the street suffix, with USPS's forms (Avenue AVE,
  Boulevard BLVD, Highway HWY, Parkway PKWY, Crossing XING, Turnpike TPKE, ...) and the directions (North N, Westbound WB): "US
  HIGHWAY 101 NORTH" becomes "US HWY 101 N"; then Mount MT, Fort FT, Point PT and Saint St; then words at the end are left out;
  a single word still too wide is cut after its last letter that fits. A USPS form that is no narrower than the word in the HUD's
  font (PRT, CRST, INLT, TRCE, HOLW; WAY, PARK and the like are the word itself) leaves the word as it is.
- An exit keeps its number first and whole, and the road gets the width that is left:
  "Exit 23B&nbsp;&nbsp;I-95 North" becomes "Exit 23B&nbsp;&nbsp;I-95 N". Besides off ramps, this now also covers a keep left /
  right, slight left / right or highway change that the iPhone gives an exit number.

The cluster shows the same shorter text. **To fit your HUD exactly** (14 digits is a cautious guess): create the file
`/var/opt/hmi/data/rw/carplaymenu/kombi_road_ruler` (`touch` it), restart the head unit and start a route with Apple Maps. Instead
of the street, the cluster and the HUD show the digits 1234567890 1234567890 ... (without the spaces). Count the digits the HUD shows
whole (each 0 ends another ten), write that number into `kombi_road_max` (`echo 16 > /var/opt/hmi/data/rw/carplaymenu/kombi_road_max`),
delete `kombi_road_ruler` and restart. `/var/iap/rghook.log` then says `kombi: street at most 16 digits wide`. A turn-by-turn update
keeps both files.

**The phone app stops after the install with "Backup read failed: .../settings/hud_guidance"** (1.3.3, ghackt's CarPlay
Advanced Installer app 1.6.4, a turn-by-turn update over 1.3.1). The install itself had completed: the head unit was fine, only the
copy of its backup to the phone stopped. The installer up to 1.3.2 wrote the "CarPlay directions in HUD" switch as an empty file.
A turn-by-turn update moves the previous settings folder into the backup, and the app cannot copy an empty file. The switch is now
written as "1", as the menu writes its switches, and an empty one from an earlier install is filled before the backup. Its meaning
does not change (only whether the file is there counts). A turn-by-turn update now also keeps the menu's HUD settings (effects,
colour, debug log, HUD tuning); until 1.3.3 they were reset at every update.

**Nothing in the cluster or the HUD on a BMW 6WB** (1.3.2, [ghackt/Carplay-advanced-installer issue #2](https://github.com/ghackt/Carplay-advanced-installer/issues/2),
BMW F11 retrofit, 6WB with HUD, HMI `ID5_1807-490-1809H Build 29`, turn-by-turn-only install). That report's log covers the install
only, so the cause is not known yet. These changes, compared with nbt-android-auto's 6WB sender (tested in a car), make the
likely causes go away, and the log now says which step stops:
- **The plugin waited for the BMW navigation that was not guiding.** One drawing call by any other client on the cluster's guidance
  terminal counted as the BMW navigation guiding, and the CarPlay picture waited. On a 6WB, single calls like that arrive now and
  then, and nbt-android-auto saw its own sender wait wrongly because of them. Now only a burst (4 picture calls within 3 s) counts,
  as there. Each wait and its end are in the log.
- **The 6WB was taken for a 6WA.** When the cluster answers no size question, the plugin drew on the 6WA's surfaces, which a 6WB
  does not have, and the cluster stayed dark. The size question now also goes the way nbt-android-auto asked a 6WB, it is asked
  again for a minute, and if the cluster refuses the 6WA surfaces the plugin switches to the 6WB layout by itself.
- **On a 6WB the picture is made visible directly**, as nbt-android-auto does, not only by swapping its two picture buffers.
- **The plugin's own calls are recognised among the last 256**, not 32. A 6WB picture sends every shape twice (filled and outline)
  and could exceed 32 calls, so its own calls could look like another client's.

If it still shows nothing: turn on *HUD effects > HUD debug log*, drive a short route with Apple Maps, and post
`/var/iap/rghook.log` (turn-by-turn-only install; the full plugin logs to `/dev/shmem/carplaymenu.log`). It says whether the
iPhone sends directions (`RG route_state`), whether the plugin reached the cluster (`MOST client`, `registry: HUDTerminal`), which
layout it drew (`cluster layout`), and how the cluster answered the first picture (`first picture ... taken ... refused`).

"ID5" in the software name is normal on a unit coded for iDrive 6: the HMI package is the same, and the installer decides iDrive 5
or 6 from the HMI itself. On iDrive 6 the HMI already does full screen, tilts and the touchpad, so the turn-by-turn-only install
is all the plugin adds there.

**HUD arrow small and in the corner on a BMW** (1.2.12, BMW F20): the plugin chose the MINI picture box whenever the MINI layout
was on the head unit, and many BMW head units carry it too. It now goes by the car's coded brand first (`HMI_BRAND`, as the menu does
for its layout): only a MINI gets the MINI box. The log says which (`HUD: HMI_BRAND=00 (not MINI): the 6WA picture box`).
`hud_box 6wa` was the workaround and still works.

**On the MOST cluster** the plugin takes the cluster's own handles from the BMW navigation's set-up at start (they differ between
clusters: an F25 X3 has its texts elsewhere than a MINI), asks the cluster how big its picture area is and fits the arrow into it,
and also draws on a second HUDTerminal (82/02) when the navigation uses one. Distances keep their decimal point ("1.5 km"). Seen in
a car so far only on the MINI F54 (the F30/F31 and F25 worked with the same settings made by hand); for other clusters the settings files `hud_box`, `hud_hide`, `hud_inst` and `hud_text_handles`
(AGENTS.md) set things by hand, and `sh carplay-advanced.sh hud-probe` shows what the cluster has.

**On the KOMBI** the street is fitted to the windscreen HUD's width with USPS short forms (1.3.5, above; `kombi_road_max` sets the
width), an exit shows its number first ("Exit 23B  I-95 N"), and a maneuver without a distance shows "0 m".

**Lane row going on and off on the KOMBI, music stopping and starting on a wireless connection** (1.3.1, G-series / J29). Four
causes in the plugin's own code, all fixed; none of the fixes has been tried in a car yet:
- The iPhone sends the lane sets and maneuvers of the whole route once, when the route starts. The plugin kept them in 8 (lanes) and
  32 (maneuvers) slots by number, so on a longer route a later lane set took the current one's slot, and the lane row came and went
  whenever one of them was sent again; past 32 maneuvers the arrow could come from another maneuver. They are now kept by their
  number (48 lane sets, 128 maneuvers).
- iOS reports "no route" for a moment after a reroute, and now and then during a route. Each time, the KOMBI was cleared and the
  whole route drawn again. The arrow and lane row now stay through such a gap for up to 8 s (as NBT-AAD does); "Arrived" still
  clears at once.
- A lane row that drops out for a moment stays 2.5 s, and a new one is sent once it has been the same for half a second.
- When the iAP2 stack reads the iPhone's data in pieces smaller than a route-guidance message (as a wireless connection is
  likely to), the plugin passed the bytes after that message to the stack first, out of order. A message the stack wrote in pieces,
  or that the connection took only part of, could leave with a wrong sequence number or cut short. Either one makes the phone and
  the head unit resend or reset the connection, which would explain music that stops and starts with full bars. Both directions now
  keep the stream intact.

**HUD debug log** (menu *HUD effects*, 1.3.0) logs every `HUDTerminal` call and answer, its terminals and its handles, in the
plugin log (in a HUD-only install `/var/iap/rghook.log`), within 5 s and without a restart: for an issue when the HUD shows
nothing. Switch it off again afterwards.

The BMW navigation always wins: while it guides, the CarPlay directions step aside.

## Supported software

The menu plugin patches the HMI's code in memory at fixed addresses, so it supports exact software builds, recognised by file
hashes:

| HMI | Build | |
|---|---|---|
| 006_006_006 | ID5_1703-490-1650ZD Build 139 (MINI F54, 8.8") | tested in a car |
| 006_017_006 | ID5_1803-510-1749MA Build 71 | not tried in a car yet |
| NBTevo_ID5_1607_Release | ID5_1607-490-1610T Build 153 (BMW F20) | since 1.2.4, [issue #8](https://github.com/st1ng/nbtevo-carplay-advanced/issues/8): knob tilts, touchpad, rotation as zoom and the HUD directions; since 1.2.12 **Full-screen CarPlay on a BMW** (tested in a BMW F20, 8.8": CarPlay fills the screen, no side panel). Its MINI layout has no CarPlay state the plugin can widen, so a MINI with this build keeps the option off |
| NBTevo_ID5_1903_Release | ID5_1903-490-1837J Build 41 (head unit NBTevo_Q18491I; BMW F25 X3, BMW F36) | since 1.2.11, [issue #9](https://github.com/st1ng/nbtevo-carplay-advanced/issues/9) / [issue #10](https://github.com/st1ng/nbtevo-carplay-advanced/issues/10): all options, Full-screen CarPlay included. The HUD directions work on the F25 X3; full mode not tried in a car yet |
| NBTevo_ID5_1807_Release | ID5_1807-490-1809H Build 29 (BMW F11, NBT Evo retrofit, 6WB cluster with HUD) | since 1.3.7, ghackt/Carplay-advanced-installer issue #2: all options (Full-screen CarPlay, knob tilts, touchpad, rotation as zoom, HUD directions). Its HMI is 006_017_006's code at other addresses; every place was found by the HMI search and checked by hand against the car's HmiMain3D. Not tried in a car in full mode yet |

Before it changes anything, the installer reads the head unit's HMI and CarPlay library and compares their hashes with the list
above. On other iDrive 5 software it **searches the HMI for the places the plugin uses** (about 20 s on your computer). If it finds
all of them, it says that this HMI is not supported yet and asks whether to install with the addresses it found (untested on that
build; the plugin still checks every word before it changes it, so a wrong address changes nothing). Either way it prints a link
that opens a GitHub issue with the hashes and the addresses it found: that is all that is needed to add the build to the list. If
the search does not find everything, it stops with nothing changed. The turn-by-turn-only install (`--hud`) patches no HMI code
and does not need a known build (reported drawing the arrow with `ID5_1903-490-1837J Build 41` on a BMW F25 X3, issue #9; installed
on `ID5_2211-490-2222A Build 13`, an iDrive 6 software, on a BMW F30, issue #11; arrow, street and distance drawn with
`ID5_2403-525-2403A Build 1` on a BMW F31, issue #12; installed on `ID5_1807-490-1809H Build 29` on a BMW F11 with a 6WB,
ghackt/Carplay-advanced-installer issue #2; the cluster arrow shows there with the 6WB layout, found by itself since 1.3.8, and since 1.3.8 a 6WB also gets copies on HUDTerminal 82/02 for the windscreen HUD; since 1.3.9 the HUD copy is drawn in a box that fits the HUD (switch `hud_box_2`), and on a 6WB street names in Chinese and other non-Latin scripts go as UTF-16 instead of question marks (switch `hud_text_enc`, issue #26); a supported build for the full install since 1.3.7). On a build nobody has reported yet, the
turn-by-turn install still goes ahead and prints a link that opens an issue with the build and its hashes, so you can say how it went. The installer also checks `mm-ipod` (the iPhone driver the directions hook into) by hash in every
mode: an unknown one only gets a note, since the hook works on the iPhone's messages rather than on addresses in it.

**Is full-screen CarPlay possible on my software?** Run `sh carplay-advanced.sh check` (since 1.2.0;
[issue #7](https://github.com/st1ng/nbtevo-carplay-advanced/issues/7)). It is **read only**: it reads the HMI, the CarPlay library,
`mm-ipod` and the layouts over SSH and works everything out on your computer, and nothing is uploaded, written or restarted on the
head unit. It reports:

- the generation (iDrive 5 or 6), the HMI version line and the files' hashes, and whether the build is a known one;
- the HMI search on **either generation**: whether this build has the `CARPLAY_FULLSCREEN` flag code (variant 0x9c71) the plugin
  forces on in memory, and the knob-tilt code;
- whether the HMI's layouts (MINI's, and the common one BMW uses) place CarPlay by the `IsCarPlayFullscreen` flag (then full screen is
  a state of that flag, drawn by the HMI) or at a fixed width (then only a layout change widens it);
- what that means: the plugin's option (known build), an install with the found addresses (iDrive 5, untested on that build), or, on
  **iDrive 6**, the coding of variant 0x9c71 (the plugin does not patch iDrive 6 HMIs);
- how it is undone: the plugin's patches are made in memory at every start and its layout and `carplay.ini` copies are path-manager
  links the head unit forgets at every reboot, so switching the option off (or `uninstall`) and a restart gives the stock state.

It writes `CHECK-<time>.txt` next to the installer (versions, hashes and the search result only: no VIN, coding values, logs or
captures) and prints a link that opens an issue with the same lines. A build is added to the supported list from exactly this: the
hashes of the real files and the addresses the search found, confirmed in a car.

**`ID5_1903-490-1837J Build 41`** (BMW F25 X3, [issue #9](https://github.com/st1ng/nbtevo-carplay-advanced/issues/9); BMW F36,
[issue #10](https://github.com/st1ng/nbtevo-carplay-advanced/issues/10)): a supported build since 1.2.11, so `install --full` no
longer asks. Up to 1.2.9 the search stopped at `not found: ctl` (this HMI keeps the controller's "pressed" value in r7 instead of r8);
1.2.10 installed it with the found addresses, but Full-screen CarPlay did nothing there: the search missed this HMI's
`CARPLAY_FULLSCREEN` flag code (one of its seven places uses another register), and its BMW layout, which already widens CarPlay when
that flag is on, was not one the plugin recognised. 1.2.11 has the build's own table, made from the car's HmiMain3D and layout
attached to issue #10: the flag is forced on in memory as on 006_017_006, and nothing is written into the layout or `carplay.ini`
(this car's is already full width). The turn-by-turn install (`--hud`) was already working on the F25 X3. Full mode has not been
tried in a car yet: please report how it goes.

**When the search finds nothing** (`not found: touch tdfirst tdend ctl play`, as on 1607): the HMI's code around those places
differs too much from the builds the search was made from. Such a build is added by hand, from the HMI file itself (1607 in 1.2.4,
issue #8). The installer then says the build was mapped by hand instead of asking for a report.

> **NBT-AAD.** This plugin has **not been tested together with [NBT-AAD](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)**
> (Android Auto for the NBT Evo). Both are HMI plugins that use the same cluster channels (the MOST `HUDTerminal`, the KOMBI guidance
> service) and similar hooks, so they may conflict. If you run both, please let me know how it goes:
> [open an issue](https://github.com/st1ng/nbtevo-carplay-advanced/issues/new).

## Prerequisites

- **SSH access on the head unit.** The NBT Evo's SSH server (root login over ENET, `169.254.199.99`) must be enabled; on most units
  it is not by default. The installer does everything over that connection.
- **For the menu: the car coded with A4A** (apps), so the HMI loads app plugins such as this menu. CarPlay itself activated and
  working.
- A **Mac or Linux computer** with `ssh` (OpenSSH), an **ENET cable** (OBD to Ethernet), and the ignition on (the head unit
  running). macOS gives the cable a link-local `169.254.x.x` address by itself; on Linux set that wired connection's IPv4 method
  to "Link-Local Only" (`nmcli connection modify <name> ipv4.method link-local`).

## Install

From this folder (the installer uploads `plugin/` next to it), the installer logs in over SSH, checks the head unit, shows what it
will do and asks before changing anything:

```bash
sh carplay-advanced.sh install
```

It then:

1. checks the plugin files, and that the head unit answers;
2. reads the HMI; for the full plugin on iDrive 5 it identifies the software by hash and searches it for the places the plugin uses
   (an unknown build where the search finds everything asks first; otherwise it stops here, with nothing changed);
3. asks **Proceed?**, copies every file it may replace to `BACKUP/` on your computer *before* it uploads anything, then uploads the
   plugin and backs those files up on the head unit too (copied to `BACKUP/` as well afterwards);
4. installs, prints a summary of what is on the head unit now, and offers to **restart** it. (Before 1.2.5 this restart could end
   with "The head unit did not confirm the restart in 40 s" and no reboot
   ([issue #12](https://github.com/st1ng/nbtevo-carplay-advanced/issues/12)); `sh carplay-advanced.sh restart` was not affected.)

After the restart: **Media/Radio > CarPlay Advanced Settings**. On a first install all options are off; a reinstall keeps them.

Other commands:

```bash
sh carplay-advanced.sh install --dry-run      # every check and the plan, nothing changed
sh carplay-advanced.sh status                 # what is installed, the links, the backups
sh carplay-advanced.sh check                  # read only: what decides full-screen CarPlay on this head unit, as a report
sh carplay-advanced.sh uninstall              # back to the original state (--keep-config keeps your switches)
sh carplay-advanced.sh restart                # restart the head unit
sh carplay-advanced.sh hud-probe              # HUD shows nothing? asks the cluster and the HUD what they have (~40 s, changes
                                              # nothing; needs "CarPlay directions in HUD" on), then shows five test pictures
                                              # and asks which ones you see; saved in BACKUP/ for an issue
```

Options: `--hud` / `--full` (turn-by-turn only, or everything), `--cluster most|kombi|both` (where the directions go, instead of
what the installer detects), `--yes` (no questions), `--host user@address`, `--help`.

What changes on the head unit: the plugin folder, the settings folder, and **one line** in the boot script (removed again by
uninstall). Everything else happens at run time and is gone after a restart once the plugin is removed.

## How it works

The short version. [AGENTS.md](AGENTS.md) has everything: every hook, address, file format and finding from the car.

- **The menu** is an HMI plugin (`libCarPlayMenu.so`), the same kind the HMI uses for its own apps. When the HMI starts, the plugin
  patches the HMI's code in memory (every word is checked first), shadows a few system files with path-manager links (a reboot
  forgets them), and hooks the controller and touchpad input.
- **Full-screen CarPlay** widens the CarPlay slot in the HMI's layout (MINI's own layout, or the common one BMW uses; on
  006_017_006 BMW the HMI's own full-screen layout is switched on) and tells the iPhone the larger screen size.
- **Knob and touchpad** reach CarPlay as real CarPlay input devices: `libcppinch.so`, preloaded into the head unit's Connectivity
  process, adds directions to the knob and turns rotation into a pinch on a hidden touchscreen.
- **Directions**: `librghook.so`, preloaded into the iPod/CarPlay process, decodes the iPhone's iAP2 route guidance. The plugin draws
  it on the MOST `HUDTerminal`, or `librghook-kombi.so` sends it to the Ethernet KOMBI.

## Build and test

```bash
./build.sh                                    # plugin/: the ARMv7 QNX libraries and the menu texts
python3 -m unittest discover -s tests         # the tests (macOS / Linux, needs clang)
python3 tools/hud_previews.py                 # docs/hud-*.svg, after a change to the pictures
```

Building needs only `clang` and `ld.lld`, and no BMW files: the libraries link against empty stand-ins that carry only the names of
the head unit's libraries. `plugin/` is committed prebuilt, so installing needs no build.

The tests run the real code on the computer: the menu's file patches, the HUD pictures, the iAP2 decoder, the KOMBI client against a
fake KOMBI, and the installer against a simulated head unit (its shell, file system and tools).

| Folder | What |
|---|---|
| `src/` | The plugin and its libraries (C / freestanding C++) |
| `plugin/` | The built plugin: what the installer uploads |
| `tests/` | Unit tests |
| `tools/` | The HUD picture generator (from Mapbox's drawings), the previews, and two scripts for the head unit: `hud_probe.sh` (probes the cluster's HUD) and `hud_sim.sh` (a pretend route, to see the HUD effects without driving) |
| `docs/` | The HUD previews |
| `.claude/skills/` | Skills for AI coding agents (below) |

### Skills for AI agents

Much of this project was built with an AI coding agent ([Claude Code](https://claude.com/claude-code)). `AGENTS.md` is written for
such agents as much as for people, and `.claude/skills/` holds two task guides that an agent loads when the task comes up:

- **`hmi-menu`**: adding, moving or removing menu rows. The menu lives in three files that must agree (the XML, the texts and the
  handlers); its `check_menu.py` checks that they do.
- **`hud-glyphs`**: the HUD pictures: regenerating them from Mapbox's drawings, changing the procedural ones (roundabouts, lanes,
  digits), the piece budget, and `preview.py`, which renders any picture to SVG exactly as the head unit builds it.

## Credits

- **[NBT-AAD](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)** by nanaisu: Android Auto for the NBT Evo. This project stands on
  their reverse engineering: the HMI plugin interface, the MOST/DSI client and the HUD terminal layout, and the Ethernet KOMBI
  route-guidance stack, which `src/kombi.h` ports (with their captured route graph, `src/kombitemplate.h`, unchanged). Thank you.
- **[NBT-AAD USB Tools](https://github.com/Yalozo/nbt-aad-usb-tools)** by Yalozo: the installer's pre-installation checks follow
  theirs.
- **[Mapbox Navigation SDK for iOS](https://github.com/mapbox/mapbox-navigation-ios)** v0.40.0: the maneuver and lane drawings
  (ISC licence), turned into HUD polygons by `tools/hud_glyphs.py`.
- [ftojke/BMW-iDrive6-CarPlay-HUD](https://github.com/ftojke/BMW-iDrive6-CarPlay-HUD),
  [JayJay3071/nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto),
  [luka-dev/mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi) and [f-io/LIVI](https://github.com/f-io/LIVI):
  findings and formats that helped (AGENTS.md, References). The BMW 6WB cluster's `HUDTerminal` layout and call forms come from
  nbt-android-auto's car-verified work.
- **Contributors**: [ghackt](https://github.com/ghackt) (the 1.2.x fixes from the issues and their port into 1.3.0: the 1607 build, the
  exit number, the start marker after a reroute, the KOMBI street length and its USPS short forms, the second `--hud` install, the
  restart after install),
  [FreZZZeR](https://github.com/FreZZZeR) (`hud_box` / `hud_hide`, PR #13), and the owners who tested in their cars and sent logs,
  handle maps and files: cfrogjump (F25), mrmilankd (F31), judirado-byte (F32), Barlows (F20), itstheodor (F36), tangshijiao and
  Sir-Gordon (6WB), ONSS12.

## Feedback

Tried it on another car or another software build, found a bug, or run it next to NBT-AAD? Please
[open an issue](https://github.com/st1ng/nbtevo-carplay-advanced/issues/new).

## License

[PolyForm Noncommercial 1.0.0](LICENSE), the installer included: free for personal, hobby, research and educational use, not for
commercial use. The exceptions are Mapbox's drawings (`src/hudglyphs.h` and the previews in `docs/`, ISC) and the files from NBT-AAD
(PolyForm Noncommercial, with their notice). Details in [NOTICE](NOTICE).

Not affiliated with BMW, MINI, Apple or Harman. BMW, MINI, iDrive and CarPlay are trademarks of their owners. For educational
purposes only, on your own car and at your own risk (see the disclaimer at the top): it changes how your head unit runs, though
everything it changes can be undone with `uninstall`.
