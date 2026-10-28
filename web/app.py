"""
JK BMS status endpoint and dashboard.

The T-Display board POSTs its status to /api/status every minute (bearer token
auth). The dashboard at / (login required) loads /api/latest once; reload the
page for fresh data.

Remote maintenance (see manage.py): the reply to a status upload tells the
board whether to POST its log to /api/logs, which log levels to use, a pending
command, and the firmware to run, downloaded from /api/firmware
(firmware/jkbms.bin). Logs go to logs/<device>.log.

Environment:
    JKBMS_API_TOKEN     token the board sends in the Authorization header
    JKBMS_WEB_USER      dashboard login
    JKBMS_WEB_PASSWORD  dashboard password
    SECRET_KEY          Flask session key
    JKBMS_DB            SQLite path (default: jkbms.sqlite3 next to this file)
"""

import functools
import hmac
import json
import os
import re
import sqlite3
import time
from pathlib import Path

from flask import Flask, abort, g, jsonify, redirect, render_template, request, send_file, session, url_for

HISTORY_DAYS = 90
OFFLINE_AFTER_S = 180   # three missed uploads

app = Flask(__name__)
app.secret_key = os.environ["SECRET_KEY"]
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"
app.permanent_session_lifetime = 30 * 86400

HERE = Path(__file__).parent
DB_PATH = os.environ.get("JKBMS_DB", str(HERE / "jkbms.sqlite3"))
FIRMWARE_PATH = HERE / "firmware" / "jkbms.bin"
LOGS_DIR = HERE / "logs"
LOG_MAX_BYTES = 20 * 1024 * 1024
ANSI_RE = re.compile(rb"\x1b\[[0-9;]*m")
DEFAULT_CONFIG = {"logs": "0", "ota": "0", "log_level": "{}"}

SCHEMA = """
CREATE TABLE IF NOT EXISTS latest (
    mac TEXT PRIMARY KEY,
    device TEXT,
    received REAL,
    payload TEXT
);
CREATE TABLE IF NOT EXISTS history (
    id INTEGER PRIMARY KEY,
    mac TEXT,
    ts REAL,
    voltage REAL,
    current REAL,
    soc INTEGER,
    min_cell REAL,
    max_cell REAL,
    charging INTEGER,
    discharging INTEGER,
    errors INTEGER,
    cells TEXT
);
CREATE INDEX IF NOT EXISTS history_mac_ts ON history (mac, ts);
CREATE TABLE IF NOT EXISTS events (
    mac TEXT,
    number INTEGER,
    ts REAL,
    code INTEGER,
    label TEXT,
    PRIMARY KEY (mac, number)
);
CREATE TABLE IF NOT EXISTS devices (
    device TEXT PRIMARY KEY,
    received REAL,
    wake INTEGER
);
CREATE TABLE IF NOT EXISTS commands (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device TEXT,
    payload TEXT,
    created REAL
);
CREATE TABLE IF NOT EXISTS config (
    device TEXT,
    key TEXT,
    value TEXT,
    PRIMARY KEY (device, key)
);
"""


def db():
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH)
        g.db.row_factory = sqlite3.Row
    return g.db


@app.teardown_appcontext
def close_db(exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


with app.app_context():
    db().executescript(SCHEMA)
    # added with remote maintenance: the board's firmware and diagnostics
    columns = [r["name"] for r in db().execute("PRAGMA table_info(devices)")]
    if "board" not in columns:
        db().execute("ALTER TABLE devices ADD COLUMN board TEXT")
        db().commit()


def get_config(conn, device):
    """Settings of a board: its own rows over the "*" rows over DEFAULT_CONFIG."""
    config = dict(DEFAULT_CONFIG)
    for row in conn.execute("SELECT key, value FROM config WHERE device = '*'"):
        config[row["key"]] = row["value"]
    for row in conn.execute("SELECT key, value FROM config WHERE device = ?", (device,)):
        config[row["key"]] = row["value"]
    return config


def firmware_elf():
    """SHA-256 of the ELF of firmware/jkbms.bin, from its app description, or None."""
    try:
        with open(FIRMWARE_PATH, "rb") as f:
            # image header (24) + segment header (8) + esp_app_desc_t up to app_elf_sha256 (144)
            head = f.read(24 + 8 + 144 + 32)
    except FileNotFoundError:
        return None
    if len(head) < 208 or head[0] != 0xE9 or int.from_bytes(head[32:36], "little") != 0xABCD5432:
        return None
    return head[176:208].hex()


def check_token():
    token = request.headers.get("Authorization", "").removeprefix("Bearer ")
    if not hmac.compare_digest(token, os.environ["JKBMS_API_TOKEN"]):
        abort(401)


def board_reply(conn, device, data):
    """What the board should do next; boards without "fw" ignore the reply."""
    reply = {"ok": True}
    config = get_config(conn, device)
    reply["logs"] = config["logs"] == "1"
    try:
        reply["log_level"] = json.loads(config["log_level"])
    except ValueError:
        pass
    done = (data.get("diag") or {}).get("cmd_id", 0)
    row = conn.execute("SELECT id, payload FROM commands WHERE device = ? AND id > ? ORDER BY id LIMIT 1",
                       (device, done)).fetchone()
    if row:
        reply["cmd"] = {**json.loads(row["payload"]), "id": row["id"]}
    elf = firmware_elf()
    fw = data.get("fw") or {}
    if config["ota"] == "1" and elf and fw.get("elf") and fw["elf"] != elf:
        reply["ota"] = {"elf": elf, "url": "firmware"}
    return reply


def login_required(view):
    @functools.wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("user"):
            if request.path.startswith("/api/"):
                abort(401)
            return redirect(url_for("login"))
        return view(*args, **kwargs)
    return wrapped


@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        user_ok = hmac.compare_digest(request.form.get("user", ""), os.environ["JKBMS_WEB_USER"])
        password_ok = hmac.compare_digest(request.form.get("password", ""), os.environ["JKBMS_WEB_PASSWORD"])
        if user_ok and password_ok:
            session.permanent = True
            session["user"] = request.form["user"]
            return redirect(url_for("dashboard"))
        error = "Wrong user or password"
    return render_template("login.html", error=error)


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
@login_required
def dashboard():
    return render_template("dashboard.html")


@app.post("/api/status")
def post_status():
    check_token()
    data = request.get_json(force=True)
    now = time.time()
    conn = db()
    device = data.get("device", "")
    part = data.get("part")
    if part:
        # the board sends the settings and the logbook in separate small reports
        conn.execute("UPDATE devices SET received = ? WHERE device = ?", (now, device))
        for bms in data.get("bms", []):
            store_part(conn, bms, now)
        conn.commit()
        return {"ok": True}
    board = {key: data[key] for key in ("fw", "diag") if key in data}
    conn.execute("INSERT OR REPLACE INTO devices (device, received, wake, board) VALUES (?, ?, ?, ?)",
                 (device, now, data.get("wake"), json.dumps(board)))
    # a board out of Wi-Fi reach sends what it read earlier, with its age in seconds
    measured = now - float(data.get("age") or 0)
    for bms in data.get("bms", []):
        store_bms(conn, device, bms, now, measured)
    conn.execute("DELETE FROM history WHERE ts < ?", (now - HISTORY_DAYS * 86400,))
    reply = board_reply(conn, device, data)
    conn.commit()
    return reply


@app.post("/api/logs")
def post_logs():
    """Log output of a board, appended to logs/<device>.log."""
    check_token()
    device = request.args.get("device", "")
    if not re.fullmatch(r"[0-9A-F]{12}", device):
        abort(400)
    LOGS_DIR.mkdir(exist_ok=True)
    path = LOGS_DIR / f"{device}.log"
    if path.exists() and path.stat().st_size > LOG_MAX_BYTES:
        path.replace(path.with_suffix(".log.1"))
    text = ANSI_RE.sub(b"", request.get_data())
    lost = request.args.get("lost", "0")
    header = f"===== received {time.strftime('%Y-%m-%d %H:%M:%S')}"
    if lost != "0":
        header += f", {lost} bytes lost"
    with open(path, "ab") as f:
        f.write(f"{header} =====\n".encode())
        f.write(text)
        if not text.endswith(b"\n"):
            f.write(b"\n")
    return {"ok": True}


@app.get("/api/firmware")
def get_firmware():
    check_token()
    if not FIRMWARE_PATH.exists():
        abort(404)
    return send_file(FIRMWARE_PATH, mimetype="application/octet-stream")


def store_part(conn, bms, now):
    """Settings or logbook events of a BMS, merged into what is known of it."""
    mac = bms["mac"]
    row = conn.execute("SELECT payload FROM latest WHERE mac = ?", (mac,)).fetchone()
    if "settings" in bms and row:
        payload = json.loads(row["payload"])
        payload["settings"] = bms["settings"]
        conn.execute("UPDATE latest SET payload = ? WHERE mac = ?", (json.dumps(payload), mac))
    if bms.get("logbook"):
        store_events(conn, mac, bms["logbook"], now)


def store_events(conn, mac, logbook, now):
    # event timestamps count seconds of BMS uptime; convert them to wall clock
    uptime = logbook["uptime"]
    total = logbook["total"]
    events = logbook["events"]
    for i, (ts, code, label) in enumerate(events):
        number = total - (len(events) - 1 - i)
        conn.execute("INSERT OR IGNORE INTO events (mac, number, ts, code, label) VALUES (?, ?, ?, ?, ?)",
                     (mac, number, now - (uptime - ts), code, label))


def store_bms(conn, device, bms, now, measured):
    """`now` is when the report arrived, `measured` when the board read the BMS."""
    mac = bms["mac"]
    row = conn.execute("SELECT payload FROM latest WHERE mac = ?", (mac,)).fetchone()
    previous = json.loads(row["payload"]) if row else {}
    new_logbook = bms.get("logbook")
    # the board sends settings and the logbook only now and then: keep the last ones
    for key in ("settings", "logbook", "info"):
        if key not in bms and key in previous:
            bms[key] = previous[key]
    if not bms.get("online") and "cells" not in bms and "cells" in previous:
        # BMS not reachable this time: keep the last known values, flagged offline
        bms = {**previous, "online": False, "error": bms.get("error")}
    bms["updated"] = measured if bms.get("online") else previous.get("updated")
    conn.execute("INSERT OR REPLACE INTO latest (mac, device, received, payload) VALUES (?, ?, ?, ?)",
                 (mac, device, now, json.dumps(bms)))

    if bms.get("online") and "cells" in bms:
        conn.execute(
            "INSERT INTO history (mac, ts, voltage, current, soc, min_cell, max_cell, charging, discharging, errors,"
            " cells) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (mac, measured, bms["voltage"], bms["current"], bms["soc"], min(bms["cells"]), max(bms["cells"]),
             bms["charging"], bms["discharging"], bms["errors"], json.dumps(bms["cells"])))

    if new_logbook:
        store_events(conn, mac, new_logbook, now)


@app.get("/api/latest")
@login_required
def get_latest():
    now = time.time()
    conn = db()
    result = []
    for row in conn.execute("SELECT * FROM latest ORDER BY mac"):
        bms = json.loads(row["payload"])
        bms["received"] = row["received"]
        bms["stale"] = now - row["received"] > OFFLINE_AFTER_S
        bms["events"] = [dict(e) for e in conn.execute(
            "SELECT number, ts, code, label FROM events WHERE mac = ? ORDER BY number DESC LIMIT 500",
            (row["mac"],))]
        result.append(bms)
    devices = []
    for row in conn.execute("SELECT * FROM devices ORDER BY received DESC"):
        device = dict(row)
        device.update(json.loads(device.pop("board") or "{}"))
        devices.append(device)
    return jsonify({"now": now, "bms": result, "devices": devices})


@app.get("/api/history")
@login_required
def get_history():
    mac = request.args["mac"]
    hours = float(request.args.get("hours", 24))
    rows = db().execute(
        "SELECT ts, voltage, current, soc, min_cell, max_cell, charging, discharging, errors FROM history"
        " WHERE mac = ? AND ts > ? ORDER BY ts", (mac, time.time() - hours * 3600))
    return jsonify([dict(r) for r in rows])
