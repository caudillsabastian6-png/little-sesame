# Potion CoreS3 bridge -- LAN-only helper service for the CoreS3 desk board.
# Binds ONLY to the bridge PC's LAN interface (address from config.json).
# Token (header X-Potion-Token) required on every endpoint except /health
# (audit F13: the v1.2 comment understated this; the code was always
# stricter). The token is never logged or echoed.
import ctypes
import hashlib
import json
import math
import os
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
PRESSES_PATH = os.path.join(HERE, "presses.jsonl")
LOG_PATH = os.path.join(HERE, "bridge.log")
VOICE_DIR = os.path.join(HERE, "voice")
os.makedirs(VOICE_DIR, exist_ok=True)
START = time.time()
LOCK = threading.Lock()

# ---- v1.22 ears + backup additions -------------------------------------
EARS_LOG_PATH = os.path.join(HERE, "ears.log")
ASR_EXE = os.path.join(HERE, "ears_asr.exe")
CAPTIONS_PATH = os.path.join(HERE, "captions.json")
EARS_TMP_DIR = os.path.join(HERE, "ears-tmp")
os.makedirs(EARS_TMP_DIR, exist_ok=True)
WAKE_PHRASE = "open sesame"
PCM_RATE = 16000  # the board streams 16 kHz 16-bit mono PCM

# The pocket-backup allowlist (step 2c of the ears round): serve name ->
# absolute path on the bridge PC. The board downloads these onto her SD card and
# hash-verifies every byte there. config.json carries the bridge token;
# it is copied file-to-card by the board and its bytes are never printed
# or logged by either side (only its SHA-256 travels, in the manifest).
CORES3_DIR = os.path.join(os.path.dirname(HERE), "cores3")  # the firmware tree sits next to this bridge folder; adjust if yours lives elsewhere
BACKUP_FILES = [
    ("potion_v121-known-good.bin",
     os.path.join(CORES3_DIR, "v1.2.1-known-good", "potion_v121.ino.bin")),
    ("BUILD-RECORD.md", os.path.join(CORES3_DIR, "BUILD-RECORD.md")),
    ("bridge.py", os.path.join(HERE, "bridge.py")),
    ("config.json", CONFIG_PATH),
    ("crew.json", os.path.join(HERE, "crew.json")),
    ("hackathons.json", os.path.join(HERE, "hackathons.json")),
    ("presses.jsonl", PRESSES_PATH),
    ("voice-hello-boss.wav", os.path.join(VOICE_DIR, "hello-boss.wav")),
    ("voice-im-here-boss.wav", os.path.join(VOICE_DIR, "im-here-boss.wav")),
    ("voice-open-sesame-listening.wav",
     os.path.join(VOICE_DIR, "open-sesame-listening.wav")),
    ("voice-ears-gotit.wav", os.path.join(VOICE_DIR, "ears-gotit.wav")),
    ("voice-ears-sorry.wav", os.path.join(VOICE_DIR, "ears-sorry.wav")),
    ("potion-face-320x240.jpg",
     os.path.join(VOICE_DIR, "potion-face-320x240.jpg")),
]

with open(CONFIG_PATH, "r", encoding="utf-8") as f:
    CFG = json.load(f)
HOST = CFG["host"]
PORT = int(CFG["port"])
TOKEN = CFG["token"]
BT_SPEAKER = CFG.get("bt_speaker", "")  # exact render-endpoint name of the room speaker
PROBE_EXE = os.path.join(HERE, "audioprobe.exe")
CLIP_FOR_EVENT = {"press": "im-here-boss.wav", "hello": "hello-boss.wav"}
CREW_PATH = os.path.join(HERE, "crew.json")
# Optional peer PC health probe: set peer_health_url in config.json to
# another machine's /health endpoint and the MACHINES screen checks it
# live. Empty = not checked. peer_health_host_hint, when set, must
# appear in the answer's "host" field for the check to count as the peer's.
PEER_HEALTH_URL = CFG.get("peer_health_url", "")
PEER_HEALTH_HOST_HINT = CFG.get("peer_health_host_hint", "")
HACKATHONS_PATH = os.path.join(HERE, "hackathons.json")


def log(msg):
    line = "%s %s" % (datetime.now().isoformat(timespec="seconds"), msg)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def clip_play_ms(path):
    # clips are 22050 Hz 16-bit mono PCM = 44100 bytes/s after the 44-byte header
    try:
        return max(0, (os.path.getsize(path) - 44)) * 1000 // 44100
    except OSError:
        return 0


def default_output_name():
    """Current default render endpoint name, or None if the probe fails."""
    try:
        out = subprocess.run([PROBE_EXE], capture_output=True, text=True, timeout=4)
        for line in out.stdout.splitlines():
            if line.startswith("DEFAULT="):
                return line[len("DEFAULT="):].strip()
    except Exception as e:
        log("audioprobe failed: %r" % (e,))
    return None


def decide_route(clip_name):
    """Per-exchange route decision, never cached.

    route=room only when the Bluetooth speaker endpoint is the bridge PC's CURRENT
    default output (which also proves it is present and active) -- then the
    bridge plays the clip itself through that output. Anything else (BT off,
    speaker gone, default fell back to Realtek/HDMI, probe failure, player
    failure) -> route=local and the board plays the clip herself.
    Returns (route, play_ms).
    """
    path = os.path.join(VOICE_DIR, clip_name)
    if not BT_SPEAKER or not os.path.isfile(path):
        return ("local", 0)
    if default_output_name() != BT_SPEAKER:
        return ("local", 0)
    try:
        started = ctypes.windll.winmm.PlaySoundW(path, None, 0x00020000 | 0x00000001)
    except Exception as e:
        log("playsound failed: %r" % (e,))
        return ("local", 0)
    if not started:
        log("playsound returned 0 for %s" % clip_name)
        return ("local", 0)
    ms = clip_play_ms(path)
    log("route=room clip=%s play_ms=%d (default output is %s)" % (clip_name, ms, BT_SPEAKER))
    return ("room", ms)


def record_press(source):
    rec = {"ts": datetime.now().isoformat(timespec="seconds"),
           "client": "sesame", "source": source}
    with LOCK:
        with open(PRESSES_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    n = presses_today()
    log("press recorded client=sesame source=%s presses_today=%d" % (source, n))
    return n


def presses_today():
    today = datetime.now().date().isoformat()
    n = 0
    try:
        with open(PRESSES_PATH, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if str(rec.get("ts", "")).startswith(today):
                    n += 1
    except FileNotFoundError:
        pass
    return n



def crew_data():
    """The Tiered Crew seats, read fresh from crew.json on every call so the
    status lines can be updated on the bridge PC without touching the firmware."""
    try:
        with open(CREW_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and isinstance(data.get("seats"), list):
            data["ok"] = True
            return data
        log("crew.json has unexpected shape")
    except Exception as e:
        log("crew.json read failed: %r" % (e,))
    return {"ok": False, "error": "no crew data"}



def hackathons_data():
    """Hackathon events (per-event to-dos + credits line), read fresh from
    hackathons.json on every call so the lists can be updated on the bridge PC
    without a reflash. Credits figures live in that file and are only ever
    what Potion has verified, with an as-of note -- the bridge itself never
    computes or invents a balance."""
    try:
        with open(HACKATHONS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and isinstance(data.get("events"), list):
            data["ok"] = True
            return data
        log("hackathons.json has unexpected shape")
    except Exception as e:
        log("hackathons.json read failed: %r" % (e,))
    return {"ok": False, "error": "no hackathon data"}


def machines_data():
    """The machine chain, checked live from the bridge's seat.

    The bridge PC is ok by construction (this code is answering). A peer
    PC is genuinely probed when peer_health_url is set in config.json:
    its /health must answer ok -- anything else (timeout, refusal, wrong
    answer) is reported honestly as "unknown" / "Not checked", never
    dressed up as green. Edit the rows below to describe your own chain.
    """
    peer_state, peer_line = "unknown", "Not checked"
    if PEER_HEALTH_URL:
        try:
            req = urllib.request.Request(PEER_HEALTH_URL,
                                         headers={"User-Agent": "sesame-bridge"})
            with urllib.request.urlopen(req, timeout=3) as resp:
                if resp.status == 200:
                    body = json.loads(resp.read().decode("utf-8", "replace"))
                    if body.get("status") == "ok" and \
                            (not PEER_HEALTH_HOST_HINT or
                             PEER_HEALTH_HOST_HINT in str(body.get("host", ""))):
                        peer_state, peer_line = "ok", "Answering"
        except Exception as e:
            log("peer check failed: %r" % (e,))
    return {
        "ok": True,
        "checked_at": datetime.now().isoformat(timespec="seconds"),
        "machines": [
            {"rank": 1, "name": "Bridge PC",
             "state": "ok", "line": "On duty"},
            {"rank": 2, "name": "Peer PC",
             "state": peer_state, "line": peer_line},
        ],
    }


# ------------------------------------------------------------- ears (v1.22)
# The conversation loop's bridge half. The board streams raw PCM here;
# recognition is Windows' built-in offline en-US recognizer driven
# through ears_asr.exe (System.Speech). The wake phrase is a DOORBELL
# only -- it is spotted here, on the bridge, over the streamed audio
# (ESP-SR WakeNet ships no "Open Sesame" model; this route is the
# honest one and the report says so). Spoken identity is always Potion:
# every voiced line is one of her pre-rendered app-voice clips, and the
# substantive answer travels as text to the face screen.

def ears_log(msg):
    line = "%s %s" % (datetime.now().isoformat(timespec="seconds"), msg)
    try:
        with open(EARS_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def epoch_ms():
    return int(time.time() * 1000)


def clip_caption(name):
    """The exact words a clip speaks (for the board's caption band).
    Sourced from captions.json; a clip with no caption entry gets NO
    caption rather than an invented one."""
    try:
        with open(CAPTIONS_PATH, "r", encoding="utf-8") as f:
            caps = json.load(f)
        return str(caps.get(name, ""))
    except Exception:
        return ""


def write_wav(path, pcm):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, PCM_RATE,
                                       PCM_RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)))
        f.write(pcm)


def run_asr(mode, wav_path, phrase=None):
    """Drive ears_asr.exe; returns (kind, conf, text)."""
    cmd = [ASR_EXE, mode, wav_path]
    if phrase:
        cmd.append(phrase)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=25)
        line = (out.stdout or "").strip().splitlines()
        line = line[0] if line else ""
    except Exception as e:
        ears_log("asr run failed mode=%s: %r" % (mode, e))
        return ("ERR", 0.0, "")
    kind = line.split(" ", 1)[0] if line else "ERR"
    conf = 0.0
    text = ""
    for tok in line.split(" "):
        if tok.startswith("conf="):
            try:
                conf = float(tok[5:])
            except ValueError:
                pass
        elif tok.startswith("text="):
            text = line[line.index("text=") + 5:]
            break
        elif tok.startswith("best="):
            text = line[line.index("best=") + 5:].split(" conf=")[0]
            break
    return (kind, conf, text)


def frame_rms(frame):
    n = len(frame) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack("<%dh" % n, frame[:n * 2])
    return math.sqrt(sum(s * s for s in samples) / n)


class ChunkedBody:
    """Incremental reader for a chunked-transfer request body."""

    def __init__(self, rfile):
        self.rfile = rfile
        self.buf = b""
        self.done = False

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.rfile.read(n - len(self.buf))
            if not chunk:
                raise ConnectionError("body closed early")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.rfile.read(1024)
            if not chunk:
                raise ConnectionError("body closed early")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_chunk(self):
        """Next data chunk (bytes), or None at the terminal chunk."""
        if self.done:
            return None
        line = self._read_line()
        try:
            size = int(line.split(b";")[0].strip(), 16)
        except ValueError:
            raise ConnectionError("bad chunk size line")
        if size == 0:
            self.done = True
            # swallow one optional trailer line set ending in CRLF
            try:
                self.rfile.read(2)
            except Exception:
                pass
            return None
        data = self._read_exact(size)
        self._read_exact(2)  # trailing CRLF after the chunk data
        return data


class WakeSpotter:
    """Bridge-side wake-phrase detection over the streamed audio.

    Keeps the last ~2.4 s of PCM; every ~0.55 s of new audio it runs the
    phrase grammar over the last ~1.8 s. This is detection on streamed
    audio by the bridge computer -- NOT an on-device wake word, and the
    ears report says exactly that."""

    WINDOW_BYTES = int(PCM_RATE * 2 * 1.8)
    KEEP_BYTES = int(PCM_RATE * 2 * 2.4)
    STEP_BYTES = int(PCM_RATE * 2 * 0.55)

    def __init__(self, tag):
        self.tag = tag
        self.buf = bytearray()
        self.since_spot = 0
        self.spots = 0

    def feed(self, data):
        self.buf += data
        if len(self.buf) > self.KEEP_BYTES:
            del self.buf[:len(self.buf) - self.KEEP_BYTES]
        self.since_spot += len(data)
        if self.since_spot < self.STEP_BYTES or len(self.buf) < PCM_RATE * 2:
            return None
        self.since_spot = 0
        self.spots += 1
        window = bytes(self.buf[-self.WINDOW_BYTES:])
        wav = os.path.join(EARS_TMP_DIR, "spot-%s-%d.wav" % (self.tag, self.spots))
        try:
            write_wav(wav, window)
            kind, conf, _ = run_asr("spot", wav, WAKE_PHRASE)
        finally:
            try:
                os.unlink(wav)
            except OSError:
                pass
        if kind == "MATCH":
            return conf
        return None


class VadListener:
    """Energy VAD over the ask stream: finds the utterance's end.

    The board streams while he is still speaking; this class decides
    where the speech starts and where it ends (700 ms of trailing quiet
    after at least 350 ms of speech). The noise floor is measured from
    the stream's own first frames."""

    FRAME = 640  # 20 ms at 16 kHz 16-bit mono
    END_QUIET_MS = 700
    MIN_SPEECH_MS = 350
    MAX_MS = 15000

    def __init__(self):
        self.pending = bytearray()
        self.audio = bytearray()
        self.floor = None
        self.floor_frames = []
        self.speech_ms = 0
        self.quiet_ms = 0
        self.started = False
        self.total_ms = 0
        self.t_first_frame = None
        self.t_speech_start = None
        self.t_speech_end = None

    def feed(self, data):
        """Returns 'ended' when the utterance is complete, else None."""
        if self.t_first_frame is None:
            self.t_first_frame = epoch_ms()
        self.pending += data
        while len(self.pending) >= self.FRAME:
            frame = bytes(self.pending[:self.FRAME])
            del self.pending[:self.FRAME]
            self.total_ms += 20
            rms = frame_rms(frame)
            if self.floor is None:
                self.floor_frames.append(rms)
                if len(self.floor_frames) >= 15:
                    self.floor = max(60.0, sum(self.floor_frames) / len(self.floor_frames))
                continue
            thresh = max(self.floor * 3.0, 220.0)
            loud = rms > thresh
            if loud:
                if not self.started:
                    self.started = True
                    self.t_speech_start = epoch_ms()
                self.speech_ms += 20
                self.quiet_ms = 0
                self.audio += frame
            elif self.started:
                self.audio += frame
                self.quiet_ms += 20
                if self.quiet_ms >= self.END_QUIET_MS and \
                        self.speech_ms >= self.MIN_SPEECH_MS:
                    self.t_speech_end = epoch_ms()
                    # trim the trailing quiet we appended
                    keep_ms = self.speech_ms + 150
                    self.audio = self.audio[:int(keep_ms * 32)]
                    return "ended"
            if self.total_ms >= self.MAX_MS and self.started:
                self.t_speech_end = epoch_ms()
                return "ended"
        return None


def generate_reply(transcript):
    """Potion's answer text for one recognized utterance.

    A bounded bench brain over the bridge's own LIVE data (clock, the
    machines chain probed this minute, the countdown epoch, the crew
    file, today's press count). Anything outside the fence gets an
    honest fallback that says so -- never an invented fact. The seam is
    this one function: a richer brain (an LLM endpoint) drops in here
    without touching the stream, ASR, or playback paths."""
    t = transcript.lower()
    now = datetime.now()
    if "time" in t:
        return ("It's %s." % now.strftime("%I:%M %p").lstrip("0"), "time")
    if ("date" in t or "today" in t) and "how many" not in t:
        return ("Today is %s." % now.strftime("%A, %B %d"), "date")
    if "deadline" in t or "countdown" in t or "how long" in t or "submit" in t:
        try:
            with open(HACKATHONS_PATH, "r", encoding="utf-8") as f:
                events = json.load(f).get("events", [])
            epoch = None
            for ev in events:
                if ev.get("key") == "meta":
                    epoch = ev.get("deadline_epoch")
            if epoch:
                rem = int(epoch - time.time())
                if rem <= 0:
                    return ("The submission deadline has passed.", "deadline")
                d, rem = divmod(rem, 86400)
                h, rem = divmod(rem, 3600)
                m = rem // 60
                return ("%d days, %d hours and %d minutes left "
                        "until the deadline." % (d, h, m), "deadline")
        except Exception as e:
            ears_log("deadline intent failed: %r" % (e,))
        return ("I couldn't read the deadline just now.", "deadline")
    if "peer" in t or "machine" in t or "bridge" in t:
        md = machines_data()
        lines = ["%s is %s" % (row["name"].split(" - ")[0], row["line"].lower())
                 for row in md["machines"]]
        return ("Machines: " + "; ".join(lines) + ".", "machines")
    if "crew" in t:
        cd = crew_data()
        if cd.get("ok"):
            return ("The crew board shows %d seats, boss. %s"
                    % (len(cd["seats"]), cd.get("title", "")), "crew")
        return ("I couldn't read the crew board just now.", "crew")
    if "how many" in t and ("press" in t or "talk" in t or "times" in t):
        return ("You have talked to me %d times today, boss."
                % presses_today(), "presses")
    if "your name" in t or "who are you" in t:
        return ("I'm Potion. This little box is Sesame.", "identity")
    if "thank" in t:
        return ("Any time, boss.", "thanks")
    if "hello" in t or "hi " in t or t.startswith("hi"):
        return ("Hello, boss. I'm here.", "hello")
    return ("I heard you, boss, but that one is not in my "
            "bench brain yet.", "fallback")


def plan_for(transcript, conf):
    """The voiced plan for an exchange: app-voice clips (with captions)
    plus the written answer for the face screen."""
    if not transcript or conf < 0.20:
        reply = "Sorry, boss -- I did not catch that."
        clips = ["ears-sorry.wav"]
        intent = "nocatch"
    else:
        reply, intent = generate_reply(transcript)
        clips = ["ears-gotit.wav"]
    plan = []
    for name in clips:
        plan.append({
            "clip": name,
            "caption": clip_caption(name),
            "ms": clip_play_ms(os.path.join(VOICE_DIR, name)),
        })
    return plan, reply, intent


def play_plan_room(clips):
    """Room-route playback by the bridge itself (room speaker), sequential.
    Returns the epoch ms at which the first clip started."""
    started = epoch_ms()
    for clip in clips:
        path = os.path.join(VOICE_DIR, clip["clip"])
        try:
            ok = ctypes.windll.winmm.PlaySoundW(path, None, 0x00020000 | 0x00000001)
        except Exception as e:
            log("ears playsound failed: %r" % (e,))
            ok = 0
        if not ok:
            log("ears playsound returned 0 for %s" % clip["clip"])
        time.sleep(max(0, clip["ms"]) / 1000.0 + 0.05)
    return started


def backup_index():
    out = []
    for name, path in BACKUP_FILES:
        try:
            with open(path, "rb") as f:
                data = f.read()
            out.append({"name": name, "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
        except OSError as e:
            out.append({"name": name, "error": str(e)})
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep stdout clean; we log our own lines
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, code, data, content_type):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _token_ok(self):
        return self.headers.get("X-Potion-Token") == TOKEN

    def do_GET(self):
        now = datetime.now()
        if self.path == "/health":
            self._send(200, {
                "ok": True,
                "time": now.isoformat(timespec="seconds"),
                "uptime_s": int(time.time() - START),
            })
        elif self.path.startswith("/voice/"):
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            name = os.path.basename(self.path[len("/voice/"):])
            fpath = os.path.join(VOICE_DIR, name)
            if not name.endswith(".wav") or not os.path.isfile(fpath):
                self._send(404, {"ok": False, "error": "no clip"})
                return
            with open(fpath, "rb") as f:
                data = f.read()
            log("voice served name=%s bytes=%d" % (name, len(data)))
            self._send_bytes(200, data, "audio/wav")
        elif self.path == "/face":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            fpath = os.path.join(VOICE_DIR, "potion-face-320x240.jpg")
            if not os.path.isfile(fpath):
                self._send(404, {"ok": False, "error": "no face"})
                return
            with open(fpath, "rb") as f:
                data = f.read()
            log("face served bytes=%d" % len(data))
            self._send_bytes(200, data, "image/jpeg")
        elif self.path == "/hackathons":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            self._send(200, hackathons_data())
        elif self.path == "/crew":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            self._send(200, crew_data())
        elif self.path == "/machines":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            self._send(200, machines_data())
        elif self.path == "/backup/index":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            self._send(200, {"ok": True, "files": backup_index()})
        elif self.path.startswith("/backup/file"):
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            name = ""
            if "?" in self.path:
                for pair in self.path.split("?", 1)[1].split("&"):
                    if pair.startswith("name="):
                        name = pair[5:]
            path = dict(BACKUP_FILES).get(name)
            if not path or not os.path.isfile(path):
                self._send(404, {"ok": False, "error": "no such backup file"})
                return
            with open(path, "rb") as f:
                data = f.read()
            log("backup served name=%s bytes=%d" % (name, len(data)))
            self._send_bytes(200, data, "application/octet-stream")
        elif self.path == "/board":
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                return
            self._send(200, {
                "ok": True,
                "time": now.strftime("%H:%M:%S"),
                "date": now.strftime("%a, %b %d, %Y"),
                "uptime_s": int(time.time() - START),
                "presses_today": presses_today(),
            })
        else:
            self._send(404, {"ok": False, "error": "not found"})

    # ------------------------------------------------------- ears (v1.22)
    def _ears_watch(self):
        """One wake-watch stream: PCM flows in until the phrase grammar
        fires (bridge-side spotting) or the board hangs up."""
        tag = "%d" % int(time.time() * 1000)
        spotter = WakeSpotter(tag)
        body = ChunkedBody(self.rfile)
        t0 = epoch_ms()
        ears_log("watch start tag=%s" % tag)
        wake_conf = None
        try:
            while epoch_ms() - t0 < 240000:  # 4-minute watch cap
                chunk = body.read_chunk()
                if chunk is None:
                    break
                got = spotter.feed(chunk)
                if got is not None:
                    wake_conf = got
                    break
        except (ConnectionError, BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            ears_log("watch error tag=%s: %r" % (tag, e))
        if wake_conf is not None:
            record_press("ears-wake")
            ears_log("WAKE tag=%s conf=%.2f after_ms=%d spots=%d"
                     % (tag, wake_conf, epoch_ms() - t0, spotter.spots))
            self.close_connection = True
            self._send(200, {"ok": True, "wake": True,
                             "conf": round(wake_conf, 2)})
        else:
            ears_log("watch end tag=%s no wake (spots=%d)" % (tag, spotter.spots))
            self.close_connection = True
            self._send(200, {"ok": True, "wake": False})

    def _ears_ask(self):
        """One question stream: VAD finds the end, ASR transcribes, the
        brain answers, the voiced plan plays (room) or is returned for
        the board to play (local). Every stage is timestamped."""
        tag = "%d" % int(time.time() * 1000)
        vad = VadListener()
        body = ChunkedBody(self.rfile)
        t_rx = epoch_ms()
        ended = False
        try:
            while epoch_ms() - t_rx < 30000:
                chunk = body.read_chunk()
                if chunk is None:
                    break
                if vad.feed(chunk) == "ended":
                    ended = True
                    break
        except (ConnectionError, BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            ears_log("ask stream error tag=%s: %r" % (tag, e))
        self.close_connection = True
        if not ended and vad.started and vad.speech_ms >= vad.MIN_SPEECH_MS:
            vad.t_speech_end = epoch_ms()
            ended = True
        if not ended or not vad.audio:
            ears_log("ask tag=%s no speech heard" % tag)
            self._send(200, {"ok": False, "error": "no speech",
                             "timings": {"rx_ms": t_rx}})
            return
        wav = os.path.join(EARS_TMP_DIR, "ask-%s.wav" % tag)
        try:
            write_wav(wav, bytes(vad.audio))
            kind, conf, text = run_asr("hear", wav)
        finally:
            try:
                os.unlink(wav)
            except OSError:
                pass
        t_asr = epoch_ms()
        transcript = text if kind == "TEXT" else ""
        plan, reply, intent = plan_for(transcript, conf)
        t_reply = epoch_ms()
        route = "local"
        bt = default_output_name()
        if bt is not None and bt == BT_SPEAKER:
            route = "room"
        play_start = 0
        if route == "room":
            play_start = play_plan_room(plan)
        t_play = epoch_ms()
        record_press("ears-ask")
        timings = {
            "speech_start_ms": vad.t_speech_start or 0,
            "speech_end_ms": vad.t_speech_end or 0,
            "asr_done_ms": t_asr,
            "reply_ready_ms": t_reply,
            "play_start_ms": play_start,
            "end_to_asr_ms": t_asr - (vad.t_speech_end or t_asr),
            "end_to_reply_ms": t_reply - (vad.t_speech_end or t_reply),
            "end_to_play_ms": (play_start - (vad.t_speech_end or play_start))
                               if play_start else -1,
        }
        ears_log("ask tag=%s transcript=%r conf=%.2f intent=%s route=%s "
                 "end_to_asr=%dms end_to_reply=%dms end_to_play=%dms"
                 % (tag, transcript, conf, intent, route,
                    timings["end_to_asr_ms"], timings["end_to_reply_ms"],
                    timings["end_to_play_ms"]))
        self._send(200, {
            "ok": True, "transcript": transcript, "conf": round(conf, 2),
            "reply_text": reply, "intent": intent, "route": route,
            "play_started_ms": play_start, "clips": plan,
            "timings": timings,
        })

    def do_POST(self):
        if self.path in ("/ears/watch", "/ears/ask"):
            if not self._token_ok():
                self._send(403, {"ok": False, "error": "bad token"})
                log("ears REJECTED (bad token) from %s" % self.client_address[0])
                return
            if self.path == "/ears/watch":
                self._ears_watch()
            else:
                self._ears_ask()
            return
        if self.path not in ("/press", "/hello"):
            self._send(404, {"ok": False, "error": "not found"})
            return
        if not self._token_ok():
            self._send(403, {"ok": False, "error": "bad token"})
            log("event REJECTED (bad token) from %s" % self.client_address[0])
            return
        event = "hello" if self.path == "/hello" else "press"
        source = "board-boot" if event == "hello" else "board"
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length > 0:
            try:
                data = json.loads(self.rfile.read(length).decode("utf-8", "replace"))
                if isinstance(data, dict) and data.get("source"):
                    source = str(data["source"])[:32]
            except ValueError:
                pass
        n = record_press(source)
        route, play_ms = decide_route(CLIP_FOR_EVENT[event])
        if route == "local":
            log("route=local event=%s (board plays %s)" % (event, CLIP_FOR_EVENT[event]))
        self._send(200, {"ok": True,
                         "ack": "Hello" if event == "hello" else "Talk sent",
                         "route": route, "play_ms": play_ms,
                         "presses_today": n})


def main():
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    log("bridge starting on %s:%d" % (HOST, PORT))
    print("bridge listening on %s:%d" % (HOST, PORT), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
