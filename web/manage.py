"""
The boards as the dashboard database knows them, from the VPS shell. The
boards take no commands: their firmware is updated on their own update page.

    ./venv/bin/python manage.py status [DEVICE]   boards, firmware, diagnostics, BLE devices around
    ./venv/bin/python manage.py forget-bms MAC    drop a BMS from the dashboard
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
              f"on {fw.get('partition')}")
        print(f"  uptime {diag.get('uptime')} s, reset: {diag.get('reset')}, heap {diag.get('heap')} "
              f"(min {diag.get('heap_min')}, block {diag.get('heap_block')}), Wi-Fi {diag.get('wifi_rssi')} dBm")
        for slot in diag.get("slots", []):
            print(f"  slot {slot['slot']}: {slot['link']}, error {slot['error']}, {slot.get('mac', '-')} "
                  f"rssi {slot.get('rssi', '-')}")
        print("  BLE devices around (last 5 min):")
        for dev in diag.get("ble", []):
            print(f"    {dev['mac']} {dev['rssi']:4} dBm {'JK ' if dev['jk'] else '   '}"
                  f"'{dev['name']}' type {dev['addr_type']}, {dev['count']} adv, {dev['age']} s ago")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    conn = connect()
    action, args = argv[1], argv[2:]
    if action == "status":
        cmd_status(conn, *args)
    elif action == "forget-bms" and len(args) == 1:
        conn.execute("DELETE FROM latest WHERE mac = ?", (args[0].upper(),))
        conn.commit()
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
