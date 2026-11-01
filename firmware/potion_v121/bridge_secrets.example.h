// bridge_secrets.h -- TEMPLATE. Copy this file to "bridge_secrets.h" in
// the same folder and fill in YOUR values. The filled file is listed in
// .gitignore -- never commit it, never share it.
//
// Tip: bridge/gen_config.py generates ONE random token and writes it to
// BOTH the bridge's config.json and this header, so the two sides always
// match. Run it once on the bridge PC and copy this header into your
// sketch folder.
#pragma once

// The bridge PC's address on your home network (the PC that runs
// bridge/bridge.py). The board talks only to this address, only on your
// own Wi-Fi.
#define BRIDGE_HOST "YOUR-BRIDGE-LAN-IP"
#define BRIDGE_PORT 8377

// The shared token. Every bridge endpoint except /health requires it in
// the X-Potion-Token header. Use a long random value of your own.
#define BRIDGE_TOKEN "YOUR-OWN-LONG-RANDOM-TOKEN"
