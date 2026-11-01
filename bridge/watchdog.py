# Potion CoreS3 bridge watchdog -- the no-silent-changes law, enforced.
#
# Born of 2026-11-01: the bridge died mid-afternoon with no record
# anywhere and the owner felt it only as "transitions take a little
# while". This supervisor probes the bridge's /health every 15 s. After
# 2 consecutive failures it restarts the bridge (killing any wedged
# listener on the bridge's own port first) and logs every transition
# with a timestamp to watchdog.log. It also keeps bridge_state.json
# current so any other tool can see the bridge's state at a glance.
#
# Stdlib only. Runs as scheduled task PotionBridgeWatchdog (ONLOGON).
import json
import os
import subprocess
import time
import urllib.request
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(HERE, "watchdog.log")
STATE_PATH = os.path.join(HERE, "bridge_state.json")
try:
    with open(os.path.join(HERE, "config.json"), encoding="utf-8") as _f:
        _CFG = json.load(_f)
except Exception:
    _CFG = {}
HEALTH_URL = "http://%s:%d/health" % (_CFG.get("host", "127.0.0.1"),
                                      int(_CFG.get("port", 8377)))
TASK_NAME = "PotionCoreS3Bridge"
PROBE_EVERY_S = 15
FAIL_THRESHOLD = 2


def log(msg):
    line = "%s %s" % (datetime.now().isoformat(timespec="seconds"), msg)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def write_state(alive, restarts, last_ok):
    try:
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump({
                "bridge_alive": alive,
                "restarts": restarts,
                "last_ok": last_ok,
                "watchdog_seen": datetime.now().isoformat(timespec="seconds"),
            }, f)
    except OSError:
        pass


def probe():
    try:
        req = urllib.request.Request(HEALTH_URL,
                                     headers={"User-Agent": "bridge-watchdog"})
        with urllib.request.urlopen(req, timeout=3) as resp:
            if resp.status == 200:
                body = json.loads(resp.read().decode("utf-8", "replace"))
                return bool(body.get("ok"))
    except Exception:
        pass
    return False


def kill_port_listeners():
    """Kill whatever still holds the bridge port (a wedged python whose
    /health stopped answering is the case that matters). The port is
    the bridge's alone, so this cannot hit an innocent process."""
    try:
        out = subprocess.run(["netstat", "-ano"], capture_output=True,
                             text=True, timeout=10).stdout
    except Exception as e:
        log("watchdog: netstat failed: %r" % (e,))
        return
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[1].endswith(":8377") and \
                parts[3].upper() == "LISTENING":
            pid = parts[4]
            if pid.isdigit() and pid != "0":
                log("watchdog: killing stale listener pid=%s" % pid)
                subprocess.run(["taskkill", "/PID", pid, "/F"],
                               capture_output=True, timeout=10)


def restart_bridge():
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Stop-ScheduledTask -TaskName '%s'" % TASK_NAME],
                   capture_output=True, timeout=30)
    time.sleep(1)
    kill_port_listeners()
    time.sleep(1)
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Start-ScheduledTask -TaskName '%s'" % TASK_NAME],
                   capture_output=True, timeout=30)


def main():
    log("watchdog starting (probe every %ds, restart after %d failures)"
        % (PROBE_EVERY_S, FAIL_THRESHOLD))
    fails = 0
    restarts = 0
    alive = probe()
    last_ok = datetime.now().isoformat(timespec="seconds") if alive else None
    write_state(alive, restarts, last_ok)
    log("watchdog initial state: bridge_alive=%s" % alive)
    while True:
        time.sleep(PROBE_EVERY_S)
        ok = probe()
        if ok:
            if not alive:
                log("watchdog: bridge is answering again")
            alive = True
            fails = 0
            last_ok = datetime.now().isoformat(timespec="seconds")
        else:
            fails += 1
            if alive:
                log("watchdog: /health FAILED (%d consecutive)" % fails)
            alive = False
            if fails >= FAIL_THRESHOLD:
                log("watchdog: bridge DOWN -- restarting task %s" % TASK_NAME)
                try:
                    restart_bridge()
                    restarts += 1
                    log("watchdog: restart issued (total restarts=%d)" % restarts)
                except Exception as e:
                    log("watchdog: restart attempt failed: %r" % (e,))
                fails = 0
                # give the fresh bridge a moment, then re-probe honestly
                time.sleep(5)
                if probe():
                    alive = True
                    last_ok = datetime.now().isoformat(timespec="seconds")
                    log("watchdog: bridge recovered after restart")
                else:
                    log("watchdog: bridge STILL DOWN after restart")
        write_state(alive, restarts, last_ok)


if __name__ == "__main__":
    main()
