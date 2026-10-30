"""
JK BMS status endpoint and dashboard.

The T-Display board POSTs its status to /api/status every minute (bearer token
auth). The dashboard at / (login required) loads /api/latest once; reload the
page for fresh data. The board's firmware and diagnostics come with each
status report (see manage.py status); the firmware is updated only on the
board's own update page, the dashboard sends the board no commands.

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
import sqlite3
import time
from pathlib import Path

from flask import Flask, abort, g, jsonify, redirect, render_template, request, session, url_for

HISTORY_DAYS = 90
OFFLINE_AFTER_S = 180   # three missed uploads

app = Flask(__name__)
app.secret_key = os.environ["SECRET_KEY"]
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"
app.permanent_session_lifetime = 30 * 86400

HERE = Path(__file__).parent
DB_PATH = os.environ.get("JKBMS_DB", str(HERE / "jkbms.sqlite3"))

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


def check_token():
    token = request.headers.get("Authorization", "").removeprefix("Bearer ")
    if not hmac.compare_digest(token, os.environ["JKBMS_API_TOKEN"]):
        abort(401)


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
    conn.commit()
    return {"ok": True}


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
