#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════╗
║       Claude Token Meter — Companion Script v1.0.0          ║
╠══════════════════════════════════════════════════════════════╣
║  Reads Claude Code token usage and forwards it to your      ║
║  ESP32-based Claude Token Meter display every 30 seconds.   ║
╠══════════════════════════════════════════════════════════════╣
║  Data sources (tried in order):                             ║
║    1. Claude Code local database  (~/.claude/)              ║
║    2. Anthropic Admin Usage API   (if org key set)          ║
║    3. Manual increment mode       (stdin)                   ║
╠══════════════════════════════════════════════════════════════╣
║  Usage:                                                     ║
║    python claude_monitor.py --ip 192.168.1.42               ║
║    python claude_monitor.py --ip claude-meter.local         ║
║    python claude_monitor.py --discover                      ║
╚══════════════════════════════════════════════════════════════╝
"""

import os
import sys
import json
import time
import sqlite3
import argparse
import requests
import platform
import datetime
import socket
from pathlib import Path
from typing import Optional, Dict, Any

# ─── Constants ──────────────────────────────────────────────
VERSION        = "2.0"
POLL_INTERVAL  = 30        # seconds between ESP32 updates
DISPLAY_PERIOD = 5         # seconds between console refreshes
WEEKLY_DEFAULT = 1_000_000 # fallback limit if not set on device

# Claude Sonnet 4 pricing (per 1M tokens, USD)
# Update these if Anthropic changes pricing
PRICE_INPUT    = 3.00  / 1_000_000
PRICE_OUTPUT   = 15.00 / 1_000_000
PRICE_CACHE_R  = 0.30  / 1_000_000
PRICE_CACHE_W  = 3.75  / 1_000_000


# ════════════════════════════════════════════════════════════
#   DISCOVERY — find device on LAN
# ════════════════════════════════════════════════════════════

def discover_device(timeout: float = 5.0) -> Optional[str]:
    """Try common addresses to locate the Claude Token Meter."""
    candidates = [
        "claude-meter.local",
        "192.168.4.1",
    ]
    # Also scan common subnets
    local_ip = socket.gethostbyname(socket.gethostname())
    prefix   = ".".join(local_ip.split(".")[:3])
    for i in (1, 100, 101, 110, 200):
        candidates.append(f"{prefix}.{i}")

    print("🔍 Searching for Claude Token Meter on network...")
    for addr in candidates:
        url = f"http://{addr}/api/status"
        try:
            r = requests.get(url, timeout=timeout)
            if r.status_code == 200 and "weeklyTotal" in r.text:
                print(f"✅ Found device at {addr}")
                return addr
        except Exception:
            pass
    return None


# ════════════════════════════════════════════════════════════
#   CLAUDE CODE  — local database reader
# ════════════════════════════════════════════════════════════

def get_claude_dirs() -> list:
    """Return all possible Claude Code data directories for this OS."""
    dirs = []
    home = Path.home()
    system = platform.system()
    if system == "Windows":
        appdata = Path(os.environ.get("APPDATA", str(home)))
        dirs += [appdata / "Claude", appdata / "claude"]
        localappdata = Path(os.environ.get("LOCALAPPDATA", str(home)))
        dirs += [localappdata / "Claude", localappdata / "claude"]
        dirs += [home / ".claude"]
    elif system == "Darwin":
        dirs += [
            home / ".claude",
            home / "Library" / "Application Support" / "Claude",
            home / "Library" / "Caches" / "Claude",
        ]
    else:
        dirs += [
            home / ".claude",
            home / ".config" / "claude",
            home / ".local" / "share" / "claude",
            Path("/root/.claude"),
        ]
    return [d for d in dirs if d.exists()]


# Back-compat alias
def get_claude_code_path() -> Path:
    dirs = get_claude_dirs()
    return dirs[0] if dirs else Path.home() / ".claude"


def read_claude_code_stats() -> Optional[Dict[str, Any]]:
    """
    Read real session token usage from Claude Code local storage.
    Scans SQLite databases AND .jsonl/.json files in every known directory.
    Works for Claude Pro subscribers — reads what Claude Code actually wrote
    to disk, regardless of subscription tier.
    """
    now      = datetime.datetime.now(datetime.timezone.utc)
    week_ago = now - datetime.timedelta(days=7)
    day_ago  = now - datetime.timedelta(days=1)

    totals: Dict[str, Any] = dict(
        weeklyTotal=0, dailyTotal=0, sessionTotal=0,
        inputTokens=0, outputTokens=0,
        cacheRead=0, cacheWrite=0,
        costUSD=0.0, model="claude-sonnet-4",
    )

    claude_dirs = get_claude_dirs()
    if not claude_dirs:
        print("  [companion] No Claude Code data directory found.")
        print("  [companion] Expected: ~/.claude/  or  %APPDATA%/Claude/")
        return None

    found_anything = False
    for base in claude_dirs:
        print(f"  [companion] Scanning: {base}")

        # ── 1. SQLite databases ────────────────────────────────────
        for db_path in base.rglob("*.db"):
            try:
                conn = sqlite3.connect(str(db_path), timeout=3)
                conn.row_factory = sqlite3.Row
                cur = conn.cursor()
                cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
                tables = [r[0] for r in cur.fetchall()]
                for table in tables:
                    try:
                        cur.execute(f"PRAGMA table_info([{table}])")
                        cols = [r[1].lower() for r in cur.fetchall()]
                        if not any("token" in c or "usage" in c for c in cols):
                            continue
                        time_col = next((c for c in cols
                                         if any(x in c for x in
                                                ["timestamp", "created", "time", "date"])), None)
                        if time_col:
                            cur.execute(
                                f"SELECT * FROM [{table}] WHERE [{time_col}] >= ?",
                                (week_ago.isoformat(),))
                        else:
                            cur.execute(f"SELECT * FROM [{table}]")
                        for row in cur.fetchall():
                            d = dict(row)
                            inp = int(d.get("input_tokens")  or d.get("inputtokens")  or 0)
                            out = int(d.get("output_tokens") or d.get("outputtokens") or 0)
                            cr  = int(d.get("cache_read_input_tokens")
                                      or d.get("cachereadtokens") or 0)
                            cw  = int(d.get("cache_creation_input_tokens")
                                      or d.get("cachewritetokens") or 0)
                            total = inp + out + cr + cw
                            if total == 0:
                                continue
                            found_anything = True
                            totals["weeklyTotal"]  += total
                            totals["inputTokens"]  += inp
                            totals["outputTokens"] += out
                            totals["cacheRead"]    += cr
                            totals["cacheWrite"]   += cw
                            if time_col:
                                try:
                                    ts = datetime.datetime.fromisoformat(
                                        str(d.get(time_col, "")).replace("Z", "+00:00"))
                                    if ts >= day_ago:
                                        totals["dailyTotal"] += total
                                except Exception:
                                    pass
                            mdl = d.get("model") or d.get("model_id") or ""
                            if mdl and "claude" in str(mdl).lower():
                                totals["model"] = str(mdl)
                    except sqlite3.OperationalError:
                        continue
                conn.close()
            except Exception:
                pass

        # ── 2. JSON / JSONL files ──────────────────────────────────
        seen = set()
        patterns = ["**/*.jsonl", "**/usage*.json", "**/stats*.json",
                    "**/session*.json", "**/*.json"]
        for pattern in patterns:
            for jf in base.glob(pattern):
                if jf in seen:
                    continue
                seen.add(jf)
                try:
                    if jf.stat().st_size > 50 * 1024 * 1024:
                        continue
                    with open(jf, "r", encoding="utf-8", errors="ignore") as f:
                        content = f.read().strip()
                except Exception:
                    continue
                if not content:
                    continue

                is_jsonl = jf.suffix == ".jsonl" or (
                    content.startswith("{") and "\n{" in content[:500])

                if is_jsonl:
                    for line in content.splitlines():
                        line = line.strip()
                        if not line or not line.startswith("{"):
                            continue
                        try:
                            rec = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        usage = (rec.get("usage")
                                 or (rec.get("message", {}) or {}).get("usage")
                                 or (rec.get("response", {}) or {}).get("usage") or {})
                        if not usage:
                            continue
                        inp = int(usage.get("input_tokens", 0) or 0)
                        out = int(usage.get("output_tokens", 0) or 0)
                        cr  = int(usage.get("cache_read_input_tokens", 0) or 0)
                        cw  = int(usage.get("cache_creation_input_tokens", 0) or 0)
                        total = inp + out + cr + cw
                        if total == 0:
                            continue

                        # Time filter — only count last 7 days
                        ts_str = (rec.get("timestamp")
                                  or rec.get("created_at")
                                  or (rec.get("message", {}) or {}).get("created_at"))
                        in_week, in_day = True, False
                        if ts_str:
                            try:
                                ts = datetime.datetime.fromisoformat(
                                    str(ts_str).replace("Z", "+00:00"))
                                if ts.tzinfo is None:
                                    ts = ts.replace(tzinfo=datetime.timezone.utc)
                                in_week = ts >= week_ago
                                in_day  = ts >= day_ago
                            except Exception:
                                pass
                        if not in_week:
                            continue

                        found_anything = True
                        totals["weeklyTotal"]  += total
                        totals["inputTokens"]  += inp
                        totals["outputTokens"] += out
                        totals["cacheRead"]    += cr
                        totals["cacheWrite"]   += cw
                        if in_day:
                            totals["dailyTotal"] += total
                        mdl = (rec.get("model")
                               or (rec.get("message", {}) or {}).get("model") or "")
                        if mdl and "claude" in str(mdl).lower():
                            totals["model"] = str(mdl)
                else:
                    try:
                        data = json.loads(content)
                    except json.JSONDecodeError:
                        continue
                    items = data if isinstance(data, list) else [data]
                    for rec in items:
                        if not isinstance(rec, dict):
                            continue
                        usage = rec.get("usage") or rec
                        inp = int(usage.get("input_tokens",  usage.get("inputTokens", 0)) or 0)
                        out = int(usage.get("output_tokens", usage.get("outputTokens", 0)) or 0)
                        cr  = int(usage.get("cache_read_input_tokens", 0) or 0)
                        cw  = int(usage.get("cache_creation_input_tokens", 0) or 0)
                        total = inp + out + cr + cw
                        if total == 0:
                            continue
                        found_anything = True
                        totals["weeklyTotal"]  += total
                        totals["inputTokens"]  += inp
                        totals["outputTokens"] += out
                        totals["cacheRead"]    += cr
                        totals["cacheWrite"]   += cw
                        mdl = rec.get("model") or ""
                        if mdl and "claude" in str(mdl).lower():
                            totals["model"] = str(mdl)

    if not found_anything:
        print("  [companion] No token usage data found in Claude Code files.")
        print("  [companion] Make sure Claude Code has been used at least once.")
        print("  [companion] Dirs scanned:", [str(d) for d in claude_dirs])
        return None

    totals["costUSD"] = round(
        totals["inputTokens"]  * PRICE_INPUT  +
        totals["outputTokens"] * PRICE_OUTPUT +
        totals["cacheRead"]    * PRICE_CACHE_R +
        totals["cacheWrite"]   * PRICE_CACHE_W,
        6,
    )
    print(f"  [companion] Found: weekly={totals['weeklyTotal']:,}  "
          f"model={totals['model']}  cost=${totals['costUSD']:.4f}")
    return totals


# ════════════════════════════════════════════════════════════
#   ANTHROPIC ADMIN API  — organization usage endpoint
# ════════════════════════════════════════════════════════════

def read_anthropic_api_stats(admin_key: str) -> Optional[Dict[str, Any]]:
    """
    Fetch usage from Anthropic's organization usage API.
    Requires an organization admin API key (not a regular API key).
    """
    now      = datetime.datetime.now(datetime.timezone.utc)
    week_ago = now - datetime.timedelta(days=7)

    headers = {
        "x-api-key":         admin_key,
        "anthropic-version": "2023-06-01",
        "anthropic-beta":    "usage-reporting-2024-10-31",
    }

    params = {
        "start_time": week_ago.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "end_time":   now.strftime("%Y-%m-%dT%H:%M:%SZ"),
    }

    try:
        r = requests.get(
            "https://api.anthropic.com/v1/usage",
            headers=headers,
            params=params,
            timeout=15
        )
        if r.status_code != 200:
            return None

        data = r.json()
        agg = {
            "weeklyTotal": 0, "dailyTotal": 0, "sessionTotal": 0,
            "inputTokens": 0, "outputTokens": 0,
            "cacheRead": 0, "cacheWrite": 0,
            "model": "various", "costUSD": 0.0,
        }

        day_ago = now - datetime.timedelta(days=1)
        records = data.get("data", data.get("usage", []))
        for rec in records:
            inp = rec.get("input_tokens", 0) or 0
            out = rec.get("output_tokens", 0) or 0
            cr  = rec.get("cache_read_input_tokens", 0) or 0
            cw  = rec.get("cache_creation_input_tokens", 0) or 0
            total = inp + out + cr + cw

            agg["weeklyTotal"]  += total
            agg["inputTokens"]  += inp
            agg["outputTokens"] += out
            agg["cacheRead"]    += cr
            agg["cacheWrite"]   += cw

            # Daily approximation from recent records
            ts_str = rec.get("timestamp", rec.get("created_at", ""))
            if ts_str:
                try:
                    ts = datetime.datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
                    if ts >= day_ago:
                        agg["dailyTotal"] += total
                except Exception:
                    pass

        agg["costUSD"] = (
            agg["inputTokens"]  * PRICE_INPUT  +
            agg["outputTokens"] * PRICE_OUTPUT +
            agg["cacheRead"]    * PRICE_CACHE_R +
            agg["cacheWrite"]   * PRICE_CACHE_W
        )
        return agg

    except Exception as e:
        print(f"⚠  API error: {e}")
        return None


# ════════════════════════════════════════════════════════════
#   ESP32 COMMUNICATION
# ════════════════════════════════════════════════════════════

def send_to_device(ip: str, stats: Dict[str, Any]) -> bool:
    """POST token stats to the ESP32."""
    url = f"http://{ip}/api/update"
    try:
        r = requests.post(
            url,
            json=stats,
            timeout=8,
            headers={"Content-Type": "application/json"}
        )
        return r.status_code == 200
    except Exception as e:
        print(f"⚠  Send failed: {e}")
        return False


def set_limit_on_device(ip: str, limit: int) -> bool:
    """Push the weekly limit setting to the device."""
    try:
        r = requests.post(
            f"http://{ip}/api/config",
            json={"weeklyLimit": limit},
            timeout=8
        )
        return r.status_code == 200
    except Exception:
        return False


def get_device_status(ip: str) -> Optional[Dict]:
    """Read current stats from device."""
    try:
        r = requests.get(f"http://{ip}/api/status", timeout=5)
        return r.json() if r.status_code == 200 else None
    except Exception:
        return None


# ════════════════════════════════════════════════════════════
#   CONSOLE UI
# ════════════════════════════════════════════════════════════

def fmt_num(n: int) -> str:
    if n >= 1_000_000: return f"{n/1_000_000:.2f}M"
    if n >= 1_000:     return f"{n/1_000:.1f}K"
    return str(n)

def progress_bar(pct: float, width: int = 30) -> str:
    filled = int(width * min(pct, 1.0))
    bar    = "█" * filled + "░" * (width - filled)
    return f"[{bar}] {pct*100:.1f}%"

def print_stats(stats: Dict, device_ip: str, source: str) -> None:
    pct = stats["weeklyTotal"] / max(stats.get("weeklyLimit", WEEKLY_DEFAULT), 1)
    bar = progress_bar(pct)
    color = "\033[92m" if pct < 0.8 else ("\033[93m" if pct < 0.95 else "\033[91m")
    rst   = "\033[0m"
    now   = datetime.datetime.now().strftime("%H:%M:%S")

    print(f"\n┌─────────────────────────────────────────────────┐")
    print(f"│  🤖  Claude Token Meter  │  {now}  │ src: {source:<9}│")
    print(f"├─────────────────────────────────────────────────┤")
    print(f"│  Weekly:  {color}{fmt_num(stats['weeklyTotal']):>10}{rst}  {bar}")
    print(f"│  Daily:   {fmt_num(stats['dailyTotal']):>10}  Input:  {fmt_num(stats['inputTokens'])}")
    print(f"│  Session: {fmt_num(stats['sessionTotal']):>10}  Output: {fmt_num(stats['outputTokens'])}")
    print(f"│  Cache R: {fmt_num(stats['cacheRead']):>10}  Cache W:{fmt_num(stats['cacheWrite'])}")
    print(f"│  Cost:    ${stats['costUSD']:>9.4f}  Model:  {stats.get('model','---')}")
    print(f"│  Device:  {device_ip}")
    print(f"└─────────────────────────────────────────────────┘")


# ════════════════════════════════════════════════════════════
#   MAIN
# ════════════════════════════════════════════════════════════

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Claude Token Meter — companion data sender",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--ip",       default=None,      help="ESP32 IP or hostname (e.g. 192.168.1.42 or claude-meter.local)")
    parser.add_argument("--discover", action="store_true",help="Auto-discover device on LAN")
    parser.add_argument("--interval", type=int, default=POLL_INTERVAL, help="Update interval in seconds (default 30)")
    parser.add_argument("--api-key",  default=None,      help="Anthropic org admin key for API usage (optional)")
    parser.add_argument("--limit",    type=int, default=None, help="Weekly token limit to set on device")
    parser.add_argument("--once",     action="store_true", help="Send one update then exit")
    parser.add_argument("--simulate", action="store_true", help="Use simulated data (for testing)")
    args = parser.parse_args()

    print(f"╔═══════════════════════════════════════════╗")
    print(f"║  Claude Token Meter Companion  v{VERSION}   ║")
    print(f"╚═══════════════════════════════════════════╝\n")

    # Quick scan — show what Claude Code files exist on this machine
    print("[companion] Scanning for Claude Code data...")
    _dirs = get_claude_dirs()
    if _dirs:
        for _d in _dirs:
            try:
                _db = len(list(_d.rglob("*.db")))
                _js = len(list(_d.rglob("*.json"))) + len(list(_d.rglob("*.jsonl")))
                print(f"  Found: {_d}  ({_db} .db, {_js} .json/.jsonl)")
            except Exception:
                print(f"  Found: {_d}  (could not enumerate)")
    else:
        print("  WARNING: No Claude Code directory found!")
        print("  Install Claude Code: https://claude.ai/code")
    print()

    # ── Find device ──────────────────────────────────────────
    device_ip = args.ip
    if not device_ip or args.discover:
        device_ip = discover_device()
        if not device_ip:
            print("❌ Could not find Claude Token Meter on the network.")
            print("   Try:  --ip 192.168.x.x  or  --ip claude-meter.local")
            sys.exit(1)

    # ── Verify device reachable ─────────────────────────────
    status = get_device_status(device_ip)
    if status:
        print(f"✅ Device online  FW: {status.get('fwVersion','?')}  IP: {status.get('ip','?')}")
    else:
        print(f"⚠  Device at {device_ip} not responding — will keep trying...")

    # ── Push limit if specified ──────────────────────────────
    if args.limit:
        if set_limit_on_device(device_ip, args.limit):
            print(f"✅ Weekly limit set to {fmt_num(args.limit)}")
        else:
            print(f"⚠  Could not set limit on device")

    # ── Main loop ───────────────────────────────────────────
    session_tokens = 0
    last_weekly    = 0
    iteration      = 0

    print(f"\n📡  Polling every {args.interval}s  →  {device_ip}")
    print("    Press Ctrl+C to stop\n")

    try:
        while True:
            iteration += 1
            stats: Optional[Dict] = None
            source = "none"

            if args.simulate:
                # ── Simulated data ───────────────────────────
                source = "simulate"
                stats = {
                    "weeklyTotal":  min(iteration * 12340, WEEKLY_DEFAULT),
                    "dailyTotal":   min(iteration * 1800,  WEEKLY_DEFAULT // 7),
                    "sessionTotal": iteration * 480,
                    "inputTokens":  iteration * 9000,
                    "outputTokens": iteration * 3340,
                    "cacheRead":    iteration * 200,
                    "cacheWrite":   iteration * 100,
                    "model":        "claude-sonnet-4-20250514",
                    "costUSD":      iteration * 0.0423,
                    "weeklyLimit":  args.limit or WEEKLY_DEFAULT,
                }

            elif args.api_key:
                # ── Anthropic Admin API ──────────────────────
                source = "api"
                stats = read_anthropic_api_stats(args.api_key)
                if not stats:
                    print("⚠  API returned no data, falling back to local files")

            if not stats:
                # ── Claude Code local files ──────────────────
                source = "local"
                stats = read_claude_code_stats()

            if not stats:
                print(f"⚠  [{datetime.datetime.now():%H:%M:%S}] No data found. "
                      f"Is Claude Code running?")
                if args.once:
                    break
                time.sleep(args.interval)
                continue

            # Compute session delta
            if last_weekly > 0 and stats["weeklyTotal"] >= last_weekly:
                session_tokens += stats["weeklyTotal"] - last_weekly
            stats["sessionTotal"]  = session_tokens
            stats["weeklyLimit"]   = args.limit or WEEKLY_DEFAULT
            last_weekly = stats["weeklyTotal"]

            # Send to device
            ok = send_to_device(device_ip, stats)
            status_icon = "✅" if ok else "❌"
            print_stats(stats, device_ip, source)
            print(f"  {status_icon} Sent to device  [{datetime.datetime.now():%H:%M:%S}]")

            if args.once:
                break

            time.sleep(args.interval)

    except KeyboardInterrupt:
        print("\n\n👋  Stopped.\n")


if __name__ == "__main__":
    main()
