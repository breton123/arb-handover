//! Offline FLOWRA1 replay. Decode N, match liveuniv pools. No hot_decide. No S_today $.

use std::collections::HashMap;
use std::fs::File;
use std::io::Write;
use std::path::Path;

use flowra_probe::flw::{open_flw, read_rec};
use flowra_probe::n::{FeedN, SRC_FLOWRA};
use flowra_probe::parse::VenueBits;
use serde_json::json;

fn vote_prog() -> [u8; 32] {
    let v = bs58::decode("Vote111111111111111111111111111111111111111")
        .into_vec()
        .unwrap_or_default();
    let mut a = [0u8; 32];
    if v.len() == 32 {
        a.copy_from_slice(&v);
    }
    a
}

fn load_pools(path: &Path) -> HashMap<[u8; 32], (u32, String, String)> {
    let mut out = HashMap::new();
    let Ok(txt) = std::fs::read_to_string(path) else {
        return out;
    };
    let Ok(j) = serde_json::from_str::<serde_json::Value>(&txt) else {
        return out;
    };
    let pools = j
        .get("pools")
        .and_then(|p| p.as_array())
        .cloned()
        .unwrap_or_default();
    for p in pools {
        let pk = p.get("pubkey").and_then(|x| x.as_str()).unwrap_or("");
        let kind = p.get("kind").and_then(|x| x.as_str()).unwrap_or("").to_string();
        let idx = p.get("idx").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
        if let Ok(raw) = bs58::decode(pk).into_vec() {
            if raw.len() == 32 {
                let mut k = [0u8; 32];
                k.copy_from_slice(&raw);
                out.insert(k, (idx, kind, pk.to_string()));
            }
        }
    }
    out
}

fn main() {
    let mut flw_path = None;
    let mut univ_path = None;
    let mut out_dir = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--flw" => flw_path = args.next(),
            "--univ" => univ_path = args.next(),
            "--out" => out_dir = args.next(),
            _ => {}
        }
    }
    let flw_path = flw_path.expect("flowra_replay --flw FILE --univ liveuniv.json --out DIR");
    let univ_path = univ_path.expect("--univ liveuniv.json");
    let out_dir = out_dir.unwrap_or_else(|| ".".into());
    let _ = std::fs::create_dir_all(&out_dir);
    let pools = load_pools(Path::new(&univ_path));
    let vote = vote_prog();

    let mut f = open_flw(Path::new(&flw_path)).expect("open flw");
    let mut recs = 0u64;
    let mut firsts = 0u64;
    let mut dups = 0u64;
    let mut valid = 0u64;
    let mut bad = 0u64;
    let mut vote_n = 0u64;
    let mut nonvote = 0u64;
    let mut trig = 0u64;
    let mut known = 0u64;
    let mut known_trig = 0u64;
    let mut v_dlmm = 0u64;
    let mut v_pump = 0u64;
    let mut v_damm = 0u64;
    let mut v_clmm = 0u64;
    let mut v_cpmm = 0u64;
    let mut v_orca = 0u64;
    let mut both_dp = 0u64;
    let mut pool_hits: HashMap<String, u64> = HashMap::new();

    let npath = format!("{out_dir}/n.jsonl");
    let mut nfile = File::create(&npath).expect("n.jsonl");

    loop {
        match read_rec(&mut f) {
            Ok(None) => break,
            Err(e) => {
                eprintln!("flowra_replay: stop at rec {recs}: {e}");
                break;
            }
            Ok(Some(r)) => {
                recs += 1;
                if !r.first {
                    dups += 1;
                    continue;
                }
                firsts += 1;
                let n = FeedN::from_tx(
                    SRC_FLOWRA,
                    r.t_mono_ns,
                    r.t_tsc,
                    r.wall_utc_ns,
                    &r.tx,
                    &vote,
                );
                let v = &n.view;
                if v.valid {
                    valid += 1;
                    if v.vote {
                        vote_n += 1;
                    } else {
                        nonvote += 1;
                    }
                } else {
                    bad += 1;
                }
                if v.venues & VenueBits::DLMM != 0 {
                    v_dlmm += 1;
                }
                if v.venues & VenueBits::PUMP != 0 {
                    v_pump += 1;
                }
                if v.venues & VenueBits::DAMM != 0 {
                    v_damm += 1;
                }
                if v.venues & VenueBits::CLMM != 0 {
                    v_clmm += 1;
                }
                if v.venues & VenueBits::CPMM != 0 {
                    v_cpmm += 1;
                }
                if v.venues & VenueBits::ORCA != 0 {
                    v_orca += 1;
                }
                if v.venues & VenueBits::DLMM != 0 && v.venues & VenueBits::PUMP != 0 {
                    both_dp += 1;
                }
                if v.trigger_ok {
                    trig += 1;
                }
                let hit = pools.get(&v.pool);
                if hit.is_some() {
                    known += 1;
                    if v.trigger_ok {
                        known_trig += 1;
                    }
                    if let Some((_, _, pk)) = hit {
                        *pool_hits.entry(pk.clone()).or_insert(0) += 1;
                    }
                }
                if v.trigger_ok || hit.is_some() {
                    let sig = bs58::encode(&v.sig).into_string();
                    let pool_b58 = bs58::encode(&v.pool).into_string();
                    let line = json!({
                        "sig": sig,
                        "t_rx_mono_ns": n.t_rx_mono_ns,
                        "wall_utc_ns": n.wall_utc_ns,
                        "source": "flowra",
                        "valid": v.valid,
                        "vote": v.vote,
                        "venues": v.venues,
                        "trigger_ok": v.trigger_ok,
                        "protocol": v.protocol,
                        "direction": v.direction,
                        "amount_in": v.amount_in,
                        "min_out": v.min_out,
                        "pool": pool_b58,
                        "known_pool": hit.map(|(i, k, _)| json!({"idx": i, "kind": k})),
                        "both_dlmm_pump": v.venues & VenueBits::DLMM != 0
                            && v.venues & VenueBits::PUMP != 0,
                    });
                    let _ = writeln!(nfile, "{line}");
                }
            }
        }
    }

    let mut top: Vec<(String, u64)> = pool_hits.into_iter().collect();
    top.sort_by(|a, b| b.1.cmp(&a.1));
    top.truncate(20);

    let funnel = json!({
        "flw": flw_path,
        "univ": univ_path,
        "records": recs,
        "first": firsts,
        "dup_records": dups,
        "valid": valid,
        "bad": bad,
        "vote": vote_n,
        "non_vote": nonvote,
        "venues": {
            "dlmm": v_dlmm,
            "pump": v_pump,
            "damm": v_damm,
            "clmm": v_clmm,
            "cpmm": v_cpmm,
            "orca": v_orca,
            "same_tx_dlmm_and_pump": both_dp
        },
        "supported_trigger": trig,
        "known_universe_pool": known,
        "known_pool_and_trigger": known_trig,
        "n_jsonl": npath,
        "top_known_pools": top.into_iter().map(|(k, n)| json!({"pool": k, "n": n})).collect::<Vec<_>>(),
        "note": "known_pool_and_trigger is decode+universe membership only. Not an opportunity. Do not quote S_today."
    });
    let fpath = format!("{out_dir}/funnel.json");
    std::fs::write(&fpath, serde_json::to_string_pretty(&funnel).unwrap()).unwrap();
    println!("{}", serde_json::to_string_pretty(&funnel).unwrap());
}
