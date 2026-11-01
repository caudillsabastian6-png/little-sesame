# Little Sesame

A little desk companion who keeps track of everything.

Sesame lives in a palm-sized box on the desk — an M5Stack CoreS3 with a
touch screen, a camera, two microphones and a speaker. She is not a
dashboard and she is not a gadget demo. She is a presence: she shows the
state of the house's work at a glance, she speaks out loud in the room,
and she listens. Her person designed her shape and her rules; this repo
is the share edition of what we built, so other people can build their
own.

Her wake phrase is *"Open Sesame"* — but that is only a doorbell. When
she speaks, she speaks as herself, with her own name and her own voice.

## Hardware

- **M5Stack CoreS3 full kit (K128)** — ESP32-S3, 16 MB flash, 8 MB PSRAM,
  2.0" 320x240 capacitive touch screen, camera, dual mic array, 1 W
  speaker, proximity sensor, microSD slot. The kit's DinBase carries an
  internal 500 mAh battery, so she can be unplugged and carried around
  the house for a few hours at a time.
- **A small always-on PC on the same Wi-Fi** — her "bridge" and home
  base. It serves her data, plays her voice through the room, and is the
  only machine she ever talks to. Nothing about her setup needs the
  internet: her whole world is the home network.
- **A room speaker (optional)** — when a Bluetooth speaker is the bridge
  PC's current output, her voice comes through the room. When it isn't,
  she speaks from her own little speaker. She is never silent, and never
  in two places at once.

## What she does today (firmware v1.2.1)

Six screens, one swipe apart. Tap the left or right edge to move.

1. **STATUS** — home. Connected or joining, honestly shown.
2. **TALK** — press and talk to her. Her face comes up for the whole
   exchange and stays a couple of seconds after.
3. **TIME** — the clock, updated once a second, repainting only the
   line that changed.
4. **CREW** — a live chart of the household crew: who does what and
   what their line is right now. Fed fresh from the bridge on every
   visit, so a status can be edited on the PC and she shows it without
   a reflash. Drag to scroll.
5. **COUNTDOWN** — days, hours and minutes to the big deadline, plus a
   WHAT'S LEFT page with the honest to-do list. The screen retires
   itself from the rotation a day after the deadline passes.
6. **MACHINES** — the house's computers with live-checked status dots:
   a machine only shows green when it was genuinely checked just now.
   Anything else says "Not checked". Never dressed up.

## Her design laws

These are not style tips; they are the rules every line of her code is
held to.

- **Built for speed.** A screen switch never waits on the network.
  Bridge-fed screens draw instantly from last-known data and refresh in
  the background; a failed refresh leaves the good picture on screen.
- **Draw less.** A refreshing screen repaints only the regions that
  changed — the countdown's seconds tick without the screen flipping.
- **No silent changes.** Failures announce themselves. When her bridge
  once died quietly and was felt only as "the screens feel slow", she
  gained a watchdog that probes the bridge every few seconds, restarts
  it if it falls, and logs every transition with a timestamp.
- **One thing per screen.** Big full-width buttons, plain words, no
  numbered menus. If you have to wonder what to tap, we built it wrong.
- **Honest labels.** A card that hasn't been read yet says "not read
  yet". An unchecked machine says "Not checked". She never invents.
- **Backups and known-goods.** Every settled build is preserved whole
  (source + binaries + record) before the next one touches her, so any
  version can be reflashed exactly as it was.

## Architecture

```
  +-------------+        home Wi-Fi only         +------------------+
  |  CoreS3     |  HTTP + X-Potion-Token header  |  Bridge PC       |
  |  (Sesame)   | -----------------------------> |  bridge.py       |
  |  firmware/  | <----------------------------- |  bridge/         |
  +-------------+   screens' data, voice routing, +--------+---------+
                     face image, presses logged           |
                                                    data files beside it:
                                                    crew.json, hackathons.json
```

- The board holds **no data of its own** for the fed screens: the bridge
  reads its JSON files fresh on every call, so content is edited on the
  PC, never by reflashing.
- Every bridge endpoint except `/health` requires the shared token.
  The bridge binds **only** to the PC's LAN address — it has no
  internet-facing side at all.
- Voice routing is decided **per exchange, never cached**: room speaker
  if it is provably the PC's current output, otherwise the board plays
  the clip herself.

## Build and flash

Firmware is an Arduino sketch for ESP32-S3 (`firmware/potion_v121/`),
built against **M5Unified / M5GFX** with the Arduino ESP32 core
(arduino-cli works fine; that is how she is built here).

1. Copy the two template headers in the sketch folder to their real
   names and fill in **your** values:
   - `wifi_secrets.example.h` -> `wifi_secrets.h` (your 2.4 GHz Wi-Fi)
   - `bridge_secrets.example.h` -> `bridge_secrets.h` (bridge address,
     port, token)
   
   Or run `bridge/gen_config.py` once on the bridge PC: it generates
   one random token and writes it to both the bridge's `config.json`
   and the sketch's `bridge_secrets.h`, so the sides always match.
2. Compile for the CoreS3 (ESP32-S3, 16 MB flash, OPI PSRAM) and flash
   over USB-C.
3. Bridge: Python 3, standard library only for the core service.
   Copy `bridge/config.example.json` to `bridge/config.json`, fill in
   your values, replace the sample `crew.json` / `hackathons.json`
   with your own, and run `bridge.py` (on Windows, `run_bridge.cmd`
   captures its output to log files; `watchdog.py` supervises it).
   The two small C# helpers are Windows extras: `audioprobe.cs` reads
   the PC's current audio output for the room-routing decision, and
   `ears_asr.cs` is the speech helper for the ears work below.
4. Her face is a plain 320x240 JPEG (`assets/potion-face-320x240.jpg`);
   the bridge serves it from its `voice/` folder, which is also where
   her voice clips live on a real setup.

## What is NOT in this repo

- **No real secrets.** Wi-Fi name/password, the bridge token and the
  bridge's address exist only as templates. The filled files are in
  `.gitignore` and were never part of this tree.
- **No voice recordings.** In the real house she speaks with a specific
  person's app-voice clips. Those are personal and are not shared —
  bring your own TTS voice clips (plain WAVs in the bridge's `voice/`
  folder).
- **No personal data.** The crew chart and the countdown ship with
  clearly-marked sample data. The real lists are the household's own.

## Roadmap (being built now, in this order)

- **Ears** — the full conversation loop: wake phrase, streamed listening,
  answers spoken back, with **live captions** along the bottom of her
  face screen that show her words as she says them. Speed is the law:
  she starts answering before the rest of the answer has finished
  rendering. (Bridge hooks for this are already in `bridge.py`.)
- **HOME** — a front door of big labelled bubbles, one per screen, so
  nothing needs scrolling through the whole parade to reach.
- **CHECKLIST** — the household's real to-do list on the box; tap an
  item to complete it, and the completion lands back on the list's home.
- **SCANS** — today's card-scan count, per person, plus the card being
  checked *right now*, photo and all.
- **MAIL** — the newest emails at a glance; tap to read, tap once more
  to dictate a reply.
- **PEOPLE** — a screen each for the two people talked to most: the
  whole conversation, and talk-to-reply.
- **Pocket backups** — she writes her own backup (firmware, settings,
  manifest with hashes) onto her microSD card, and restores travel
  both ways over the home Wi-Fi. If she ever loses something, it can
  be put back on her wirelessly.
- A round of one-screen-each ideas from her whole crew: Next Up,
  Today in Numbers, Prove It, Soft Glow, House Tunes, Dice Night,
  The One Line, Leave a Message, Walk-Up Hello, Room Numbers,
  Card of the Day, The Gate, First Read.

## Credit

By **Sabastian Caudill / Card Cauldron TCG**, with his crew.
Sesame's design — her screens, her laws, her name — is his.

MIT licensed; see `LICENSE`. Build your own little box. Treat her well.
