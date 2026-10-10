"""
Pocket Nova Panel - the PC companion for Pocket Nova (M5Stack Atom Matrix).

Runs quietly in the background (started with Windows). It watches the USB
ports, and the moment Pocket Nova is plugged in it opens the control panel
in its own window. The panel talks to the device through this program.

With no cable, it falls back to Bluetooth: if this PC is paired with
Pocket Nova, it talks to the device's remote service instead (everything
except firmware updates, which need the cable). USB always wins.

    pythonw pocketnova_panel.py          background watcher (no window until plugged in)
    pythonw pocketnova_panel.py --open   ...and open the panel right away

The panel itself is panel.html, served only to this PC (127.0.0.1).
"""

import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor, wait
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import serial
from serial.tools import list_ports

try:                                  # Bluetooth fallback (optional: pip install bleak)
    import asyncio
    from bleak import BleakClient, BleakScanner
    HAVE_BLEAK = True
except ImportError:
    HAVE_BLEAK = False

# Pocket Nova's remote service (NovaRemote.h on the device).
REMOTE_SVC = "8f3c0001-5b2a-4c8e-9a71-2f6d1e0b9a10"
REMOTE_CMD = "8f3c0002-5b2a-4c8e-9a71-2f6d1e0b9a10"
REMOTE_OUT = "8f3c0003-5b2a-4c8e-9a71-2f6d1e0b9a10"

APP_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = APP_DIR   # settings + log live next to the app (Store Python virtualises AppData)
SETTINGS_PATH = os.path.join(DATA_DIR, "settings.json")
LOG_PATH = os.path.join(DATA_DIR, "panel.log")
HISTORY_PATH = os.path.join(DATA_DIR, "history.json")
HTTP_PORT = 47800
BAUD = 115200

# USB-serial chips used by Atom boards (FTDI on older ones, WCH/Silabs on newer).
KNOWN_USB = {(0x0403, 0x6001), (0x1A86, 0x55D4), (0x1A86, 0x7523), (0x10C4, 0xEA60)}

# Commands the panel page may forward to the device.
ALLOWED_CMDS = {
    "hello", "get", "status", "bonds", "set", "flip", "unbond", "unbond_all", "slot",
    "factory_reset", "reboot", "input", "tilt", "ir", "time", "mirror", "tutorial", "pet",
    "tvbrands", "findtv", "wifi", "keys", "keytest", "netscan", "block",
}

STARTUP_LNK = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs\Startup",
                           "Pocket Nova Panel.lnk")


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ---------------------------------------------------------------- settings

DEFAULT_SETTINGS = {
    "autoOpen": True,                                   # open the panel when plugged in
    "labels": {},                                       # Bluetooth address -> your name for it
    "netDevices": {},                                   # Wi-Fi MAC -> {label, name, first, last, ip, mine}
    "historyDays": 30,                                  # forget history older than this (0 = keep forever)
    "firmwareDir": os.path.join(os.path.expanduser("~"), "Downloads", "PocketNova", "firmware", "PocketNova"),
    "arduinoCli": r"C:\Program Files\Arduino CLI\arduino-cli.exe",
}


def load_settings():
    s = dict(DEFAULT_SETTINGS)
    try:
        # utf-8-sig: install.ps1 (Windows PowerShell) writes the file with a
        # byte-order mark, which plain "utf-8" chokes on, and then every
        # setting was quietly lost (known devices, names...).
        with open(SETTINGS_PATH, encoding="utf-8-sig") as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = SETTINGS_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    os.replace(tmp, SETTINGS_PATH)


settings = load_settings()
settings_lock = threading.Lock()


# ---------------------------------------------------------------- history

class History:
    """Every event Pocket Nova reports (joins, tries, wrong codes...) plus
    new devices found on the home network, kept in history.json."""

    MAX = 2000

    def __init__(self):
        self.lock = threading.Lock()
        self.items, self.boot, self.last = [], None, 0
        try:
            with open(HISTORY_PATH, encoding="utf-8") as f:
                saved = json.load(f)
            self.items = saved.get("items", [])[-self.MAX:]
            self.boot = saved.get("boot")     # the device start we're reading
            self.last = saved.get("last", 0)  # newest event number read from it
        except (OSError, ValueError, AttributeError):
            pass

    def _prune(self):
        days = settings.get("historyDays", 30)
        if days:
            cutoff = time.time() - days * 86400
            self.items = [e for e in self.items if e.get("t", 0) >= cutoff]
        del self.items[:-self.MAX]

    def add(self, item):
        with self.lock:
            self.items.append(item)
            self._prune()
            self._save()

    def _save(self):
        tmp = HISTORY_PATH + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"boot": self.boot, "last": self.last, "items": self.items}, f)
            os.replace(tmp, HISTORY_PATH)
        except OSError:
            pass

    def from_device(self, msg):
        """Takes a @{"t":"history"} reply; returns True if there's more to fetch."""
        if msg.get("boot") != self.boot:      # Pocket Nova restarted: its numbers start again
            self.boot, self.last = msg.get("boot"), 0
            return True                       # ask again from #0
        now = time.time()
        fresh = [e for e in msg.get("list", []) if e.get("n", 0) > self.last]
        with self.lock:
            for e in fresh:
                item = {"t": round(now - e.get("ago", 0) / 1000), "k": e.get("k"), "mac": e.get("mac", ""),
                        "ip": e.get("ip", ""), "note": e.get("note", "")}
                if e.get("pass"):
                    item["pass"] = e["pass"]
                if item["k"] == "joined":       # that Connect worked: its password is a real one, drop it
                    for old in reversed(self.items):
                        if old.get("k") == "try" and old.get("ip") == item["ip"]:
                            old.pop("pass", None)
                            break
                self.items.append(item)
                self.last = e["n"]
            if fresh:
                self._prune()
                self._save()
        return self.last < msg.get("last", 0)

    def clear(self, older_than_days=None, kinds=None):
        """Everything, or only events older than N days, or only some kinds."""
        with self.lock:
            if older_than_days:
                cutoff = time.time() - older_than_days * 86400
                self.items = [e for e in self.items if e.get("t", 0) >= cutoff]
            elif kinds:
                self.items = [e for e in self.items if e.get("k") not in kinds]
            else:
                self.items = []
            self._save()

    def all(self):
        with self.lock:
            return list(self.items)

    def recent(self, n=300):
        with self.lock:
            return self.items[-n:]


history = History()


# ---------------------------------------------------------------- network devices

def _dns_name(ip):
    """Reverse DNS: ask the router "who is 192.168.1.23?". Most home routers
    answer with the name the device gave when it got its address (DHCP)."""
    try:
        name = socket.gethostbyaddr(ip)[0]
    except OSError:
        return ""
    return "" if name == ip else name.split(".")[0]


def _dns_read_name(buf, i):
    """Reads a DNS name at buf[i] (following compression pointers); returns (name, next index)."""
    labels, end, hops = [], None, 0
    while i < len(buf) and hops < 20:
        n = buf[i]
        if n == 0:
            i += 1
            break
        if n >= 0xC0:                       # pointer to a name earlier in the packet
            if end is None:
                end = i + 2
            i = ((n & 0x3F) << 8) | buf[i + 1]
            hops += 1
            continue
        labels.append(buf[i + 1:i + 1 + n].decode("utf-8", "replace"))
        i += 1 + n
    return ".".join(labels), (end if end is not None else i)


def _mdns_name(ip):
    """mDNS: asks the device itself "what's your name?" (the "who is
    23.1.168.192.in-addr.arpa" question, sent straight to it on port 5353).
    iPhones, Macs, Chromecasts and many Androids answer, e.g. "Unos-iPhone"."""
    q = b"".join(bytes([len(p)]) + p.encode() for p in reversed(ip.split("."))) + b"\x07in-addr\x04arpa\x00"
    pkt = struct.pack(">HHHHHH", 0x4E56, 0, 1, 0, 0, 0) + q + struct.pack(">HH", 12, 0x8001)   # PTR, "answer me directly"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.5)
    try:
        s.sendto(pkt, (ip, 5353))
        buf = s.recv(1500)
        an = struct.unpack(">H", buf[6:8])[0]
        _, i = _dns_read_name(buf, 12)
        i += 4                              # the question's type and class
        for _ in range(an):
            _, i = _dns_read_name(buf, i)
            rtype, _, _, rlen = struct.unpack(">HHIH", buf[i:i + 10])
            i += 10
            if rtype == 12:
                name = _dns_read_name(buf, i)[0]
                return name[:-6] if name.endswith(".local") else name
            i += rlen
    except (OSError, struct.error, IndexError):
        pass
    finally:
        s.close()
    return ""


def _netbios_name(ip):
    """NetBIOS: the old Windows "what's your name?" (UDP port 137). Windows
    PCs, many printers and NAS boxes answer with their computer name."""
    pkt = struct.pack(">HHHHHH", 0x4E57, 0, 1, 0, 0, 0) + b"\x20CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\x00" + struct.pack(">HH", 0x21, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.sendto(pkt, (ip, 137))
        buf = s.recv(1500)
        count = buf[56]
        for k in range(count):
            e = buf[57 + 18 * k:57 + 18 * (k + 1)]
            flags = struct.unpack(">H", e[16:18])[0]
            if e[15] == 0 and not flags & 0x8000:   # the machine's own name, not a group
                return e[:15].decode("ascii", "replace").strip()
    except (OSError, struct.error, IndexError):
        pass
    finally:
        s.close()
    return ""


# Names many devices share ("Android", "iPhone"...): fine to show, useless
# for telling one device from another.
GENERIC_NAMES = {"android", "iphone", "ipad", "localhost", "unknown", "espressif", "esp32", "galaxy"}


def _unique_name(name):
    return bool(name) and name.lower().split("-")[0] not in GENERIC_NAMES and not name.lower().startswith("android-")


def _my_ips():
    try:
        return {a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    except OSError:
        return set()


def enrich_netscan(msg):
    """Adds names and "new" flags to a device scan from Pocket Nova, and
    remembers every device seen so the next scan can spot newcomers."""
    found = [dict(d) for d in msg.get("list", [])]
    if msg.get("myMac"):
        found.append({"ip": msg.get("me", ""), "mac": msg["myMac"], "nova": True})
    # Three ways to ask a device's name, all at once (the router's can take
    # 5 s to give up); the best answer wins: router, then mDNS, then NetBIOS.
    ways = (_dns_name, _mdns_name, _netbios_name)
    pool = ThreadPoolExecutor(32)
    jobs = {pool.submit(f, d["ip"]): (rank, d) for d in found if d.get("ip") and not d.get("nova")
            for rank, f in enumerate(ways)}
    done, _ = wait(jobs, timeout=6)            # slow answers are skipped, not waited for
    pool.shutdown(wait=False)
    best = {}
    for j in done:
        rank, d = jobs[j]
        name = j.result()
        if name and rank < best.get(id(d), (9,))[0]:
            best[id(d)] = (rank, name)
            d["name"] = name
    mine, now = _my_ips(), int(time.time())
    with settings_lock:
        book = settings.setdefault("netDevices", {})
        first_scan = not book
        for d in found:
            mac = d["mac"].upper()
            entry = book.get(mac)
            if entry is None:
                entry = book[mac] = {"label": "", "first": now, "mine": first_scan or bool(d.get("nova"))}
                # SAME DEVICE, NEW ADDRESS: a phone that changed its private
                # address still answers with its own name. If that name belongs
                # to a device we already know, it's that device, not a newcomer.
                twin = next((m for m, e in book.items() if m != mac and _unique_name(d.get("name", ""))
                             and e.get("name", "").lower() == d["name"].lower()), None)
                if twin:
                    old = book[twin]
                    entry.update(label=old.get("label", ""), mine=old.get("mine", False),
                                 first=old.get("first", now), sameAs=twin)
                    d["linked"] = twin
                else:
                    d["new"] = not first_scan
            entry.update(last=now, ip=d["ip"])
            if d.get("name"):
                entry["name"] = d["name"]
            d.setdefault("new", False)
            d["name"] = entry.get("name", "")
            d["label"] = entry.get("label", "")
            d["first"] = entry["first"]
            d["mine"] = entry.get("mine", False) or bool(entry.get("label"))
            d["router"] = d["ip"] == msg.get("router")
            d["pc"] = d["ip"] in mine
            if d["router"] or d["pc"] or d.get("nova"):   # obviously yours
                entry["mine"] = d["mine"] = True
                d["new"] = False
            if d["new"]:
                history.add({"t": now, "k": "home_new", "mac": mac, "ip": d["ip"], "note": d.get("name", "")})
        save_settings(settings)
    return {"at": now, "ssid": msg.get("ssid", ""), "router": msg.get("router", ""),
            "list": found, "firstScan": first_scan}


# ---------------------------------------------------------------- Bluetooth link

class BleLink:
    """Talks to Pocket Nova's remote service over Bluetooth, in its own thread.

    Replies arrive as notifications in packet-sized pieces; a newline ends
    each line. Lines are handed to on_line(), the same as USB lines."""

    def __init__(self, on_line, on_up, on_down):
        self.on_line, self.on_up, self.on_down = on_line, on_up, on_down
        self.connected = False
        self.name = ""
        self.address = ""
        self.last_error = ""
        self._loop = None
        self._client = None
        self._buf = ""
        self._busy = False

    def start(self):
        if not HAVE_BLEAK:
            return
        threading.Thread(target=self._thread, daemon=True).start()

    def _thread(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def try_connect(self):
        """Look for Pocket Nova and connect (runs in the background)."""
        if self._loop and not self.connected and not self._busy:
            self._busy = True
            asyncio.run_coroutine_threadsafe(self._connect(), self._loop)

    async def _connect(self):
        try:
            dev = await BleakScanner.find_device_by_filter(
                lambda d, a: REMOTE_SVC in [u.lower() for u in a.service_uuids], timeout=5)
            if not dev:
                # The service ID is in the second broadcast packet (the scan
                # response), which is easy to miss at weak signal. A device
                # we've linked to before can be reached by its address.
                with settings_lock:
                    known = settings.get("bleAddress")
                if not known:
                    self.last_error = "not found nearby"
                    return
                dev = known
            # Ask for just the remote service, read fresh from the device.
            # Windows keeps an old copy of a paired device's services, and a
            # fresh read of ALL of them fails while the keyboard driver holds
            # it ("catastrophic failure"); a fresh read of one service works.
            client = BleakClient(dev, services=[REMOTE_SVC], winrt={"use_cached_services": False},
                                 disconnected_callback=lambda c: self._gone())
            await client.connect(timeout=12)
            if not any(sv.uuid.lower() == REMOTE_SVC for sv in client.services):
                self.last_error = "its remote service didn't answer (update the firmware over USB)"
                await client.disconnect()
                return
            await client.start_notify(REMOTE_OUT, self._notify)
            self._client, self.connected = client, True
            self.address = dev if isinstance(dev, str) else dev.address
            self.name = getattr(dev, "name", None) or "Pocket Nova"
            with settings_lock:
                if settings.get("bleAddress") != self.address:
                    settings["bleAddress"] = self.address
                    save_settings(settings)
            self.last_error = ""
            log(f"Connected over Bluetooth to {self.name} ({self.address})")
            # on_up() sends commands, and sending waits on this event loop,
            # so it must run on another thread or it would wait for itself.
            threading.Thread(target=self.on_up, daemon=True).start()
        except Exception as e:
            self.last_error = f"{type(e).__name__}: {e}" if str(e) else type(e).__name__
        finally:
            self._busy = False

    def _notify(self, _char, data):
        self._buf += bytes(data).decode("utf-8", "replace")
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            line = line.strip()
            if line:
                self.on_line(line if line.startswith("@") else "@" + line)

    def _gone(self):
        if self.connected:
            self.connected = False
            self._client = None
            log("Bluetooth link closed")
            self.on_down()

    def send(self, obj):
        if not (self.connected and self._client):
            return False
        data = json.dumps(obj, separators=(",", ":")).encode()
        if len(data) > 190:                    # the device reads up to ~200 bytes per command
            return False
        fut = asyncio.run_coroutine_threadsafe(
            self._client.write_gatt_char(REMOTE_CMD, data, response=True), self._loop)
        try:
            fut.result(timeout=4)
            return True
        except Exception:
            return False

    def close(self):
        if self._client and self._loop:
            asyncio.run_coroutine_threadsafe(self._client.disconnect(), self._loop)


# ---------------------------------------------------------------- device link

class DeviceLink:
    """Finds Pocket Nova on a USB serial port (or, failing that, over
    Bluetooth) and keeps talking to it."""

    def __init__(self):
        self.ser = None
        self.port = None
        self.lock = threading.Lock()          # guards writes + state
        self.state = {}                       # hello / config / status / bonds / fb
        self.logs = deque(maxlen=500)         # (seq, text)
        self.replies = deque(maxlen=50)       # (seq, reply dict) for toasts
        self.seq = 0
        self.connected_at = 0.0
        self.paused = False                   # True while a firmware update owns the port
        self.ui_seen = 0.0                    # last time the panel page asked for state
        self.mirror_on = False
        self._buf = b""
        self._next_status = 0.0
        self._next_bonds = 0.0
        self._next_time = 0.0
        self._next_hist = 0.0
        self._failed = {}                     # port -> time of last failed probe
        self.ble = BleLink(self._ble_line, self._ble_up, self._ble_down)
        self._next_ble = 0.0
        self.ble_lost_at = 0.0                # when the Bluetooth link last dropped
        self.ble.start()

    # ---- helpers
    def _add_log(self, text):
        self.seq += 1
        self.logs.append((self.seq, text))

    def connected(self):
        return self.ser is not None or self.ble.connected

    def transport(self):
        return "usb" if self.ser is not None else "bluetooth" if self.ble.connected else ""

    def ble_reconnecting(self):
        """A short Bluetooth drop: keep showing the last state while it relinks."""
        return self.ser is None and not self.ble.connected and time.time() - self.ble_lost_at < 20

    # ---- Bluetooth callbacks (from the Bluetooth thread)
    def _ble_line(self, line):
        if self.ser is None:                  # USB wins if both are up
            self._handle_line(line)

    def _ble_up(self):
        quick = time.time() - self.ble_lost_at < 20
        self.ble_lost_at = 0.0
        with self.lock:
            if not quick:
                self.state = {}
            self._add_log("-- Bluetooth link back" if quick else f"-- Connected to {self.ble.name} over Bluetooth (no cable)")
        self.send({"cmd": "hello"})
        self._after_connect()

    def _ble_down(self):
        self.mirror_on = False
        if self.ser is None:
            self.ble_lost_at = time.time()
            self._next_ble = time.time() + 2      # try again soon: drops are usually brief
            with self.lock:
                self._add_log("-- Bluetooth link dropped, reconnecting...")

    def ui_active(self):
        return time.time() - self.ui_seen < 5

    def send(self, obj):
        if not self.ser:
            return self.ble.send(obj)
        try:
            with self.lock:
                self.ser.write((json.dumps(obj, separators=(",", ":")) + "\n").encode())
            return True
        except (serial.SerialException, OSError):
            self._drop("write failed")
            return False

    def _drop(self, why):
        if self.ser:
            log(f"Disconnected from {self.port}: {why}")
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.port = None
        self.mirror_on = False
        with self.lock:
            self.state = {}
            self._add_log(f"-- Pocket Nova disconnected ({why})")

    # ---- discovery
    def usb_present(self):
        """Is a Pocket Nova-type USB port plugged in? (checked at most every 2 s)"""
        now = time.time()
        if now - getattr(self, "_usb_at", 0) > 2:
            self._usb_at, self._usb = now, next(self._candidates(), None) is not None
        return self._usb

    def _candidates(self):
        for p in list_ports.comports():
            if (p.vid, p.pid) in KNOWN_USB:
                yield p.device

    def _probe(self, dev):
        """Open a port without resetting the board and ask who it is."""
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = dev, BAUD, 0.1
        s.dtr = False   # keep these low: toggling them resets an ESP32
        s.rts = False
        try:
            s.open()
        except (serial.SerialException, OSError):
            return None
        try:
            s.reset_input_buffer()
            buf = b""
            for attempt in range(3):
                s.write(b'{"cmd":"hello"}\n')
                end = time.time() + 1.2
                while time.time() < end:
                    buf += s.read(4096)
                    for line in buf.split(b"\n"):
                        line = line.strip()
                        if line.startswith(b'@{"t":"hello"'):
                            try:
                                hello = json.loads(line[1:].decode("utf-8", "replace"))
                            except ValueError:
                                continue
                            if hello.get("device") == "PocketNova":
                                return s, hello
                time.sleep(0.2)
        except (serial.SerialException, OSError):
            pass
        try:
            s.close()
        except Exception:
            pass
        return None

    def _after_connect(self):
        now = time.time()
        offset = -time.altzone if time.localtime().tm_isdst > 0 else -time.timezone
        self.send({"cmd": "time", "epoch": int(now), "tz": int(offset)})
        self.send({"cmd": "get"})
        self.send({"cmd": "bonds"})
        self.send({"cmd": "tvbrands"})
        self.send({"cmd": "keys"})
        self.send({"cmd": "status"})
        self.mirror_on = False
        self._next_time = now + 600
        self._next_hist = 0.0                   # catch up on what happened while we were away

    def scan(self):
        for dev in self._candidates():
            if time.time() - self._failed.get(dev, 0) < 6:
                continue
            res = self._probe(dev)
            if not res:
                self._failed[dev] = time.time()
                continue
            self.ser, hello = res
            self.port = dev
            self.connected_at = time.time()
            with self.lock:
                self.state = {"hello": hello, "helloAt": time.time()}
                self._add_log(f"-- Connected to {hello.get('name')} on {dev} (firmware {hello.get('fw')})")
            log(f"Connected on {dev}: {hello}")
            if self.ble.connected:           # the cable is back: it takes over
                self.ble.close()
            addr = hello.get("address")      # remember it for the Bluetooth fallback
            if addr:
                with settings_lock:
                    if settings.get("bleAddress") != addr:
                        settings["bleAddress"] = addr
                        save_settings(settings)
            self._after_connect()
            return True
        return False

    # ---- incoming
    def _handle_line(self, line):
        if line.startswith("@"):
            try:
                msg = json.loads(line[1:])
            except ValueError:
                return
            t = msg.get("t")
            with self.lock:
                if t in ("hello", "config", "status", "bonds", "tvbrands", "keys"):
                    self.state[t] = msg
                    if t == "hello":
                        self.state["helloAt"] = time.time()
                elif t == "history":
                    if history.from_device(msg):
                        self._next_hist = 0.0       # more waiting: ask again right away
                elif t == "fb":
                    self.state["fb"] = msg.get("d")
                elif t == "netscan":            # names come from DNS, which takes a moment
                    self.state["netscanBusy"] = True
                    threading.Thread(target=self._netscan_done, args=(msg,), daemon=True).start()
                elif t in ("ok", "error"):
                    self.seq += 1
                    self.replies.append((self.seq, msg))
            return
        with self.lock:
            self._add_log(line)
        if "ready (firmware" in line:      # the device restarted: say hello again
            self.send({"cmd": "hello"})
            self._after_connect()

    def _netscan_done(self, msg):
        try:
            result = enrich_netscan(msg)
        except Exception as e:                  # never lose the scan over a name lookup
            log(f"netscan: {e}")
            result = {"at": int(time.time()), "list": msg.get("list", [])}
        with self.lock:
            self.state["netscan"] = result
            self.state["netscanBusy"] = False

    def _periodic(self):
        now = time.time()
        if now >= self._next_hist:              # always, panel open or not: it's a log
            fw = (self.state.get("hello") or {}).get("fw", "0")
            try:
                has_history = tuple(int(x) for x in fw.split(".")[:2]) >= (2, 8)   # older firmware doesn't know it
            except ValueError:
                has_history = False
            if has_history:
                self.send({"cmd": "history", "since": history.last})
            self._next_hist = now + 3
        active = self.ui_active()
        if now >= self._next_status:
            self.send({"cmd": "status"})
            fast = 1.5 if self.ser is None else 0.8   # Bluetooth: fewer, slower updates
            self._next_status = now + (fast if active else 5)
        if active and now >= self._next_bonds:
            self.send({"cmd": "bonds"})
            self.send({"cmd": "tvbrands"})
            self._next_bonds = now + 6
        if now >= self._next_time:
            self._after_connect()
        if active != self.mirror_on:        # only stream the screen while someone watches
            if self.send({"cmd": "mirror", "on": active}):
                self.mirror_on = active
                if not active:
                    with self.lock:
                        self.state.pop("fb", None)

    def run(self):
        while True:
            if self.paused:
                time.sleep(0.3)
                continue
            if not self.ser:
                if self.scan():
                    continue
                if self.ble.connected:
                    self._periodic()
                    time.sleep(0.05)
                    continue
                if time.time() >= self._next_ble:   # no cable: look for it over Bluetooth
                    self._next_ble = time.time() + (4 if self.ble_reconnecting() else 15)
                    self.ble.try_connect()
                if not self.ble_reconnecting() and self.ble_lost_at:
                    self.ble_lost_at = 0.0
                    with self.lock:
                        self.state = {}
                time.sleep(1.5)
                continue
            try:
                data = self.ser.read(4096)
            except (serial.SerialException, OSError) as e:
                self._drop(str(e) or "unplugged")
                continue
            if data:
                self._buf += data
                while b"\n" in self._buf:
                    raw, self._buf = self._buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if line:
                        self._handle_line(line)
            self._periodic()

    def release(self):
        """Give the port up (for firmware updates)."""
        self.paused = True
        time.sleep(0.4)
        if self.ser:
            self._drop("firmware update")

    def resume(self):
        self._failed.clear()
        self.paused = False


link = DeviceLink()


# ---------------------------------------------------------------- firmware update

class Updater:
    """Rebuilds the firmware and installs it, in four stages the panel shows:
    build -> upload -> restart -> done (or failed)."""

    WRITING = re.compile(r"Writing at 0x([0-9a-f]+).*\((\d+) ?%\)")

    def __init__(self):
        self.running = False
        self.lines = deque(maxlen=300)
        self.ok = None
        self.stage = ""                       # build / upload / restart / done / failed
        self.pct = 0                          # upload progress of the firmware itself
        self.started = 0.0
        self.stage_at = 0.0
        self.fw = ""                          # the version Pocket Nova reports afterwards
        self.error = ""

    def _stage(self, name):
        self.stage, self.stage_at = name, time.time()

    def info(self):
        return {"running": self.running, "ok": self.ok, "lines": list(self.lines), "stage": self.stage,
                "pct": self.pct, "elapsed": int(time.time() - self.started) if self.started else 0,
                "stageElapsed": int(time.time() - self.stage_at) if self.stage_at else 0,
                "fw": self.fw, "error": self.error}

    def start(self):
        if self.running:
            return False
        threading.Thread(target=self._run, daemon=True).start()
        return True

    def _run(self):
        self.running, self.ok = True, None
        self.lines.clear()
        self.pct, self.fw, self.error, self.started = 0, "", "", time.time()
        self._stage("build")
        cli = settings.get("arduinoCli") or "arduino-cli"
        if not os.path.exists(cli):
            cli = shutil.which("arduino-cli") or cli
        sketch = settings.get("firmwareDir")
        fqbn = "esp32:esp32:m5stack-atom:PartitionScheme=min_spiffs"
        # Not answering but plugged in (a cut-off update leaves it like that):
        # flashing over the cable still works, so use the USB port anyway.
        port = link.port or next(link._candidates(), None)
        try:
            if not port:
                raise RuntimeError("Firmware updates need the USB cable. Plug Pocket Nova in." if link.ble.connected
                                   else "Pocket Nova isn't plugged in. Connect it with a USB-C data cable.")
            if not os.path.isdir(sketch):
                raise RuntimeError(f"Firmware folder not found: {sketch}")
            self.lines.append("Building firmware... (about a minute)")
            self._exec([cli, "compile", "--fqbn", fqbn, sketch])
            self.lines.append(f"Uploading to {port}... (about 2 minutes, keep it plugged in)")
            self._stage("upload")
            link.release()
            self._exec([cli, "upload", "-p", port, "--fqbn", fqbn + ",UploadSpeed=115200", sketch])
            self.pct = 100
            self.lines.append("Installed. Waiting for Pocket Nova to start up...")
            self._stage("restart")
            link.resume()
            self._wait_for_device()
            self.lines.append(f"Done. Pocket Nova is running firmware {self.fw}." if self.fw
                              else "Done. Pocket Nova is restarting.")
            self._stage("done")
            self.ok = True
        except Exception as e:
            self.lines.append(f"FAILED: {e}")
            self.error = str(e)
            self._stage("failed")
            self.ok = False
        finally:
            link.resume()
            self.running = False

    def _wait_for_device(self):
        """Up to 30 s for the fresh firmware to say hello over USB."""
        since = time.time()
        while time.time() - since < 30:
            with link.lock:
                hello = link.state.get("hello") or {}
                fresh = link.state.get("helloAt", 0) > since
            if fresh and link.connected():
                self.fw = hello.get("fw", "")
                return
            time.sleep(0.5)

    def _exec(self, args):
        # Point arduino-cli at its folders explicitly. The tools live in
        # ~/.arduino15 (NOT AppData\Local\Arduino15): installs made from a
        # sandboxed (packaged) app land in its private, virtualised AppData,
        # which programs started by Windows (startup, watchdog) can't see.
        home = os.path.expanduser("~")
        data = os.path.join(home, ".arduino15")
        if not os.path.isdir(os.path.join(data, "packages")):
            data = os.path.join(home, "AppData", "Local", "Arduino15")
        env = dict(os.environ,
                   ARDUINO_DIRECTORIES_DATA=data,
                   ARDUINO_DIRECTORIES_DOWNLOADS=os.path.join(data, "staging"),
                   ARDUINO_DIRECTORIES_USER=os.path.join(home, "Arduino"))
        cfg_file = os.path.join(data, "arduino-cli.yaml")
        if os.path.exists(cfg_file):
            args = args[:2] + ["--config-file", cfg_file] + args[2:]
        p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            m = self.WRITING.search(line)
            if m:                               # "Writing at 0x0001c000... (12 %)"
                if int(m.group(1), 16) >= 0x10000:  # the firmware itself (lower = bootloader bits)
                    self.pct = int(m.group(2))
                continue
            if line:
                self.lines.append(line)
        if p.wait() != 0:
            raise RuntimeError(f"{os.path.basename(args[0])} {args[1]} failed (exit {p.returncode})")


updater = Updater()


# ---------------------------------------------------------------- Windows bits

def edge_path():
    for p in (os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"),
              os.path.expandvars(r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe")):
        if os.path.exists(p):
            return p
    return None


def open_window():
    url = f"http://127.0.0.1:{HTTP_PORT}/"
    edge = edge_path()
    try:
        if edge:
            subprocess.Popen([edge, f"--app={url}", "--window-size=1200,860"],
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        else:
            os.startfile(url)
        log("Opened panel window")
    except OSError as e:
        log(f"Could not open window: {e}")


def autostart_enabled():
    return os.path.exists(STARTUP_LNK)


def set_autostart(on):
    if not on:
        if os.path.exists(STARTUP_LNK):
            os.remove(STARTUP_LNK)
        return
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    if not os.path.exists(pyw):
        pyw = shutil.which("pythonw") or sys.executable
    script = os.path.abspath(__file__)
    ps = (
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:LNK);"
        "$s.TargetPath=$env:PYW;$s.Arguments='\"'+$env:SCRIPT+'\"';"
        "$s.WorkingDirectory=$env:WD;$s.Description='Pocket Nova Panel';$s.Save()"
    )
    env = dict(os.environ, LNK=STARTUP_LNK, PYW=pyw, SCRIPT=script, WD=os.path.dirname(script))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], env=env, check=True,
                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


# ---------------------------------------------------------------- web server

class Handler(BaseHTTPRequestHandler):
    server_version = "PocketNova/2"

    def log_message(self, *a):
        pass

    def _host_ok(self):
        # Refuse requests that didn't come to our own address (DNS-rebinding guard).
        return self.headers.get("Host", "") in (f"127.0.0.1:{HTTP_PORT}", f"localhost:{HTTP_PORT}")

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self._host_ok():
            return self._send(403, {"error": "forbidden"})
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            with open(os.path.join(APP_DIR, "panel.html"), "rb") as f:
                return self._send(200, f.read(), "text/html; charset=utf-8")
        if u.path in ("/icon.png", "/favicon.ico"):
            name = "icon.png" if u.path == "/icon.png" else "PocketNova.ico"
            try:
                with open(os.path.join(APP_DIR, name), "rb") as f:
                    return self._send(200, f.read(), "image/png" if name.endswith("png") else "image/x-icon")
            except OSError:
                return self._send(404, {"error": "no icon"})
        if u.path == "/api/state":
            q = parse_qs(u.query)
            since = int(q.get("since", ["0"])[0] or 0)
            rsince = int(q.get("rsince", ["0"])[0] or 0)
            link.ui_seen = time.time()
            with link.lock:
                st = dict(link.state)
                logs = [{"i": i, "text": t} for i, t in link.logs if i > since]
                replies = [{"i": i, **r} for i, r in link.replies if i > rsince]
            with settings_lock:
                labels = dict(settings.get("labels", {}))
                auto_open = settings.get("autoOpen", True)
            return self._send(200, {
                "connected": link.connected(),
                "transport": link.transport(),
                "bleReconnecting": link.ble_reconnecting(),
                "bleError": link.ble.last_error,
                "bleAvailable": HAVE_BLEAK,
                "port": link.port,
                "paused": link.paused,
                **st,
                "logs": logs,
                "replies": replies,
                "labels": labels,
                "autoOpen": auto_open,
                "historyDays": settings.get("historyDays", 30),
                "autostart": autostart_enabled(),
                "update": updater.info(),
                "usbPresent": link.usb_present(),
                "history": history.recent(),
                "netLabels": {m: e.get("label") or e.get("name", "") for m, e in settings.get("netDevices", {}).items()
                              if e.get("label") or e.get("name")},
                "firmwareDir": settings.get("firmwareDir"),
            })
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        # The custom header can't be sent cross-site without a CORS preflight we never
        # answer, so other web pages can't drive Pocket Nova through this server.
        if not self._host_ok() or self.headers.get("X-PocketNova") != "1":
            return self._send(403, {"error": "forbidden"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self._send(400, {"error": "bad json"})
        path = urlparse(self.path).path

        if path == "/api/cmd":
            if body.get("cmd") not in ALLOWED_CMDS:
                return self._send(400, {"error": "unknown command"})
            ok = link.send(body)
            return self._send(200 if ok else 503, {"sent": ok})
        if path == "/api/label":
            addr, label = str(body.get("addr", ""))[:17], str(body.get("label", ""))[:40]
            with settings_lock:
                labels = settings.setdefault("labels", {})
                if label.strip():
                    labels[addr] = label.strip()
                else:
                    labels.pop(addr, None)
                save_settings(settings)
            return self._send(200, {"ok": True})
        if path == "/api/netlabel":
            mac, label = str(body.get("mac", ""))[:17].upper(), str(body.get("label", ""))[:40].strip()
            with settings_lock:
                entry = settings.setdefault("netDevices", {}).get(mac)
                if entry is None:
                    return self._send(404, {"error": "unknown device"})
                entry["label"] = label
                if label:
                    entry["mine"] = True            # naming a device = it's yours
                save_settings(settings)
            with link.lock:                     # show it at once, without a new scan
                for d in (link.state.get("netscan") or {}).get("list", []):
                    if d["mac"].upper() == mac:
                        d["label"] = label
                        d["mine"] = d.get("mine") or bool(label)
            return self._send(200, {"ok": True})
        if path == "/api/netmine":
            # {"mac": "..", "mine": true} for one device, {"all": true} for everything in the last scan
            with link.lock:
                shown = (link.state.get("netscan") or {}).get("list", [])
                macs = [d["mac"].upper() for d in shown] if body.get("all") else [str(body.get("mac", ""))[:17].upper()]
                mine = bool(body.get("mine", True))
                for d in shown:
                    if d["mac"].upper() in macs:
                        d["mine"] = mine
                        d["new"] = False
            with settings_lock:
                book = settings.setdefault("netDevices", {})
                for m in macs:
                    if m in book:
                        book[m]["mine"] = mine
                save_settings(settings)
            return self._send(200, {"ok": True})
        if path == "/api/history/clear":
            days = body.get("olderThanDays")
            kinds = body.get("kinds")
            history.clear(older_than_days=float(days) if days else None,
                          kinds=set(map(str, kinds)) if isinstance(kinds, list) else None)
            return self._send(200, {"ok": True})
        if path == "/api/history/all":            # for exporting
            return self._send(200, {"items": history.all()})
        if path == "/api/netforget":              # start the home-network device list over
            with settings_lock:
                settings["netDevices"] = {}
                save_settings(settings)
            with link.lock:
                link.state.pop("netscan", None)
            return self._send(200, {"ok": True})
        if path == "/api/settings":
            with settings_lock:
                if "autoOpen" in body:
                    settings["autoOpen"] = bool(body["autoOpen"])
                if "historyDays" in body:
                    settings["historyDays"] = max(0, int(body["historyDays"]))
                save_settings(settings)
            if "autostart" in body:
                try:
                    set_autostart(bool(body["autostart"]))
                except Exception as e:
                    return self._send(500, {"error": str(e)})
            return self._send(200, {"ok": True})
        if path == "/api/firmware":
            return self._send(200, {"started": updater.start()})
        return self._send(404, {"error": "not found"})


# ---------------------------------------------------------------- main

def install_crash_logging():
    """pythonw has no console, so crashes used to vanish without a trace.
    Send every error (and hard crashes) to crash.log next to the app."""
    import faulthandler
    import traceback
    path = os.path.join(DATA_DIR, "crash.log")
    f = open(path, "a", encoding="utf-8", buffering=1)
    if sys.stderr is None:
        sys.stderr = f
    if sys.stdout is None:
        sys.stdout = f
    faulthandler.enable(f)

    def hook(exc_type, exc, tb):
        f.write(time.strftime("\n%Y-%m-%d %H:%M:%S ") + "".join(traceback.format_exception(exc_type, exc, tb)))
    sys.excepthook = hook
    threading.excepthook = lambda a: hook(a.exc_type, a.exc_value, a.exc_traceback)


def run_link_forever():
    # If anything unexpected goes wrong while talking to the device, log it
    # and keep going instead of silently giving up.
    while True:
        try:
            link.run()
        except Exception as e:
            log(f"Device link error (recovering): {e!r}")
            import traceback
            traceback.print_exc()
            try:
                link._drop("error")
            except Exception:
                pass
            time.sleep(2)


class SingleServer(ThreadingHTTPServer):
    """Python's HTTP server allows address reuse, and on Windows that lets a
    SECOND copy bind the same port - two copies then fight over COM3.
    Exclusive binding makes the second copy fail, so it just exits."""
    allow_reuse_address = False

    def server_bind(self):
        import socket
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def main():
    want_open = "--open" in sys.argv
    try:
        server = SingleServer(("127.0.0.1", HTTP_PORT), Handler)
    except OSError:
        # Already running in the background: just show the window if asked.
        if want_open:
            open_window()
        return

    install_crash_logging()
    log(f"Pocket Nova Panel started (pid {os.getpid()})")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=run_link_forever, daemon=True).start()
    if want_open:
        open_window()

    # Auto-open: once per plug-in, if the panel isn't already showing.
    opened_for = 0.0
    while True:
        try:
            time.sleep(1)
            if link.connected() and link.connected_at != opened_for:
                opened_for = link.connected_at
                with settings_lock:
                    auto = settings.get("autoOpen", True)
                if auto and not link.ui_active() and not updater.running:
                    open_window()
        except Exception as e:
            log(f"Main loop error (recovering): {e!r}")


if __name__ == "__main__":
    main()
