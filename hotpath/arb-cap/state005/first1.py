import json
from pathlib import Path

p = Path("/home/louis/captures/state005/triple_0000_AeWR4X9C.json")
r = json.loads(p.read_text())
b, a, e = r["before"], r["after"], r["event"]
print("sig", r["sig"])
print("pair", r["pair"])
print("tx_slot", r["tx_slot"], "before_slot", b["slot"], "after_slot", a["slot"])
print("block_time", r["block_time"])
print("N ain", r["n_ain"], "dir", r["n_dir"], "min_out", r["min_out"])
print("event", {k: e[k] for k in e if k not in ("lb_pair", "from")})
print("before active/vol/idx/last", b["active_id"], b["vol_acc"], b["vol_ref"], b["idx_ref"], b["last_upd"])
print("after  active/vol/idx/last", a["active_id"], a["vol_acc"], a["vol_ref"], a["idx_ref"], a["last_upd"])
print("params", b["parameters"])
print("reserve before", b["reserve_x"], b["reserve_y"], "vault", b["vault_x"], b["vault_y"])
print("reserve after ", a["reserve_x"], a["reserve_y"], "vault", a["vault_x"], a["vault_y"])
id0 = e["start_bin_id"]
bb = next(x for x in b["bins"] if x["id"] == id0)
aa = next(x for x in a["bins"] if x["id"] == id0)
print("bin", id0, "before", bb, "after", aa)
print("stale_before", r["stale_before"])
