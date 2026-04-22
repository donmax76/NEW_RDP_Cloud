#!/usr/bin/env python3
"""_host_events_stats.py — print aggregated host-event stats directly from
/opt/remotedesk/host_events.log on the VPS. Standalone: doesn't require
server.py to be running.

Usage:
    ssh root@vps 'python3 /opt/remotedesk/_host_events_stats.py'

Override log path:
    ssh root@vps 'RDP_HOST_EVENTS_LOG=/tmp/events.log python3 _host_events_stats.py'
"""
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

LOG_PATH = Path(os.environ.get("RDP_HOST_EVENTS_LOG",
                               "/opt/remotedesk/host_events.log"))


def fmt_duration(seconds: int) -> str:
    seconds = max(0, int(seconds))
    d, rem = divmod(seconds, 86400)
    h, rem = divmod(rem, 3600)
    m, s   = divmod(rem, 60)
    if d: return f"{d}d {h}h {m}m"
    if h: return f"{h}h {m}m"
    if m: return f"{m}m {s}s"
    return f"{s}s"


def analyze(log_path: Path) -> dict:
    now = int(time.time())
    tokens: dict[str, dict] = {}
    if not log_path.is_file():
        return {"now": now, "totals": {"hosts": 0}, "tokens": {}}

    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try:
                ev = json.loads(line)
            except Exception:
                continue
            tok, kind = str(ev.get("token", "")), str(ev.get("event", ""))
            ts = int(ev.get("epoch", 0) or 0)
            if ts <= 0:
                try:
                    iso = ev.get("ts", "")
                    if iso:
                        ts = int(datetime.fromisoformat(iso.rstrip("Z")).timestamp())
                except Exception:
                    pass
            if not tok or not kind or ts <= 0: continue

            t = tokens.setdefault(tok, {
                "state": "offline", "locked": False,
                "online_since": 0, "sleep_since": 0, "lock_since": 0,
                "uptime_seconds": 0, "sleep_seconds": 0, "locked_seconds": 0,
                "startups": 0, "shutdowns": 0, "sleeps": 0, "wakes": 0,
                "locks": 0, "unlocks": 0,
                "first_seen": ts, "last_seen": ts, "last_event": kind,
                "host_version": str(ev.get("host_version", "")),
            })
            t["last_seen"], t["last_event"] = ts, kind
            if ev.get("host_version"): t["host_version"] = str(ev["host_version"])

            if kind == "startup":
                t["startups"] += 1
                if t["state"] == "online" and t["online_since"]:
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["state"], t["online_since"] = "online", ts
            elif kind == "shutdown":
                t["shutdowns"] += 1
                if t["state"] == "online" and t["online_since"]:
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["state"], t["online_since"] = "offline", 0
                if t["locked"] and t["lock_since"]:
                    t["locked_seconds"] += max(0, ts - t["lock_since"])
                t["locked"], t["lock_since"] = False, 0
            elif kind == "sleep":
                t["sleeps"] += 1
                if t["state"] == "online" and t["online_since"]:
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["state"], t["online_since"] = "sleeping", 0
                t["sleep_since"] = ts
            elif kind == "wake":
                t["wakes"] += 1
                if t["state"] == "sleeping" and t["sleep_since"]:
                    t["sleep_seconds"] += max(0, ts - t["sleep_since"])
                t["state"], t["sleep_since"] = "online", 0
                t["online_since"] = ts
            elif kind == "lock":
                t["locks"] += 1
                if not t["locked"]:
                    t["locked"], t["lock_since"] = True, ts
            elif kind == "unlock":
                t["unlocks"] += 1
                if t["locked"] and t["lock_since"]:
                    t["locked_seconds"] += max(0, ts - t["lock_since"])
                t["locked"], t["lock_since"] = False, 0

    for t in tokens.values():
        if t["state"] == "online" and t["online_since"]:
            t["uptime_seconds"] += max(0, now - t["online_since"])
        elif t["state"] == "sleeping" and t["sleep_since"]:
            t["sleep_seconds"] += max(0, now - t["sleep_since"])
        if t["locked"] and t["lock_since"]:
            t["locked_seconds"] += max(0, now - t["lock_since"])

    # Inferred online state from activity recency (same logic as server.py).
    ACTIVE_WINDOW = 5 * 60
    for t in tokens.values():
        if t["state"] == "offline" and (now - t["last_seen"]) < ACTIVE_WINDOW \
           and t["last_event"] not in ("shutdown", "sleep"):
            t["state"] = "online"
            t["state_inferred"] = True

    return {
        "now": now,
        "totals": {
            "hosts":    len(tokens),
            "online":   sum(1 for t in tokens.values() if t["state"] == "online"),
            "sleeping": sum(1 for t in tokens.values() if t["state"] == "sleeping"),
            "offline":  sum(1 for t in tokens.values() if t["state"] == "offline"),
        },
        "tokens": tokens,
    }


def main():
    r = analyze(LOG_PATH)
    tot = r["totals"]
    print(f"=== Host Events Stats ({datetime.now().isoformat(timespec='seconds')}) ===")
    print(f"Log:    {LOG_PATH}  (size: "
          f"{LOG_PATH.stat().st_size if LOG_PATH.is_file() else 0} bytes)")
    print(f"Hosts:  {tot['hosts']}  (online={tot['online']} "
          f"sleeping={tot['sleeping']} offline={tot['offline']})")
    print()
    if not r["tokens"]:
        print("(no events logged yet)")
        return
    # Header
    hdr = ("token", "state", "uptime", "sleep", "locked",
           "starts", "sleeps", "locks", "last event", "last seen")
    w = (20, 9, 10, 10, 10, 6, 6, 5, 10, 19)
    line = "  ".join(f"{h:<{wi}}" for h, wi in zip(hdr, w))
    print(line)
    print("-" * len(line))
    for tok in sorted(r["tokens"].keys()):
        t = r["tokens"][tok]
        short = (tok[:8] + "..." + tok[-4:]) if len(tok) > 12 else tok
        row = (
            short,
            t["state"] + ("+L" if t["locked"] else ""),
            fmt_duration(t["uptime_seconds"]),
            fmt_duration(t["sleep_seconds"]),
            fmt_duration(t["locked_seconds"]),
            str(t["startups"]),
            str(t["sleeps"]),
            str(t["locks"]),
            t["last_event"],
            datetime.fromtimestamp(t["last_seen"]).strftime("%Y-%m-%d %H:%M:%S"),
        )
        print("  ".join(f"{c:<{wi}}" for c, wi in zip(row, w)))

if __name__ == "__main__":
    main()
