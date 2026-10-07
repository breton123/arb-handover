"""Read and validate serialized Address Lookup Table accounts off path."""
from __future__ import annotations

import base64
import hashlib
import json
import struct
import time
from pathlib import Path

ALT_PROGRAM = "AddressLookupTab1e1111111111111111111111111"
META_SIZE = 56
ALT2_MAGIC = 0x32544C41
META_FORMAT = struct.Struct("<QQQBB6x32s32s32sQ")
U64_MAX = (1 << 64) - 1
SLOT_HASHES = "SysvarS1otHashes111111111111111111111111111"


def slot_hash_entries(value: dict) -> list[tuple[int, bytes]]:
    if value.get("owner") != "Sysvar1111111111111111111111111111111111111":
        raise ValueError("Invalid SlotHashes owner")
    data = value.get("data")
    if not isinstance(data, list) or len(data) != 2 or data[1] != "base64":
        raise ValueError("Invalid SlotHashes encoding")
    raw = base64.b64decode(data[0], validate=True)
    if len(raw) < 8:
        raise ValueError("Truncated SlotHashes")
    n = int.from_bytes(raw[:8], "little")
    if n > 512 or len(raw) < 8 + n * 40 or any(raw[8 + n * 40:]):
        raise ValueError("Invalid SlotHashes length")
    entries = [struct.unpack_from("<Q32s", raw, 8 + i * 40) for i in range(n)]
    if any(a[0] <= b[0] for a, b in zip(entries, entries[1:])):
        raise ValueError("Invalid SlotHashes order")
    return entries


def parse_record(value: dict, encode) -> dict | None:
    if not isinstance(value, dict) or value.get("owner") != ALT_PROGRAM or value.get("executable", False):
        return None
    data = value.get("data")
    if not isinstance(data, list) or len(data) != 2 or data[1] != "base64":
        return None
    try:
        raw = base64.b64decode(data[0], validate=True)
    except (ValueError, TypeError):
        return None
    if len(raw) < META_SIZE or (len(raw) - META_SIZE) % 32:
        return None
    if int.from_bytes(raw[:4], "little") != 1:
        return None
    count = (len(raw) - META_SIZE) // 32
    if count > 256 or raw[20] > count or raw[21] not in (0, 1):
        return None
    return {
        "owner": ALT_PROGRAM,
        "addresses": [encode(raw[i:i + 32]) for i in range(META_SIZE, len(raw), 32)],
        "deactivation_slot": int.from_bytes(raw[4:12], "little"),
        "last_extended_slot": int.from_bytes(raw[12:20], "little"),
        "last_extended_slot_start_index": raw[20],
        "authority": encode(raw[22:54]) if raw[21] else None,
        "account_sha256": hashlib.sha256(raw).hexdigest(),
    }


def parse_account(value: dict, encode) -> list[str] | None:
    record = parse_record(value, encode)
    return record["addresses"] if record is not None else None


def fetch(rpc, url: str, pubkey: str, encode) -> list[str] | None:
    doc = rpc(url, "getAccountInfo", [pubkey, {"encoding": "base64"}])
    if doc.get("error"):
        return None
    value = ((doc.get("result") or {}).get("value") or {})
    return parse_account(value, encode)


def fetch_record(rpc, url: str, pubkey: str, encode) -> dict | None:
    doc = rpc(url, "getAccountInfo", [pubkey, {"encoding": "base64", "commitment": "finalized"}])
    if doc.get("error"):
        raise ValueError("ALT RPC error")
    result = doc.get("result") or {}
    slot = (result.get("context") or {}).get("slot")
    if type(slot) is not int or not 0 <= slot <= U64_MAX:
        raise ValueError("ALT RPC context missing")
    if result.get("value") is None:
        return None  # Authoritative absence, unlike a transport/RPC failure.
    record = parse_record(result["value"], encode)
    if record is None or record["last_extended_slot"] > slot or (record["deactivation_slot"] != U64_MAX and record["deactivation_slot"] > slot):
        raise ValueError("Invalid ALT account")
    # SlotHashes contains bank hashes, NOT getBlock's PoH blockhash. Read a newer
    # confirmed bank to learn the finalized observation's bank hash.
    hashes = rpc(url, "getAccountInfo", [SLOT_HASHES, {"encoding": "base64", "commitment": "confirmed", "minContextSlot": slot + 1}])
    bank_hash = None
    result = hashes.get("result") or {}
    hash_slot = (result.get("context") or {}).get("slot", 0)
    if not hashes.get("error") and type(hash_slot) is int and hash_slot > slot and result.get("value"):
        bank_hash = next((encode(h) for s, h in slot_hash_entries(result["value"]) if s == slot), None)
    record.update(observed_slot=slot, observed_bank_hash=bank_hash, hash_context_slot=hash_slot, commitment="finalized",
                  received_unix_ns=time.time_ns(), quarantined=False)
    return record


def merge_record(previous, current: dict) -> dict:
    """Do not heal a conflict automatically or infer metadata from old address lists."""
    if not isinstance(previous, dict):
        return current
    if previous.get("quarantined"):
        return previous
    old, new = previous["addresses"], current["addresses"]
    reason = None
    if old[:min(len(old), len(new))] != new[:min(len(old), len(new))]:
        reason = "prefix_conflict"
    elif current["observed_slot"] < previous["observed_slot"]:
        return previous
    elif len(new) < len(old) or current["last_extended_slot"] < previous["last_extended_slot"]:
        reason = "regressed_table"
    elif previous["deactivation_slot"] != U64_MAX and current["deactivation_slot"] != previous["deactivation_slot"]:
        reason = "deactivation_conflict"
    elif previous["authority"] != current["authority"] and current["authority"] is not None:
        reason = "authority_conflict"
    elif (current["observed_slot"] == previous["observed_slot"]
          and current["account_sha256"] != previous["account_sha256"]):
        reason = "finalized_snapshot_conflict"
    elif (current["observed_slot"] == previous["observed_slot"]
          and current.get("observed_bank_hash") and previous.get("observed_bank_hash")
          and current["observed_bank_hash"] != previous["observed_bank_hash"]):
        reason = "bank_identity_conflict"
    elif (current["last_extended_slot"] == previous["last_extended_slot"]
          and current["last_extended_slot_start_index"] != previous["last_extended_slot_start_index"]):
        reason = "extension_prefix_conflict"
    if reason:
        return dict(previous, quarantined=True, quarantine_reason=reason, conflicting_observation=current)
    return current


def write_cache(cache: dict, json_path: Path, binary_path: Path, decode) -> None:
    """ALT2 is a replacement snapshot; invalid keys are never padded or truncated."""
    if len(cache) > 1024:
        raise ValueError("ALT cache capacity exceeded")
    def key(value):
        raw = decode(value)
        if len(raw) > 32:
            raise ValueError("Invalid 32-byte ALT key")
        if len(raw) < 32:
            raw = raw.rjust(32, b"\x00")
        return raw
    kept = []
    for pk, item in sorted(cache.items()):
        rec = item if isinstance(item, dict) else None
        addrs = rec["addresses"] if rec else item
        try:
            if len(addrs) > 256:
                raise ValueError("ALT address limit exceeded")
            flags = 0
            meta = bytes(META_FORMAT.size)
            pk_raw = key(pk)
            addr_raw = [key(address) for address in addrs]
            if rec:
                account_hash = bytes.fromhex(rec["account_sha256"])
                if rec.get("owner") != ALT_PROGRAM or len(account_hash) != 32:
                    raise ValueError("Invalid ALT metadata provenance")
                flags = 1 | (2 if rec.get("commitment") == "finalized" else 0) | (4 if rec.get("observed_bank_hash") else 0) | (8 if rec.get("quarantined") else 0)
                meta = META_FORMAT.pack(rec["observed_slot"], rec["last_extended_slot"], rec["deactivation_slot"],
                                        rec["last_extended_slot_start_index"], rec["authority"] is not None,
                                        key(rec["observed_bank_hash"]) if flags & 4 else bytes(32),
                                        key(rec["authority"]) if rec["authority"] else bytes(32),
                                        account_hash, rec["received_unix_ns"])
            kept.append((pk_raw, flags, meta, addr_raw))
        except (ValueError, TypeError, KeyError):
            continue
    raw = bytearray(struct.pack("<II", ALT2_MAGIC, len(kept)))
    for pk_raw, flags, meta, addr_raw in kept:
        raw.extend(struct.pack("<32sHH", pk_raw, len(addr_raw), flags))
        raw.extend(meta)
        for address in addr_raw:
            raw.extend(address)
    # Build/validate both before replacing either. C validates the binary on reload.
    encoded = json.dumps(cache, separators=(",", ":"))
    for path, data in ((json_path, encoded.encode()), (binary_path, raw)):
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(data)
        tmp.replace(path)
