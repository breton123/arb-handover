#!/usr/bin/env python3
"""Yellowstone pAMMBay tx/state subscriber for LIVE-TRUTH.

Runs on Frankfurt. Tokens come from the box env (config.load_env),
never from this file. Does not print credentials.

Output jsonl fields consumed by live_truth:
  slot, index, sig_hex, version, t_ys_tx, t_ys_state, kind
"""
from __future__ import annotations

import json
import os
import sys
import threading
import time
from pathlib import Path

PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
SELL = bytes.fromhex("33e685a4017f83ad")
BUY_EQ = bytes.fromhex("c62e1552b4d9e870")
BUY_OUT = bytes.fromhex("66063d1201daebea")


def _b58(raw: bytes) -> str:
    alphabet = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = int.from_bytes(raw, "big")
    out = bytearray()
    while n > 0:
        n, r = divmod(n, 58)
        out.append(alphabet[r])
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return (b"1" * pad + out[::-1]).decode("ascii")


def _pamm_kind(data: bytes) -> str | None:
    if len(data) < 8:
        return None
    d = data[:8]
    if d == SELL:
        return "pump_sell"
    if d == BUY_EQ:
        return "pump_buy_exact_quote"
    if d == BUY_OUT:
        return "pump_buy_exact_out"
    return None


def _version(msg) -> int:
    v = getattr(msg, "versioned", False)
    if v:
        return 0
    hdr = getattr(msg, "version", None)
    if hdr is not None:
        try:
            return int(hdr)
        except (TypeError, ValueError):
            return 0
    return 0


def _sig_hex(info) -> str:
    sig = bytes(info.signature)
    return sig.hex() if sig else ""


def _load_shyft():
    root = os.environ.get("SHYFT_DIR", "/home/louis/arb-state/shyft")
    gen = os.path.join(root, "gen")
    sys.path.insert(0, root)
    sys.path.insert(0, gen)
    import config  # type: ignore
    import grpc  # type: ignore
    import geyser_pb2  # type: ignore
    import geyser_pb2_grpc  # type: ignore

    return config, grpc, geyser_pb2, geyser_pb2_grpc


def _sub_request(geyser_pb2):
    req = geyser_pb2.SubscribeRequest()
    sl = geyser_pb2.SubscribeRequestFilterSlots()
    sl.filter_by_commitment = False
    sl.interslot_updates = True
    req.slots["s"].CopyFrom(sl)
    txf = geyser_pb2.SubscribeRequestFilterTransactions()
    txf.vote = False
    txf.failed = False
    txf.account_include.append(PUMP)
    req.transactions["pamm"].CopyFrom(txf)
    acc = geyser_pb2.SubscribeRequestFilterAccounts()
    acc.owner.append(PUMP)
    req.accounts["pamm"].CopyFrom(acc)
    req.commitment = geyser_pb2.PROCESSED
    return req


def _req_iter(req, stop: threading.Event, geyser_pb2):
    yield req
    while not stop.is_set():
        time.sleep(15.0)
        ping = geyser_pb2.SubscribeRequest()
        ping.ping.id = 1
        yield ping


def main() -> int:
    out_path = "/data/bsc/captures/live_truth/ys.jsonl"
    for i, a in enumerate(sys.argv[1:], 1):
        if a == "--out" and i < len(sys.argv) - 1:
            out_path = sys.argv[i + 1]
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)

    config, grpc, geyser_pb2, geyser_pb2_grpc = _load_shyft()
    config.load_env()
    token = config.x_token()
    host = config.grpc_url()
    if not token or not host:
        print("ys_pamm_sub: missing host/token in env", file=sys.stderr)
        return 2

    creds = grpc.ssl_channel_credentials()
    opts = [
        ("grpc.max_receive_message_length", 64 * 1024 * 1024),
        ("grpc.keepalive_time_ms", 10000),
        ("grpc.keepalive_timeout_ms", 5000),
    ]
    req = _sub_request(geyser_pb2)
    stop = threading.Event()
    n_tx = 0
    n_pamm = 0
    n_acc = 0
    pending: dict[str, float] = {}

    print(
        f"ys_pamm_sub  host={host.split(':')[0]}  out={out_path}",
        flush=True,
    )
    fout = open(out_path, "a", buffering=1)

    while not stop.is_set():
        channel = None
        try:
            channel = grpc.secure_channel(host, creds, options=opts)
            stub = geyser_pb2_grpc.GeyserStub(channel)
            for upd in stub.Subscribe(
                _req_iter(req, stop, geyser_pb2),
                metadata=(("x-token", token),),
            ):
                now = time.time_ns()
                if upd.HasField("ping"):
                    continue
                if upd.HasField("account"):
                    n_acc += 1
                    info = upd.account.account
                    sigb = bytes(info.txn_signature) if info.txn_signature else b""
                    shex = sigb.hex() if sigb else ""
                    if shex and shex in pending:
                        pending.pop(shex, None)
                    rec = {
                        "kind": "ys_state",
                        "slot": int(upd.account.slot),
                        "sig_hex": shex,
                        "pubkey": _b58(bytes(info.pubkey)),
                        "t_ys_state": now,
                        "write_version": int(info.write_version),
                    }
                    fout.write(json.dumps(rec, separators=(",", ":")) + "\n")
                    continue
                if not upd.HasField("transaction"):
                    continue
                info = upd.transaction.transaction
                if getattr(info, "is_vote", False):
                    continue
                n_tx += 1
                tx = info.transaction
                msg = tx.message
                shex = _sig_hex(info)
                keys = [_b58(bytes(k)) for k in list(msg.account_keys)]
                kinds = []
                for ix in list(msg.instructions):
                    pidx = int(ix.program_id_index)
                    if pidx >= len(keys) or keys[pidx] != PUMP:
                        continue
                    k = _pamm_kind(bytes(ix.data))
                    if k:
                        kinds.append(k)
                if kinds:
                    n_pamm += 1
                    pending[shex] = time.monotonic()
                rec = {
                    "kind": "ys_tx",
                    "slot": int(upd.transaction.slot),
                    "index": int(info.index),
                    "sig_hex": shex,
                    "version": _version(msg),
                    "t_ys_tx": now,
                    "t_ys_state": 0,
                    "pamm": bool(kinds),
                    "variants": kinds,
                }
                fout.write(json.dumps(rec, separators=(",", ":")) + "\n")
                if n_tx % 200 == 0:
                    print(
                        f"ys_pamm_sub  txs={n_tx} pamm={n_pamm} acc={n_acc}",
                        flush=True,
                    )
        except KeyboardInterrupt:
            stop.set()
        except Exception as ex:
            print(f"ys_pamm_sub  reconnect {type(ex).__name__}", flush=True)
            time.sleep(1.5)
        finally:
            if channel is not None:
                channel.close()
    fout.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
