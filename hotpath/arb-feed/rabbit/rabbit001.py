#!/usr/bin/env python3
"""RABBIT-001 — observe-only Shyft RabbitStream vs OrbitFlare CORE-010 FRAMED.

Does not send. Does not touch ONESHOT, STATE-007, CORE-001/010, or recon.
"""
from __future__ import annotations

import json
import os
import queue
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path

os.environ.setdefault("GRPC_VERBOSITY", "ERROR")
os.environ.setdefault("GRPC_TRACE", "")

import grpc

HERE = Path(__file__).resolve().parent
GEN = Path("/home/louis/arb-state/shyft/gen")
sys.path.insert(0, str(GEN))
sys.path.insert(0, str(HERE.resolve().parents[1] / "arb-cap"))
sys.path.insert(0, str(Path("/home/louis/arb-cap")))

import geyser_pb2
import geyser_pb2_grpc

CAP = Path("/home/louis/captures/rabbit")
OF_AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
ENV = Path.home() / ".arb-state007.env"
METRICS = CAP / "METRICS.json"
JOURNAL = CAP / "race.jsonl"
RABBIT_LOG = CAP / "rabbit.jsonl"

# Official six venue IDs (same as CORE-009 / live001).
PROG = {
    "dlmm": "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo",
    "pump": "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA",
    "clmm": "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK",
    "cpmm": "CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C",
    "damm": "cpamdpZCGKUy5JxQXB4dcpGPiikHawvSWAd6mEn1sGG",
    "orca": "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc",
}
PROG_REV = {v: k for k, v in PROG.items()}
HOST_DEFAULT = "rabbitstream.fra.shyft.to:443"
LAT_CAP = 16384
TABLE_CAP = 80_000
LEAD_BUCKETS = (
    (20_000_000, ">20ms"),
    (10_000_000, "10-20ms"),
    (5_000_000, "5-10ms"),
    (2_000_000, "2-5ms"),
    (1_000_000, "1-2ms"),
    (500_000, "0.5-1ms"),
    (0, "0-0.5ms"),
)


def _mono() -> int:
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


def _utc() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime()) + "Z"


def _b58(raw: bytes) -> str:
    import record_dlmm as d
    return d._pk(raw)


def load_token() -> tuple[str, str]:
    env = {}
    if ENV.exists():
        for line in ENV.read_text(encoding="utf-8").splitlines():
            line = line.strip().strip("\r")
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip("\r")
    token = (
        env.get("SHYFT_TOKEN")
        or env.get("SHYFT_X_TOKEN")
        or os.environ.get("SHYFT_TOKEN")
        or os.environ.get("SHYFT_X_TOKEN")
        or ""
    )
    host = (
        os.environ.get("RABBIT_GRPC_URL")
        or env.get("RABBIT_GRPC_URL")
        or HOST_DEFAULT
    )
    host = host.replace("https://", "").replace("http://", "").rstrip("/")
    if ":" not in host:
        host += ":443"
    return host, token


def load_univ() -> tuple[set[str], set[str]]:
    known: set[str] = set()
    route0: set[str] = set()
    if not UNIV.exists():
        return known, route0
    try:
        u = json.loads(UNIV.read_text(encoding="utf-8"))
    except Exception:
        return known, route0
    for p in u.get("pools") or []:
        pk = p.get("pubkey")
        if pk:
            known.add(pk)
    for r in u.get("routes") or []:
        if int(r.get("family") or 255) == 0:
            for k in ("p0", "p1", "dlmm", "pump"):
                if r.get(k):
                    route0.add(str(r[k]))
    return known, route0


def _pct(v: list[int], p: float) -> int:
    if not v:
        return 0
    s = sorted(v)
    return s[int(p * (len(s) - 1))]


def bucket_lead(ns: int) -> str:
    a = abs(ns)
    for lim, name in LEAD_BUCKETS:
        if a > lim:
            return name
    return "0-0.5ms"


class Race:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.by: dict[str, dict] = {}
        self.known, self.route0 = load_univ()
        self.reconnects = 0
        self.gaps = 0
        self.errors = 0
        self.valid = 0
        self.invalid = 0
        self.votes = 0
        self.proto = defaultdict(int)
        self.hot_n = 0
        self.known_n = 0
        self.route0_n = 0
        self.rabbit_seen = 0
        self.of_seen = 0
        self.evicted = 0
        self.fifo: list[str] = []
        self.lat_cb: list[int] = []
        self.lat_sig: list[int] = []
        self.lat_n: list[int] = []
        self.stream_slot = 0
        self.t0 = _mono()
        self.jf = None
        self.rf = None

    def open(self) -> None:
        CAP.mkdir(parents=True, exist_ok=True)
        self.jf = JOURNAL.open("a", encoding="utf-8")
        self.rf = RABBIT_LOG.open("a", encoding="utf-8")

    def _evict_rabbit_only(self) -> bool:
        while self.fifo:
            old = self.fifo.pop(0)
            row = self.by.get(old)
            if row is None:
                continue
            if row.get("of_framed_ns") or row.get("known") or row.get("searchable"):
                continue
            del self.by[old]
            self.evicted += 1
            return True
        return False

    def _ensure_row(self, sig: str, keep: bool) -> dict | None:
        row = self.by.get(sig)
        if row is not None:
            return row
        if len(self.by) >= TABLE_CAP:
            if not keep and not self._evict_rabbit_only():
                return None
            while len(self.by) >= TABLE_CAP and self._evict_rabbit_only():
                pass
            if len(self.by) >= TABLE_CAP and not keep:
                return None
        row = {"sig": sig}
        self.by[sig] = row
        self.fifo.append(sig)
        return row

    def put_rabbit(self, ev: dict) -> None:
        sig = ev["sig"]
        keep = bool(ev.get("known") or ev.get("searchable") or ev.get("route0"))
        with self.lock:
            self.rabbit_seen += 1
            row = self._ensure_row(sig, keep)
            if row is None:
                return
            if "rabbit_rx_ns" in row:
                return
            row.update(ev)
            self._score(row)

    def put_of(self, ev: dict) -> None:
        sig = ev["sig"]
        with self.lock:
            self.of_seen += 1
            row = self._ensure_row(sig, True)
            if row is None:
                return
            if "of_framed_ns" in row:
                return
            row.update(ev)
            self._score(row)

    def _score(self, row: dict) -> None:
        r = row.get("rabbit_rx_ns")
        f = row.get("of_framed_ns")
        if r and f:
            row["lead_ns"] = int(f) - int(r)
            row["diag_first_shred_lead_ns"] = int(row.get("of_t0_ns") or f) - int(r)
            row["first_usable"] = "rabbit" if row["lead_ns"] > 0 else (
                "of" if row["lead_ns"] < 0 else "tie"
            )
            if self.jf:
                self.jf.write(json.dumps(row, separators=(",", ":")) + "\n")

    def snapshot(self) -> dict:
        with self.lock:
            rows = list(self.by.values())
            up = (_mono() - self.t0) // 1_000_000_000
            rab = [r for r in rows if r.get("rabbit_rx_ns")]
            ofr = [r for r in rows if r.get("of_framed_ns")]
            both = [r for r in rows if r.get("lead_ns") is not None]
            leads = [int(r["lead_ns"]) for r in both]
            r_win = sum(1 for x in leads if x > 0)
            o_win = sum(1 for x in leads if x < 0)
            ties = sum(1 for x in leads if x == 0)
            hist_r = defaultdict(int)
            hist_o = defaultdict(int)
            for x in leads:
                b = bucket_lead(x)
                if x > 0:
                    hist_r[b] += 1
                elif x < 0:
                    hist_o[b] += 1

            def split(pred):
                xs = [int(r["lead_ns"]) for r in both if pred(r)]
                return {
                    "n": len(xs),
                    "p10": _pct(xs, 0.10),
                    "p25": _pct(xs, 0.25),
                    "p50": _pct(xs, 0.50),
                    "p75": _pct(xs, 0.75),
                    "p90": _pct(xs, 0.90),
                    "p95": _pct(xs, 0.95),
                    "p99": _pct(xs, 0.99),
                    "min": min(xs) if xs else 0,
                    "max": max(xs) if xs else 0,
                    "rabbit_first": sum(1 for x in xs if x > 0),
                    "of_first": sum(1 for x in xs if x < 0),
                }

            out = {
                "uptime_s": up,
                "reconnects": self.reconnects,
                "gaps": self.gaps,
                "errors": self.errors,
                "rabbit_unique": max(self.rabbit_seen, len(rab)),
                "of_framed_unique": max(self.of_seen, len(ofr)),
                "table_stored": len(rows),
                "evicted": self.evicted,
                "matched": len(both),
                "rabbit_only": len(rab) - len(both),
                "of_only": len(ofr) - len(both),
                "valid": self.valid,
                "invalid": self.invalid,
                "votes": self.votes,
                "proto": dict(self.proto),
                "hot_decode": self.hot_n,
                "known_pool": self.known_n,
                "route0": self.route0_n,
                "of_seen_by_rabbit_pct": (
                    (100.0 * len(both) / len(ofr)) if ofr else 0.0
                ),
                "rabbit_seen_by_of_pct": (
                    (100.0 * len(both) / len(rab)) if rab else 0.0
                ),
                "race": {
                    "rabbit_first": r_win,
                    "of_first": o_win,
                    "ties": ties,
                    "rabbit_first_pct": (100.0 * r_win / len(leads)) if leads else 0.0,
                    "of_first_pct": (100.0 * o_win / len(leads)) if leads else 0.0,
                    "all": split(lambda r: True),
                    "dlmm": split(lambda r: r.get("proto") == "dlmm"),
                    "pump": split(lambda r: r.get("proto") == "pump"),
                    "known": split(lambda r: r.get("known")),
                    "route0": split(lambda r: r.get("route0")),
                    "searchable": split(lambda r: r.get("searchable")),
                    "hist_rabbit_lead": dict(hist_r),
                    "hist_of_lead": dict(hist_o),
                },
                "local_lat_ns": {
                    "cb_to_ts": {
                        "p50": _pct(self.lat_cb, 0.5),
                        "p90": _pct(self.lat_cb, 0.9),
                        "p99": _pct(self.lat_cb, 0.99),
                        "p999": _pct(self.lat_cb, 0.999),
                    },
                    "ts_to_sig": {
                        "p50": _pct(self.lat_sig, 0.5),
                        "p90": _pct(self.lat_sig, 0.9),
                        "p99": _pct(self.lat_sig, 0.99),
                        "p999": _pct(self.lat_sig, 0.999),
                    },
                    "ts_to_n": {
                        "p50": _pct(self.lat_n, 0.5),
                        "p90": _pct(self.lat_n, 0.9),
                        "p99": _pct(self.lat_n, 0.99),
                        "p999": _pct(self.lat_n, 0.999),
                    },
                },
                "stream_slot": self.stream_slot,
            }
        METRICS.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        return out


def validate_tx(info) -> tuple[bool, dict]:
    """Structural FRAMED equivalent. Not execution success."""
    out = {
        "nsig": 0,
        "nkeys": 0,
        "ninstr": 0,
        "versioned": 0,
        "tx_len": 0,
        "keys": [],
        "progs": [],
        "valid": 0,
    }
    try:
        tx = info.transaction
        sigs = list(tx.signatures)
        msg = tx.message
        keys = list(msg.account_keys)
        ixs = list(msg.instructions)
        out["nsig"] = len(sigs)
        out["nkeys"] = len(keys)
        out["ninstr"] = len(ixs)
        out["versioned"] = 1 if getattr(msg, "versioned", False) else 0
        out["tx_len"] = sum(len(s) for s in sigs) + len(msg.recent_blockhash)
        if not sigs or not keys or not ixs:
            return False, out
        if bytes(info.signature) and bytes(info.signature) != bytes(sigs[0]):
            return False, out
        b58keys = []
        for k in keys:
            if len(k) != 32:
                return False, out
            b58keys.append(_b58(k))
        out["keys"] = b58keys
        for ix in ixs:
            if ix.program_id_index >= len(keys):
                return False, out
            for a in ix.accounts:
                if a >= len(keys):
                    return False, out
            out["progs"].append(b58keys[ix.program_id_index])
        out["valid"] = 1
        return True, out
    except Exception:
        return False, out


def classify(keys: list[str], known: set[str], route0: set[str]) -> dict:
    proto = None
    for k in keys:
        if k in PROG_REV:
            proto = PROG_REV[k]
            break
    kn = any(k in known for k in keys)
    r0 = any(k in route0 for k in keys)
    have_n = proto in ("dlmm", "pump")
    return {
        "proto": proto,
        "known": kn,
        "route0": r0,
        "searchable": kn and have_n,
        "have_n": have_n,
    }


class Adapter:
    def __init__(self) -> None:
        self.race = Race()
        self.q: queue.SimpleQueue = queue.SimpleQueue()
        self.req_q: queue.SimpleQueue = queue.SimpleQueue()
        self.stop = threading.Event()
        self.host, self.token = load_token()

    def worker(self) -> None:
        while not self.stop.is_set():
            try:
                item = self.q.get(timeout=0.05)
            except queue.Empty:
                continue
            kind = item[0]
            if kind == "of":
                self.race.put_of(item[1])
            elif kind == "rabbit":
                t_rx, utc, upd = item[1], item[2], item[3]
                t1 = _mono()
                self.race.lat_cb.append(t1 - t_rx)
                if len(self.race.lat_cb) > LAT_CAP:
                    del self.race.lat_cb[:1024]
                info = upd.transaction.transaction
                if info.is_vote:
                    self.race.votes += 1
                    continue
                t2 = _mono()
                ok, meta = validate_tx(info)
                t3 = _mono()
                self.race.lat_sig.append(t2 - t_rx)
                self.race.lat_n.append(t3 - t_rx)
                if not ok:
                    self.race.invalid += 1
                    continue
                self.race.valid += 1
                sig = bytes(info.signature).hex()
                cls = classify(meta["keys"], self.race.known, self.race.route0)
                if cls["proto"]:
                    self.race.proto[cls["proto"]] += 1
                if cls["have_n"]:
                    self.race.hot_n += 1
                if cls["known"]:
                    self.race.known_n += 1
                if cls["route0"]:
                    self.race.route0_n += 1
                ev = {
                    "sig": sig,
                    "source": "RABBIT",
                    "rabbit_rx_ns": t_rx,
                    "utc": utc,
                    "slot": int(upd.transaction.slot),
                    "valid": 1,
                    "nsig": meta["nsig"],
                    "nkeys": meta["nkeys"],
                    "ninstr": meta["ninstr"],
                    "versioned": meta["versioned"],
                    "tx_len": meta["tx_len"],
                    **cls,
                }
                self.race.put_rabbit(ev)
                if self.race.rf and self.race.valid % 8 == 0:
                    self.race.rf.write(json.dumps({
                        "t_rx": t_rx, "sig": sig, "slot": ev["slot"],
                        "proto": cls["proto"], "known": cls["known"],
                    }, separators=(",", ":")) + "\n")

    def of_follow(self) -> None:
        while not OF_AUDIT.exists() and not self.stop.is_set():
            time.sleep(0.2)
        with OF_AUDIT.open("r", encoding="utf-8", errors="replace") as f:
            f.seek(0, os.SEEK_END)
            while not self.stop.is_set():
                line = f.readline()
                if not line:
                    time.sleep(0.01)
                    continue
                if '"kind":"frame"' not in line or '"framed"' not in line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if rec.get("kind") != "frame" or rec.get("class") != "framed":
                    continue
                sig = rec.get("sig_hex")
                if not sig:
                    continue
                self.q.put(("of", {
                    "sig": sig,
                    "of_framed_ns": int(rec.get("now_ns") or 0),
                    "of_t0_ns": int(rec.get("t0_ns") or 0),
                    "of_slot": int((rec.get("shred") or {}).get("slot") or 0),
                    "of_index": int((rec.get("shred") or {}).get("index") or 0),
                }))

    def _subscribe(self):
        req = geyser_pb2.SubscribeRequest()
        flt = geyser_pb2.SubscribeRequestFilterTransactions()
        flt.vote = False
        flt.failed = False
        flt.account_include.extend(PROG.values())
        req.transactions["six"].CopyFrom(flt)
        req.commitment = geyser_pb2.PROCESSED
        return req

    def _req_iter(self):
        yield self._subscribe()
        while not self.stop.is_set():
            try:
                yield self.req_q.get(timeout=1.0)
            except queue.Empty:
                ping = geyser_pb2.SubscribeRequest()
                ping.ping.id = 1
                yield ping

    def grpc_loop(self) -> None:
        if not self.token:
            raise RuntimeError("SHYFT_TOKEN missing")
        creds = grpc.ssl_channel_credentials()
        opts = (
            ("grpc.max_receive_message_length", 1024 * 1024 * 1024),
            ("grpc.keepalive_time_ms", 10_000),
            ("grpc.keepalive_timeout_ms", 5_000),
        )
        print(
            f"RABBIT-001  observe-only  host={self.host.split(':')[0]} "
            f"region=fra  token_len={len(self.token)}  FUNDED=untouched",
            flush=True,
        )
        while not self.stop.is_set():
            try:
                ch = grpc.secure_channel(self.host, creds, options=opts)
                stub = geyser_pb2_grpc.GeyserStub(ch)
                print("RABBIT-001  subscribe transactions six-venues", flush=True)
                for upd in stub.Subscribe(self._req_iter(), metadata=(("x-token", self.token),)):
                    t_rx = _mono()
                    utc = _utc()
                    if upd.HasField("ping"):
                        pong = geyser_pb2.SubscribeRequest()
                        pong.ping.id = 1
                        self.req_q.put(pong)
                        continue
                    if upd.HasField("pong"):
                        continue
                    if upd.HasField("transaction"):
                        self.race.stream_slot = int(upd.transaction.slot)
                        self.q.put(("rabbit", t_rx, utc, upd))
            except grpc.RpcError as ex:
                self.race.reconnects += 1
                self.race.errors += 1
                code = ex.code().name if hasattr(ex, "code") else "RPC"
                print(f"RABBIT-001  reconnect {code}", flush=True)
                time.sleep(1.5)
            except Exception as ex:
                self.race.reconnects += 1
                self.race.errors += 1
                print(f"RABBIT-001  reconnect {type(ex).__name__}", flush=True)
                time.sleep(1.5)

    def metrics_loop(self) -> None:
        while not self.stop.is_set():
            time.sleep(10)
            s = self.race.snapshot()
            print(
                f"RABBIT-001  up={s['uptime_s']}s rab={s['rabbit_unique']} "
                f"of={s['of_framed_unique']} match={s['matched']} "
                f"valid={s['valid']} inv={s['invalid']} "
                f"r_first={s['race']['rabbit_first']} of_first={s['race']['of_first']} "
                f"recon={s['reconnects']}",
                flush=True,
            )

    def run(self) -> int:
        self.race.open()
        threading.Thread(target=self.worker, name="r001-work", daemon=True).start()
        threading.Thread(target=self.of_follow, name="r001-of", daemon=True).start()
        threading.Thread(target=self.metrics_loop, name="r001-met", daemon=True).start()
        try:
            self.grpc_loop()
        except KeyboardInterrupt:
            self.stop.set()
        return 0


def main() -> int:
    return Adapter().run()


if __name__ == "__main__":
    raise SystemExit(main())
