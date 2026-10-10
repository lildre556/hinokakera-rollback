Hinokakera Chaotic Eclipse - GGPO rollback netplay (hinoroll VERSION)
=====================================================================

Rollback netcode (GGPO) for Hinokakera Chaotic Eclipse ver.1.01.
Battles use rollback: your inputs show up immediately and the game
corrects itself when your opponent's inputs arrive. Menus, character
select and loading wait for both players (lockstep), so they always match.

This version is for Chaotic Eclipse only (hinokakera.exe of 2011-08-30,
the game's title screen says "ver. 1.01"). For Hinokakera fragment:Eclipse
ver.3.00 use hinoroll 1.1.


INSTALL
-------
Copy these into the game folder (next to hinokakera.exe):
    dinput8.dll
    Hinokakera Netplay.bat
Both players need this same mod version and the same game (ver.1.01,
unmodified data files).

Offline play is unchanged: start hinokakera.exe as usual.


SETTINGS (F8)
-------------
Press F8 in the game for the settings screen. It works anywhere, online
too (offline the game waits while it's open). Three pages; Tab, or LB/RB
on a pad, or Left/Right on the page names, switches between them.

Controls
- Xbox and most modern pads (XInput) and DirectInput pads (PlayStation
  pads, arcade sticks, hit boxes...) all work, D-pad included.
- Pick each player's controller: Controller row, Enter (or A), then press
  a button on the pad you want. Delete there means keyboard only.
- Bind: go to an action, press Enter (or A), then press the button,
  direction, stick or trigger you want. An action can have up to 4
  controls; Delete clears it. "Reset to defaults" restores the layout.
- A+D and C+D are one-button shortcuts for those combinations.
- Defaults on an Xbox-style pad: D-pad and left stick move, X Y A B are
  A B C D (Neo Geo layout), Start pauses, RB = A+D, RT = C+D. DirectInput
  pads start from your config.exe Joypad buttons, plus the hat and stick.
- The keyboard is still set up in config.exe (or the game's Config menu).
  To let the game read pads itself again, put
      [pads]
      enabled=0
  in hinoroll.ini.

Display
- Window mode: Window, Borderless window (a window covering the whole
  monitor) or Fullscreen (the game's exclusive fullscreen). Left/Right to
  choose, Enter to switch.
- Borderless is the one to use for playing online: Alt-Tab is instant and
  never interrupts the game. The game keeps drawing at the size set in
  config.exe and the picture is scaled to the monitor.
- "F4 / maximize switches to": what F4 (and the window's maximize button)
  switches to from the window: Borderless window (default) or Fullscreen.
- "Borderless picture": keep the game's 4:3 shape (black bars at the
  sides) or fill the whole monitor.
- The choice is remembered: the game starts in the mode you left it in.

Audio
- Master, Music and Effects volume sliders (Left/Right, hold to slide;
  Enter mutes / sets 100). Music and Effects are the game's own BGM and SE
  volume settings, the same numbers its Config menu shows; Master turns
  both down together.

Everything is saved in hinoroll.ini. None of these settings are part of
the online game: each player has their own.


PLAY ONLINE
-----------
1. Both players start "Hinokakera Netplay.bat". A connection window opens
   (before the game window).
2. Press Copy and send your code (hino-XXXXX-...) to your opponent.
   Paste theirs into the second box (Paste).
3. One player picks "Player 1", the other "Player 2". Player 1's game
   settings are used for both (speed, COM level, win count, time, visual,
   field, cut-in and blood effects, texture).
4. Both press Connect (within a minute of each other). When the two games
   find each other the window closes and the game starts.
5. Pick VS Human on the title menu and play.

Each player uses their own Player 1 controls (keyboard keys from
config.exe, pad from the F8 settings).

Input delay: "Auto" picks it from the ping (1-4 frames). Lower delay means
more rollback (fine on a good connection); higher means less correction
but more input lag. The choice is remembered in hinoroll.ini.

The window title shows the ping, the input delay and how many frames were
re-simulated in the last second. If it ever says DESYNC DETECTED, the two
games disagree: tell whoever maintains the mod and send hinoroll.log.


IF YOU CAN'T CONNECT
--------------------
- Both players must use each other's current code. A code changes when your
  internet address changes, so copy it fresh each time.
- The connection is direct (no server). It gets through most home routers by
  both sides sending to each other at once. Some routers and mobile/
  satellite connections (carrier-grade NAT, e.g. Starlink) block that when
  BOTH players are behind one. Then the player with a normal home router
  forwards UDP port 7000 to their PC and gives their opponent
  "THEIR.PUBLIC.IP:7000" instead of the code. The port can be changed in
  hinoroll.ini:
      [netplay]
      port=7000
- On the same LAN the code works as is (it includes your LAN address).
- Allow hinokakera.exe through the Windows firewall if it asks.
- The game's own "Network" mode (net.txt, port 10996) isn't used by the
  mod; it still binds that port at startup, as always.


DURING ONLINE PLAY
------------------
- All 13 characters and all 4 colours are available to both players
  online, and both start from the same default picks (unlocks and
  last picks normally come from each player's own save files).
- The title menu online offers the two-player modes: VS Human, Training
  and Watch. The others (Arcade, VS Com, Network, Config, Record, Replay,
  Story, Exit) don't respond, and there's no attract demo.
- No pause in online battles (Start does nothing).
- Escape doesn't quit online. Close the window to leave; the other player
  gets "the opponent disconnected" and carries on offline.
- Your win/loss records (record.dat) aren't changed by online matches.
- The game keeps running (and connected) when its window is in the
  background or minimized, and through Alt-Tab in exclusive fullscreen.
- Replays still save as usual.
- hinoroll.log in the game folder has details if something goes wrong.


CREDITS AND LICENSES
--------------------
GGPO (c) GroundStorm Studios, MIT license. MinHook (c) Tsuda Kageyu,
BSD 2-clause license. dlmalloc by Doug Lea, public domain (CC0).
See hinoroll-LICENSES.txt. Hinokakera (c) Reddish Region.
