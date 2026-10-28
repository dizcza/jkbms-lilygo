"""
Remote maintenance of the boards from the VPS shell, through the dashboard
database. The board picks up the changes with its next status upload (every
1-2 minutes).

    ./venv/bin/python manage.py status [DEVICE]          boards, firmware, diagnostics, BLE devices around
    ./venv/bin/python manage.py assign DEVICE SLOT MAC   watch this BMS in slot 1 or 2 (MAC "" frees the slot)
    ./venv/bin/python manage.py reboot DEVICE
    ./venv/bin/python manage.py commands [DEVICE]        queued commands and whether the board ran them
    ./venv/bin/python manage.py set DEVICE|* KEY VALUE   logs 1|0, ota 1|0, log_level '{"jkbms": 4}'
    ./venv/bin/python manage.py forget-bms MAC           drop a BMS from the dashboard
    ./venv/bin/python manage.py firmware                 ELF SHA-256 of firmware/jkbms.bin

Log levels: 0 none, 1 error, 2 warn, 3 info, 4 debug, 5 verbose; "*" sets all tags.
The log of a board is in logs/<DEVICE>.log. A new firmware goes to firmware/jkbms.bin
(tools/deploy_firmware.sh); boards with "ota" on download it when their ELF differs.
"""

import json
import sqlite3
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent
DB_PATH = HERE / "jkbms.sqlite3"


def connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def ago(ts):
    return f"{time.time() - ts:.0f} s ago" if ts else "never"


def firmware_elf():
    # as app.firmware_elf(), which cannot be imported without the dashboard secrets
    path = HERE / "firmware" / "jkbms.bin"
    if not path.exists():
        return None
    head = path.read_bytes()[:208]
    return head[176:208].hex() if len(head) == 208 and head[0] == 0xE9 else None


def cmd_status(conn, device=None):
    query = "SELECT * FROM devices" + (" WHERE device = ?" if device else "") + " ORDER BY received DESC"
    for row in conn.execute(query, (device,) if device else ()):
        board = json.loads(row["board"] or "{}")
        fw = board.get("fw", {})
        diag = board.get("diag", {})
        print(f"board {row['device']}: last report {ago(row['received'])}, upload #{row['wake']}")
        if not fw:
            print("  old firmware: no diagnostics")
            continue
        print(f"  firmware {fw.get('version')} built {fw.get('built')} elf {fw.get('elf', '')[:16]} "
              f"on {fw.get('partition')}" + (f", OTA error: {fw['ota_error']}" if fw.get("ota_error") else ""))
        print(f"  uptime {diag.get('uptime')} s, reset: {diag.get('reset')}, heap {diag.get('heap')} "
              f"(min {diag.get('heap_min')}, block {diag.get('heap_block')}), Wi-Fi {diag.get('wifi_rssi')} dBm, "
              f"last command {diag.get('cmd_id')}, logs {diag.get('logs')}")
        for slot in diag.get("slots", []):
            print(f"  slot {slot['slot']}: {slot['link']}, error {slot['error']}, {slot.get('mac', '-')} "
                  f"rssi {slot.get('rssi', '-')}")
        print("  BLE devices around (last 5 min):")
        for dev in diag.get("ble", []):
            print(f"    {dev['mac']} {dev['rssi']:4} dBm {'JK ' if dev['jk'] else '   '}"
                  f"'{dev['name']}' type {dev['addr_type']}, {dev['count']} adv, {dev['age']} s ago")


def queue(conn, device, payload):
    cur = conn.execute("INSERT INTO commands (device, payload, created) VALUES (?, ?, ?)",
                       (device, json.dumps(payload), time.time()))
    conn.commit()
    print(f"queued command {cur.lastrowid}: {payload}")


def cmd_commands(conn, device=None):
    done = {}
    for row in conn.execute("SELECT device, board FROM devices"):
        done[row["device"]] = json.loads(row["board"] or "{}").get("diag", {}).get("cmd_id", 0)
    query = "SELECT * FROM commands" + (" WHERE device = ?" if device else "") + " ORDER BY id"
    for row in conn.execute(query, (device,) if device else ()):
        state = "done" if row["id"] <= done.get(row["device"], 0) else "pending"
        print(f"{row['id']:4} {row['device']} {state:8} {ago(row['created'])}: {row['payload']}")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    conn = connect()
    action, args = argv[1], argv[2:]
    if action == "status":
        cmd_status(conn, *args)
    elif action == "assign" and len(args) == 3:
        queue(conn, args[0], {"name": "assign", "slot": int(args[1]), "mac": args[2].upper()})
    elif action == "reboot" and len(args) == 1:
        queue(conn, args[0], {"name": "reboot"})
    elif action == "commands":
        cmd_commands(conn, *args)
    elif action == "set" and len(args) == 3:
        if args[1] == "log_level":
            json.loads(args[2])
        conn.execute("INSERT OR REPLACE INTO config (device, key, value) VALUES (?, ?, ?)", tuple(args))
        conn.commit()
    elif action == "forget-bms" and len(args) == 1:
        conn.execute("DELETE FROM latest WHERE mac = ?", (args[0].upper(),))
        conn.commit()
    elif action == "firmware":
        print(firmware_elf())
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
