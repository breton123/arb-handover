#![recursion_limit = "256"]
//! Observe-only Flowra Frankfurt pending-tx probe.
//! Official surface only: docs.flowra.wtf + flowrawtf/mev-protos.
//! No SendBundle. No hot_decide. No arb-core / arb-exec / shred / SWQOS.

use flowra_probe::parse;
use flowra_probe::record;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{sync_channel, TrySendError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use ed25519_dalek::{Signer, SigningKey};
use parse::{extract_sig, parse_tx, Dedup, VenueBits};
use record::{paths_for, recorder_thread, Rec, FLAG_DISCARD, FLAG_FORWARDED, FLAG_REPAIR, FLAG_STAKED, FLAG_TRACER, FLAG_VOTE};
use tokio::sync::RwLock;
use tonic::metadata::MetadataValue;
use tonic::service::Interceptor;
use tonic::transport::{Channel, ClientTlsConfig};
use tonic::{Request, Status};

pub mod auth {
    tonic::include_proto!("auth");
}
pub mod searcher {
    tonic::include_proto!("searcher");
}
pub mod packet {
    tonic::include_proto!("packet");
}
pub mod bundle {
    tonic::include_proto!("bundle");
}
pub mod shared {
    tonic::include_proto!("shared");
}

use auth::auth_service_client::AuthServiceClient;
use auth::{GenerateAuthChallengeRequest, GenerateAuthTokensRequest, RefreshAccessTokenRequest, Role};
use searcher::searcher_service_client::SearcherServiceClient;
use searcher::{ConnectedLeadersRequest, PendingTxSubscriptionRequest};

const ENDPOINT_DEFAULT: &str = "https://frankfurt.mainnet.blockengine.flowra.wtf";
const REGION: &str = "frankfurt";
const PROTO_REPO: &str = "flowrawtf/mev-protos@main";
const DOCS: &str = "https://docs.flowra.wtf/searchers/";
const REC_Q: usize = 8192;
const WORK_Q: usize = 32768;
const HIST_CAP: usize = 200_000;
const RPC_SAMPLE: usize = 64;
const RPC_POLL_MS: u64 = 2000;
const RPC_WAIT_MS: u64 = 90_000;

#[derive(Clone)]
struct Bearer {
    token: Arc<RwLock<String>>,
}

impl Interceptor for Bearer {
    fn call(&mut self, mut req: Request<()>) -> Result<Request<()>, Status> {
        let tok = self
            .token
            .try_read()
            .map(|g| g.clone())
            .unwrap_or_default();
        if tok.is_empty() {
            return Err(Status::unauthenticated("no token"));
        }
        /* never log tok */
        let val = MetadataValue::try_from(format!("Bearer {tok}"))
            .map_err(|_| Status::unauthenticated("bad token"))?;
        req.metadata_mut().insert("authorization", val);
        Ok(req)
    }
}

#[derive(Default)]
struct Hist {
    v: Vec<u32>,
}

impl Hist {
    fn rec(&mut self, ns: u64) {
        if self.v.len() < HIST_CAP {
            self.v.push(ns.min(u32::MAX as u64) as u32);
        }
    }
    fn pct(&self, p: f64) -> u64 {
        if self.v.is_empty() {
            return 0;
        }
        let mut s = self.v.clone();
        s.sort_unstable();
        let i = ((p * (s.len() as f64 - 1.0)).round() as usize).min(s.len() - 1);
        s[i] as u64
    }
}

struct Stats {
    messages: AtomicU64,
    packets: AtomicU64,
    unique: AtomicU64,
    dups: AtomicU64,
    bytes: AtomicU64,
    valid: AtomicU64,
    bad: AtomicU64,
    vote: AtomicU64,
    nonvote: AtomicU64,
    legacy: AtomicU64,
    v0: AtomicU64,
    v1: AtomicU64,
    dlmm: AtomicU64,
    pump: AtomicU64,
    damm: AtomicU64,
    clmm: AtomicU64,
    cpmm: AtomicU64,
    orca: AtomicU64,
    trigger: AtomicU64,
    capture_drop: AtomicU64,
    work_drop: AtomicU64,
    reconnects: AtomicU64,
    rec_bytes: AtomicU64,
    rec_n: AtomicU64,
}

impl Stats {
    fn new() -> Self {
        Self {
            messages: AtomicU64::new(0),
            packets: AtomicU64::new(0),
            unique: AtomicU64::new(0),
            dups: AtomicU64::new(0),
            bytes: AtomicU64::new(0),
            valid: AtomicU64::new(0),
            bad: AtomicU64::new(0),
            vote: AtomicU64::new(0),
            nonvote: AtomicU64::new(0),
            legacy: AtomicU64::new(0),
            v0: AtomicU64::new(0),
            v1: AtomicU64::new(0),
            dlmm: AtomicU64::new(0),
            pump: AtomicU64::new(0),
            damm: AtomicU64::new(0),
            clmm: AtomicU64::new(0),
            cpmm: AtomicU64::new(0),
            orca: AtomicU64::new(0),
            trigger: AtomicU64::new(0),
            capture_drop: AtomicU64::new(0),
            work_drop: AtomicU64::new(0),
            reconnects: AtomicU64::new(0),
            rec_bytes: AtomicU64::new(0),
            rec_n: AtomicU64::new(0),
        }
    }
    fn g(&self, a: &AtomicU64) -> u64 {
        a.load(Ordering::Relaxed)
    }
}

fn strip_cr(s: &str) -> String {
    s.trim().trim_end_matches('\r').to_string()
}

fn load_env_file(path: &Path) {
    let Ok(txt) = std::fs::read_to_string(path) else {
        return;
    };
    for line in txt.lines() {
        let line = line.trim().trim_end_matches('\r');
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let k = k.trim();
        let v = v.trim().trim_matches('"').trim_end_matches('\r');
        if k.is_empty() {
            continue;
        }
        if std::env::var_os(k).is_none() {
            std::env::set_var(k, v);
        }
    }
}

fn load_env() {
    if let Ok(p) = std::env::var("FLOWRA_ENV") {
        load_env_file(Path::new(&p));
    }
    if let Some(h) = std::env::var_os("HOME") {
        load_env_file(&PathBuf::from(h).join(".flowra.env"));
    }
    load_env_file(Path::new("/home/louis/.flowra.env"));
    load_env_file(Path::new("/home/louis/.arb-smoke.env"));
    load_env_file(Path::new(".env"));
    load_env_file(Path::new("../.env"));
    load_env_file(Path::new("../../.env"));
}

fn b58_32(s: &str) -> Result<[u8; 32], String> {
    let raw = bs58::decode(strip_cr(s))
        .into_vec()
        .map_err(|_| "bad b58".to_string())?;
    if raw.len() != 32 {
        return Err(format!("len {}", raw.len()));
    }
    let mut o = [0u8; 32];
    o.copy_from_slice(&raw);
    Ok(o)
}

fn signing_key() -> Result<(SigningKey, [u8; 32], String), String> {
    let want = std::env::var("FLOWRA_KEY").map_err(|_| "FLOWRA_KEY missing".to_string())?;
    let want = strip_cr(&want);
    let want_pk = b58_32(&want)?;
    let secret = std::env::var("PRIVATE_KEY").map_err(|_| "PRIVATE_KEY missing".to_string())?;
    let raw = bs58::decode(strip_cr(&secret))
        .into_vec()
        .map_err(|_| "bad secret".to_string())?;
    let seed = if raw.len() == 64 {
        let mut s = [0u8; 32];
        s.copy_from_slice(&raw[..32]);
        s
    } else if raw.len() == 32 {
        let mut s = [0u8; 32];
        s.copy_from_slice(&raw);
        s
    } else {
        return Err(format!("secret len {}", raw.len()));
    };
    let sk = SigningKey::from_bytes(&seed);
    let have = sk.verifying_key().to_bytes();
    if have != want_pk {
        return Err("FLOWRA_KEY does not match signing key".to_string());
    }
    Ok((sk, have, want))
}

fn mono_raw_ns() -> u64 {
    #[cfg(target_os = "linux")]
    {
        let mut ts = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        unsafe {
            libc::clock_gettime(libc::CLOCK_MONOTONIC_RAW, &mut ts);
        }
        (ts.tv_sec as u64)
            .saturating_mul(1_000_000_000)
            .saturating_add(ts.tv_nsec as u64)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let mut ts = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        unsafe {
            libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts);
        }
        (ts.tv_sec as u64)
            .saturating_mul(1_000_000_000)
            .saturating_add(ts.tv_nsec as u64)
    }
}

fn wall_utc_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
}

fn rdtscp() -> u64 {
    #[cfg(target_arch = "x86_64")]
    {
        let mut aux = 0u32;
        unsafe { core::arch::x86_64::__rdtscp(&mut aux) }
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        0
    }
}

fn calib_tsc_hz() -> u64 {
    let a0 = rdtscp();
    let t0 = mono_raw_ns();
    std::thread::sleep(Duration::from_millis(100));
    let a1 = rdtscp();
    let t1 = mono_raw_ns();
    let dt = t1.saturating_sub(t0).max(1);
    let dc = a1.saturating_sub(a0);
    dc.saturating_mul(1_000_000_000) / dt
}

fn expand_home(p: &str) -> PathBuf {
    if let Some(rest) = p.strip_prefix("~/") {
        if let Some(h) = std::env::var_os("HOME") {
            return PathBuf::from(h).join(rest);
        }
    }
    PathBuf::from(p)
}

fn rpc_url() -> Option<String> {
    if let Ok(u) = std::env::var("HELIUS_RPC_URL") {
        let u = strip_cr(&u);
        if !u.is_empty() {
            return Some(u);
        }
    }
    if let Ok(u) = std::env::var("RPC_URL") {
        let u = strip_cr(&u);
        if !u.is_empty() {
            return Some(u);
        }
    }
    if let Ok(k) = std::env::var("HELIUS_API_KEY") {
        let k = strip_cr(&k);
        if !k.is_empty() {
            return Some(format!("https://mainnet.helius-rpc.com/?api-key={k}"));
        }
    }
    None
}

fn sanitize_status(s: &str) -> String {
    let mut o = s.to_string();
    for key in ["FLOWRA_KEY", "PRIVATE_KEY", "HELIUS_API_KEY", "SWQOS_KEY"] {
        if let Ok(v) = std::env::var(key) {
            let v = strip_cr(&v);
            if v.len() >= 8 {
                o = o.replace(&v, "<redacted>");
            }
        }
    }
    if let Some(u) = rpc_url() {
        if u.contains("api-key") {
            o = o.replace(&u, "<rpc>");
        }
    }
    o
}

async fn connect_channel(endpoint: &str) -> Result<Channel, String> {
    let host = endpoint
        .trim_start_matches("https://")
        .trim_start_matches("http://")
        .split('/')
        .next()
        .unwrap_or(endpoint);
    let tls = ClientTlsConfig::new()
        .domain_name(host)
        .with_enabled_roots();
    Channel::from_shared(endpoint.to_string())
        .map_err(|e| sanitize_status(&e.to_string()))?
        .tls_config(tls)
        .map_err(|e| sanitize_status(&e.to_string()))?
        .tcp_nodelay(true)
        .http2_keep_alive_interval(Duration::from_secs(15))
        .keep_alive_timeout(Duration::from_secs(10))
        .keep_alive_while_idle(true)
        .connect()
        .await
        .map_err(|e| sanitize_status(&e.to_string()))
}

async fn auth_tokens(
    ch: Channel,
    sk: &SigningKey,
    pk: [u8; 32],
    pk_b58: &str,
) -> Result<(String, String), String> {
    let mut auth = AuthServiceClient::new(ch);
    let chal = auth
        .generate_auth_challenge(GenerateAuthChallengeRequest {
            role: Role::Searcher as i32,
            pubkey: pk.to_vec(),
        })
        .await
        .map_err(|e| sanitize_status(&e.to_string()))?
        .into_inner();
    let msg = format!("{}-{}", pk_b58, chal.challenge);
    let sig = sk.sign(msg.as_bytes());
    let toks = auth
        .generate_auth_tokens(GenerateAuthTokensRequest {
            challenge: msg,
            client_pubkey: pk.to_vec(),
            signed_challenge: sig.to_bytes().to_vec(),
        })
        .await
        .map_err(|e| sanitize_status(&e.to_string()))?
        .into_inner();
    let access = toks.access_token.ok_or("no access token")?.value;
    let refresh = toks.refresh_token.map(|t| t.value).unwrap_or_default();
    if access.is_empty() {
        return Err("empty access token".into());
    }
    Ok((access, refresh))
}

async fn refresh_access(ch: Channel, refresh: &str) -> Result<String, String> {
    if refresh.is_empty() {
        return Err("no refresh".into());
    }
    let mut auth = AuthServiceClient::new(ch);
    let r = auth
        .refresh_access_token(RefreshAccessTokenRequest {
            refresh_token: refresh.to_string(),
        })
        .await
        .map_err(|e| sanitize_status(&e.to_string()))?
        .into_inner();
    r.access_token
        .map(|t| t.value)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| "empty refresh".into())
}

struct Work {
    t_avail_ns: u64,
    t_mono_ns: u64,
    t_tsc: u64,
    wall_utc_ns: u64,
    server_side_sec: i64,
    server_side_nsec: i32,
    expiration_sec: i64,
    expiration_nsec: i32,
    sender_stake: u64,
    port: u32,
    flags: u32,
    addr: Vec<u8>,
    tx: Vec<u8>,
}

#[derive(Clone)]
struct Sample {
    sig_b58: String,
    wall_rx_ns: u64,
}

fn telemetry_line(st: &Stats, up: u64, tsc_hz: u64) {
    let _ = tsc_hz;
    let msg = st.g(&st.messages);
    let uniq = st.g(&st.unique);
    let dups = st.g(&st.dups);
    let pk = st.g(&st.packets);
    let bytes = st.g(&st.bytes);
    let txps = if up > 0 { pk as f64 / up as f64 } else { 0.0 };
    let mbps = if up > 0 {
        (bytes as f64 * 8.0) / (up as f64 * 1_000_000.0)
    } else {
        0.0
    };
    eprintln!(
        "FLOWRA  up={up}s  msg={msg}  uniq={uniq}  dup={dups}  {txps:.1} tx/s  {mbps:.2} Mbps"
    );
    eprintln!(
        "        valid={}  bad={}  vote={}  non-vote={}  legacy={} v0={} v1={}",
        st.g(&st.valid),
        st.g(&st.bad),
        st.g(&st.vote),
        st.g(&st.nonvote),
        st.g(&st.legacy),
        st.g(&st.v0),
        st.g(&st.v1)
    );
    eprintln!(
        "        DLMM={} Pump={} DAMM={} CLMM={} CPMM={} Orca={}  trig={}  capture_drop={}  reconnect={}",
        st.g(&st.dlmm),
        st.g(&st.pump),
        st.g(&st.damm),
        st.g(&st.clmm),
        st.g(&st.cpmm),
        st.g(&st.orca),
        st.g(&st.trigger),
        st.g(&st.capture_drop),
        st.g(&st.reconnects)
    );
}

fn rpc_post(url: &str, body: &str) -> Result<serde_json::Value, String> {
    let resp = ureq::post(url)
        .set("Content-Type", "application/json")
        .set("Accept", "application/json")
        .timeout(Duration::from_secs(8))
        .send_string(body)
        .map_err(|e| format!("rpc {}", e))?;
    let txt = resp.into_string().map_err(|e| format!("rpc body {e}"))?;
    serde_json::from_str(&txt).map_err(|e| format!("rpc json {e}"))
}

fn rpc_landing_sample(samples: &[Sample]) -> serde_json::Value {
    let Some(url) = rpc_url() else {
        return serde_json::json!({
            "sampled": 0,
            "note": "no RPC_URL / HELIUS_RPC_URL / HELIUS_API_KEY; skipped"
        });
    };
    if samples.is_empty() {
        return serde_json::json!({ "sampled": 0, "landed": 0, "expired": 0 });
    }
    let mut pending: HashMap<String, u64> = samples
        .iter()
        .map(|s| (s.sig_b58.clone(), s.wall_rx_ns))
        .collect();
    let mut landed: Vec<(String, u64)> = Vec::new();
    let start = Instant::now();
    while !pending.is_empty() && start.elapsed() < Duration::from_millis(RPC_WAIT_MS) {
        let keys: Vec<String> = pending.keys().cloned().collect();
        for chunk in keys.chunks(100) {
            let params = serde_json::json!([
                chunk,
                { "searchTransactionHistory": true }
            ]);
            let body = serde_json::json!({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "getSignatureStatuses",
                "params": params
            })
            .to_string();
            let Ok(j) = rpc_post(&url, &body) else {
                continue;
            };
            let Some(arr) = j.get("result").and_then(|r| r.get("value")).and_then(|v| v.as_array()) else {
                continue;
            };
            let now = wall_utc_ns();
            for (i, ent) in arr.iter().enumerate() {
                if ent.is_null() {
                    continue;
                }
                if ent.get("err").is_some() || ent.get("confirmationStatus").is_some() || ent.get("slot").is_some() {
                    let sig = &chunk[i];
                    if let Some(rx) = pending.remove(sig) {
                        landed.push((sig.clone(), now.saturating_sub(rx)));
                    }
                }
            }
        }
        if pending.is_empty() {
            break;
        }
        std::thread::sleep(Duration::from_millis(RPC_POLL_MS));
    }
    let mut lat: Vec<u64> = landed.iter().map(|(_, d)| *d).collect();
    lat.sort_unstable();
    let pct = |p: f64| -> u64 {
        if lat.is_empty() {
            return 0;
        }
        lat[((p * (lat.len() as f64 - 1.0)).round() as usize).min(lat.len() - 1)]
    };
    serde_json::json!({
        "sampled": samples.len(),
        "eventually_landed": landed.len(),
        "expired_or_not_seen": pending.len(),
        "flowra_to_first_rpc_obs_ns": {
            "p50": pct(0.50),
            "p90": pct(0.90),
            "p99": pct(0.99)
        },
        "note": "RPC observation latency is NOT leader sequencing latency."
    })
}

#[tokio::main]
async fn main() {
    /* debug crates must not dump Authorization */
    std::env::set_var("RUST_LOG", "off");
    std::env::set_var("RUST_BACKTRACE", "0");
    load_env();

    let endpoint = strip_cr(
        &std::env::var("FLOWRA_ENDPOINT").unwrap_or_else(|_| ENDPOINT_DEFAULT.to_string()),
    );
    let dur_s: u64 = std::env::var("FLOWRA_DURATION_SEC")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(600);
    let cap_dir = expand_home(
        &std::env::var("FLOWRA_CAPTURE_DIR").unwrap_or_else(|_| "~/captures/flowra".into()),
    );

    let (sk, pk, pk_b58) = match signing_key() {
        Ok(x) => x,
        Err(e) => {
            eprintln!("flowra_probe: {e}");
            std::process::exit(2);
        }
    };

    let vote_prog = match bs58::decode("Vote111111111111111111111111111111111111111").into_vec() {
        Ok(v) if v.len() == 32 => {
            let mut a = [0u8; 32];
            a.copy_from_slice(&v);
            a
        }
        _ => [0u8; 32],
    };

    let tsc_hz = calib_tsc_hz();
    let start_wall = wall_utc_ns();
    let start_mono = mono_raw_ns();
    let paths = paths_for(&cap_dir);
    let _ = std::fs::create_dir_all(&cap_dir);

    let st = Arc::new(Stats::new());
    let (rec_tx, rec_rx) = sync_channel::<Rec>(REC_Q);
    let (work_tx, work_rx) = sync_channel::<Work>(WORK_Q);
    let st_rec = Arc::clone(&st);
    let paths_c = record::Paths {
        dir: paths.dir.clone(),
        flw: paths.flw.clone(),
        idx: paths.idx.clone(),
        meta: paths.meta.clone(),
    };
    std::thread::Builder::new()
        .name("flowra-rec".into())
        .spawn(move || {
            recorder_thread(
                rec_rx,
                paths_c,
                tsc_hz,
                start_wall,
                start_mono,
                &st_rec.rec_bytes,
                &st_rec.rec_n,
            );
        })
        .expect("recorder thread");

    let hist_ts = Arc::new(Mutex::new(Hist::default()));
    let hist_sig = Arc::new(Mutex::new(Hist::default()));
    let hist_cls = Arc::new(Mutex::new(Hist::default()));
    let hist_dec = Arc::new(Mutex::new(Hist::default()));
    let hist_sum = Arc::new(Mutex::new(Hist::default()));
    let sources: Arc<Mutex<HashMap<String, u64>>> = Arc::new(Mutex::new(HashMap::new()));
    let samples: Arc<Mutex<Vec<Sample>>> = Arc::new(Mutex::new(Vec::new()));
    let run = Arc::new(AtomicBool::new(true));

    let rec_tx_w = rec_tx.clone();
    let st_w = Arc::clone(&st);
    let hist_ts_w = Arc::clone(&hist_ts);
    let hist_sig_w = Arc::clone(&hist_sig);
    let hist_cls_w = Arc::clone(&hist_cls);
    let hist_dec_w = Arc::clone(&hist_dec);
    let hist_sum_w = Arc::clone(&hist_sum);
    let sources_w = Arc::clone(&sources);
    let samples_w = Arc::clone(&samples);
    let run_w = Arc::clone(&run);
    std::thread::Builder::new()
        .name("flowra-work".into())
        .spawn(move || {
            let mut dedup = Dedup::new();
            while let Ok(w) = work_rx.recv() {
                if !run_w.load(Ordering::Relaxed) && work_rx.try_recv().is_err() {
                    /* drain then stop handled by disconnect */
                }
                let t0 = rdtscp();
                let t_rx_done = w.t_tsc;
                /* timestamp already taken on receive thread; measure ingest = avail???rx via mono */
                let ingest = w.t_mono_ns.saturating_sub(w.t_avail_ns);
                if let Ok(mut h) = hist_ts_w.lock() {
                    h.rec(ingest);
                }

                let t_sig0 = rdtscp();
                let sig = extract_sig(&w.tx).unwrap_or([0u8; 64]);
                let t_sig1 = rdtscp();

                let t_cls0 = rdtscp();
                let view = parse_tx(&w.tx, &vote_prog);
                let t_cls1 = rdtscp();
                let t_dec1 = rdtscp(); /* decode is inside parse_tx; same window */

                let cyc = |a: u64, b: u64| -> u64 {
                    if tsc_hz == 0 {
                        return 0;
                    }
                    b.saturating_sub(a).saturating_mul(1_000_000_000) / tsc_hz
                };
                if let Ok(mut h) = hist_sig_w.lock() {
                    h.rec(cyc(t_sig0, t_sig1));
                }
                if let Ok(mut h) = hist_cls_w.lock() {
                    h.rec(cyc(t_cls0, t_cls1));
                }
                if let Ok(mut h) = hist_dec_w.lock() {
                    h.rec(cyc(t_cls0, t_dec1));
                }
                if let Ok(mut h) = hist_sum_w.lock() {
                    h.rec(ingest.saturating_add(cyc(t_sig0, t_dec1)));
                }
                let _ = (t0, t_rx_done);

                let first = if sig != [0u8; 64] {
                    dedup.see(&sig, w.t_mono_ns, w.t_tsc, w.wall_utc_ns)
                } else {
                    false
                };
                if first {
                    st_w.unique.store(dedup.unique(), Ordering::Relaxed);
                } else if sig != [0u8; 64] {
                    st_w.dups.fetch_add(1, Ordering::Relaxed);
                }
                let rec = Rec {
                    first,
                    t_mono_ns: w.t_mono_ns,
                    t_tsc: w.t_tsc,
                    wall_utc_ns: w.wall_utc_ns,
                    server_side_sec: w.server_side_sec,
                    server_side_nsec: w.server_side_nsec,
                    expiration_sec: w.expiration_sec,
                    expiration_nsec: w.expiration_nsec,
                    sender_stake: w.sender_stake,
                    port: w.port,
                    flags: w.flags,
                    sig,
                    tx: w.tx,
                    addr: w.addr.clone(),
                };
                if rec_tx_w.try_send(rec).is_err() {
                    st_w.capture_drop.fetch_add(1, Ordering::Relaxed);
                }

                if view.valid {
                    st_w.valid.fetch_add(1, Ordering::Relaxed);
                    if view.vote {
                        st_w.vote.fetch_add(1, Ordering::Relaxed);
                    } else {
                        st_w.nonvote.fetch_add(1, Ordering::Relaxed);
                    }
                    match view.version {
                        -2 => {
                            st_w.legacy.fetch_add(1, Ordering::Relaxed);
                        }
                        0 => {
                            st_w.v0.fetch_add(1, Ordering::Relaxed);
                        }
                        1 => {
                            st_w.v1.fetch_add(1, Ordering::Relaxed);
                        }
                        _ => {}
                    }
                    if view.venues & VenueBits::DLMM != 0 {
                        st_w.dlmm.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.venues & VenueBits::PUMP != 0 {
                        st_w.pump.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.venues & VenueBits::DAMM != 0 {
                        st_w.damm.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.venues & VenueBits::CLMM != 0 {
                        st_w.clmm.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.venues & VenueBits::CPMM != 0 {
                        st_w.cpmm.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.venues & VenueBits::ORCA != 0 {
                        st_w.orca.fetch_add(1, Ordering::Relaxed);
                    }
                    if view.trigger_ok {
                        st_w.trigger.fetch_add(1, Ordering::Relaxed);
                    }
                } else {
                    st_w.bad.fetch_add(1, Ordering::Relaxed);
                }

                if first {
                    if let Ok(mut src) = sources_w.lock() {
                        let key = String::from_utf8_lossy(&w.addr).into_owned();
                        *src.entry(key).or_insert(0) += 1;
                    }
                    if !view.vote && view.valid {
                        if let Ok(mut sm) = samples_w.lock() {
                            if sm.len() < RPC_SAMPLE {
                                sm.push(Sample {
                                    sig_b58: bs58::encode(&sig).into_string(),
                                    wall_rx_ns: w.wall_utc_ns,
                                });
                            }
                        }
                    }
                }
            }
        })
        .expect("work thread");

    let token = Arc::new(RwLock::new(String::new()));
    let refresh = Arc::new(RwLock::new(String::new()));

    eprintln!("FLOWRA probe  region={REGION}  endpoint={endpoint}");
    eprintln!("           proto={PROTO_REPO}  docs={DOCS}");
    eprintln!("           observe-only SubscribePendingTransactions accounts=[] (full firehose)");
    eprintln!("           capture {}", paths.flw.display());
    eprintln!("           runtime {dur_s}s  tsc_hz={tsc_hz}");

    let deadline = Instant::now() + Duration::from_secs(dur_s);
    {
        let st_t = Arc::clone(&st);
        tokio::spawn(async move {
            let t0 = Instant::now();
            loop {
                tokio::time::sleep(Duration::from_secs(1)).await;
                let up = t0.elapsed().as_secs().max(1);
                telemetry_line(&st_t, up, 0);
                if t0.elapsed() >= Duration::from_secs(dur_s + 2) {
                    break;
                }
            }
        });
    }
    let mut backoff_ms: u64 = 100;
    let mut leaders_note = String::from("(not fetched)");
    let mut last_err = String::new();
    let mut connected_ok = false;

    /* refresh loop */
    {
        let token_r = Arc::clone(&token);
        let refresh_r = Arc::clone(&refresh);
        let endpoint_r = endpoint.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(Duration::from_secs(45 * 60)).await;
                let ch = match connect_channel(&endpoint_r).await {
                    Ok(c) => c,
                    Err(_) => continue,
                };
                let rtok = refresh_r.read().await.clone();
                if let Ok(a) = refresh_access(ch, &rtok).await {
                    *token_r.write().await = a;
                }
            }
        });
    }

    while Instant::now() < deadline {
        match connect_channel(&endpoint).await {
            Ok(ch) => {
                match tokio::time::timeout(Duration::from_secs(15), auth_tokens(ch.clone(), &sk, pk, &pk_b58)).await {
                    Ok(Ok((access, refr))) => {
                        *token.write().await = access;
                        *refresh.write().await = refr;
                        connected_ok = true;
                        backoff_ms = 100;
                        last_err.clear();
                        eprintln!("FLOWRA auth ok (challenge-response; token not logged)");

                        if leaders_note.starts_with('(') {
                            let bearer = Bearer {
                                token: Arc::clone(&token),
                            };
                            let mut sc = SearcherServiceClient::with_interceptor(ch.clone(), bearer);
                            match tokio::time::timeout(
                                Duration::from_secs(8),
                                sc.get_connected_leaders(ConnectedLeadersRequest {}),
                            )
                            .await
                            {
                                Ok(Ok(r)) => {
                                    let n = r.into_inner().connected_validators.len();
                                    leaders_note = format!("{n} connected validators this region");
                                    eprintln!("FLOWRA {leaders_note}");
                                }
                                Ok(Err(e)) => {
                                    leaders_note = format!("leaders: {}", sanitize_status(&e.to_string()));
                                    eprintln!("FLOWRA {leaders_note}");
                                }
                                Err(_) => {
                                    leaders_note = "leaders: timeout".into();
                                    eprintln!("FLOWRA {leaders_note}");
                                }
                            }
                        }

                        let bearer = Bearer {
                            token: Arc::clone(&token),
                        };
                        let mut sc = SearcherServiceClient::with_interceptor(ch, bearer);
                        /* docs: empty or "*" = full firehose. "*" used so an empty-list
                         * implementation that matches nothing cannot silently starve us. */
                        let req = Request::new(PendingTxSubscriptionRequest {
                            accounts: vec!["*".into()],
                        });
                        eprintln!("FLOWRA subscribe SubscribePendingTransactions accounts=[*]");
                        match sc.subscribe_pending_transactions(req).await {
                            Ok(stream) => {
                                let mut inbound = stream.into_inner();
                                loop {
                                    if Instant::now() >= deadline {
                                        break;
                                    }
                                    let left = deadline.saturating_duration_since(Instant::now());
                                    let got = tokio::time::timeout(left, inbound.message()).await;
                                    match got {
                                        Ok(Ok(Some(note))) => {
                                            let t_avail = mono_raw_ns();
                                            st.messages.fetch_add(1, Ordering::Relaxed);
                                            let ss = note.server_side_ts.as_ref();
                                            let ex = note.expiration_time.as_ref();
                                            let ss_s = ss.map(|t| t.seconds).unwrap_or(0);
                                            let ss_n = ss.map(|t| t.nanos).unwrap_or(0);
                                            let ex_s = ex.map(|t| t.seconds).unwrap_or(0);
                                            let ex_n = ex.map(|t| t.nanos).unwrap_or(0);
                                            for pkt in note.transactions {
                                                let t_mono = mono_raw_ns();
                                                let t_tsc = rdtscp();
                                                let wall = wall_utc_ns();
                                                let meta = pkt.meta.as_ref();
                                                let mut flags = 0u32;
                                                if let Some(m) = meta {
                                                    if let Some(f) = m.flags.as_ref() {
                                                        if f.discard {
                                                            flags |= FLAG_DISCARD;
                                                        }
                                                        if f.forwarded {
                                                            flags |= FLAG_FORWARDED;
                                                        }
                                                        if f.repair {
                                                            flags |= FLAG_REPAIR;
                                                        }
                                                        if f.simple_vote_tx {
                                                            flags |= FLAG_VOTE;
                                                        }
                                                        if f.tracer_packet {
                                                            flags |= FLAG_TRACER;
                                                        }
                                                        if f.from_staked_node {
                                                            flags |= FLAG_STAKED;
                                                        }
                                                    }
                                                }
                                                let addr = meta
                                                    .map(|m| m.addr.as_bytes().to_vec())
                                                    .unwrap_or_default();
                                                let port = meta.map(|m| m.port).unwrap_or(0);
                                                let stake = meta.map(|m| m.sender_stake).unwrap_or(0);
                                                let tx = pkt.data;
                                                st.packets.fetch_add(1, Ordering::Relaxed);
                                                st.bytes.fetch_add(tx.len() as u64, Ordering::Relaxed);
                                                let work = Work {
                                                    t_avail_ns: t_avail,
                                                    t_mono_ns: t_mono,
                                                    t_tsc,
                                                    wall_utc_ns: wall,
                                                    server_side_sec: ss_s,
                                                    server_side_nsec: ss_n,
                                                    expiration_sec: ex_s,
                                                    expiration_nsec: ex_n,
                                                    sender_stake: stake,
                                                    port,
                                                    flags,
                                                    addr,
                                                    tx,
                                                };
                                                if let Err(TrySendError::Full(_)) = work_tx.try_send(work) {
                                                    st.work_drop.fetch_add(1, Ordering::Relaxed);
                                                    st.capture_drop.fetch_add(1, Ordering::Relaxed);
                                                }
                                            }
                                        }
                                        Ok(Ok(None)) => {
                                            last_err = "stream ended".into();
                                            break;
                                        }
                                        Ok(Err(e)) => {
                                            last_err = sanitize_status(&e.to_string());
                                            break;
                                        }
                                        Err(_) => {
                                            /* deadline */
                                            last_err.clear();
                                            break;
                                        }
                                    }
                                }
                            }
                            Err(e) => {
                                last_err = sanitize_status(&e.to_string());
                            }
                        }
                    }
                    Ok(Err(e)) => {
                        last_err = e;
                    }
                    Err(_) => {
                        last_err = "auth timeout".into();
                    }
                }
            }
            Err(e) => {
                last_err = e;
            }
        }

        if Instant::now() >= deadline {
            break;
        }
        st.reconnects.fetch_add(1, Ordering::Relaxed);
        let jitter = (rdtscp() % 50) as u64;
        let wait = backoff_ms + jitter;
        if !last_err.is_empty() {
            eprintln!("FLOWRA reconnect in {wait}ms  ({last_err})");
        }
        tokio::time::sleep(Duration::from_millis(wait)).await;
        backoff_ms = (backoff_ms.saturating_mul(2)).min(5000);
    }

    run.store(false, Ordering::Relaxed);
    drop(work_tx);
    drop(rec_tx);
    std::thread::sleep(Duration::from_millis(400));

    let up = Instant::now()
        .checked_duration_since(deadline - Duration::from_secs(dur_s))
        .map(|d| d.as_secs())
        .unwrap_or(dur_s);
    telemetry_line(&st, up.max(1), tsc_hz);

    let sm = samples.lock().ok().map(|g| g.clone()).unwrap_or_default();
    eprintln!("FLOWRA rpc sample n={} (off-path, after stream)", sm.len());
    let landing = rpc_landing_sample(&sm);

    let ns_of = |h: &Hist, p: f64| h.pct(p);
    let cyc_of = |ns: u64| -> u64 {
        if tsc_hz == 0 {
            0
        } else {
            ns.saturating_mul(tsc_hz) / 1_000_000_000
        }
    };
    let h_ts = hist_ts.lock().ok();
    let h_sig = hist_sig.lock().ok();
    let h_cls = hist_cls.lock().ok();
    let h_dec = hist_dec.lock().ok();
    let h_sum = hist_sum.lock().ok();
    let adapter = serde_json::json!({
        "tsc_hz": tsc_hz,
        "samples_capped": HIST_CAP,
        "timestamp_ingest_ns": {
            "p50": h_ts.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0),
            "p90": h_ts.as_ref().map(|h| ns_of(h, 0.90)).unwrap_or(0),
            "p99": h_ts.as_ref().map(|h| ns_of(h, 0.99)).unwrap_or(0),
        },
        "sig_extract_ns": {
            "p50": h_sig.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0),
            "p90": h_sig.as_ref().map(|h| ns_of(h, 0.90)).unwrap_or(0),
            "p99": h_sig.as_ref().map(|h| ns_of(h, 0.99)).unwrap_or(0),
        },
        "six_id_plus_decode_ns": {
            "p50": h_cls.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0),
            "p90": h_cls.as_ref().map(|h| ns_of(h, 0.90)).unwrap_or(0),
            "p99": h_cls.as_ref().map(|h| ns_of(h, 0.99)).unwrap_or(0),
        },
        "hot_decode_included_ns": {
            "p50": h_dec.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0),
            "p90": h_dec.as_ref().map(|h| ns_of(h, 0.90)).unwrap_or(0),
            "p99": h_dec.as_ref().map(|h| ns_of(h, 0.99)).unwrap_or(0),
        },
        "callback_to_classified_ns": {
            "p50": h_sum.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0),
            "p90": h_sum.as_ref().map(|h| ns_of(h, 0.90)).unwrap_or(0),
            "p99": h_sum.as_ref().map(|h| ns_of(h, 0.99)).unwrap_or(0),
        },
        "callback_to_classified_cycles_p50": cyc_of(h_sum.as_ref().map(|h| ns_of(h, 0.50)).unwrap_or(0)),
        "note": "Not optimized. Classify+hot_decode_trigger diagnostic ran on the work thread after T_FLOWRA_RX."
    });

    let src_map = sources.lock().ok().map(|g| g.clone()).unwrap_or_default();
    let mut src_vec: Vec<(String, u64)> = src_map.into_iter().collect();
    src_vec.sort_by(|a, b| b.1.cmp(&a.1));
    src_vec.truncate(16);
    let src_json: serde_json::Value = src_vec
        .into_iter()
        .map(|(k, n)| serde_json::json!({"addr": k, "unique": n}))
        .collect();

    let pkts = st.g(&st.packets);
    let uniq = st.g(&st.unique);
    let dups = st.g(&st.dups);
    let dup_rate = if pkts > 0 {
        dups as f64 / pkts as f64
    } else {
        0.0
    };
    let ready = connected_ok && pkts > 0 && st.g(&st.valid) > 0 && last_err.is_empty();

    let report = serde_json::json!({
        "product": "Flowra Open Orderflow Auction ??? SearcherService.SubscribePendingTransactions",
        "docs": DOCS,
        "api_reference": "https://docs.flowra.wtf/searchers/api-reference.md",
        "orderflow": "https://docs.flowra.wtf/searchers/orderflow-stream.md",
        "endpoints_doc": "https://docs.flowra.wtf/endpoints.md",
        "proto": PROTO_REPO,
        "proto_rpc": "searcher.SearcherService/SubscribePendingTransactions",
        "auth": "AuthService challenge-response; sign \"{pubkey}-{challenge}\" Ed25519; Bearer access JWT. Secrets not recorded.",
        "region": REGION,
        "endpoint": endpoint,
        "transport": "gRPC/TLS :443",
        "filter": "accounts=[] full firehose (docs: empty or \"*\" = full stream)",
        "runtime_s": up,
        "messages": st.g(&st.messages),
        "packets": pkts,
        "unique_tx": uniq,
        "duplicates": dups,
        "duplicate_rate": dup_rate,
        "tx_per_s": if up > 0 { pkts as f64 / up as f64 } else { 0.0 },
        "mbps": if up > 0 { (st.g(&st.bytes) as f64 * 8.0) / (up as f64 * 1_000_000.0) } else { 0.0 },
        "valid": st.g(&st.valid),
        "bad": st.g(&st.bad),
        "vote": st.g(&st.vote),
        "non_vote": st.g(&st.nonvote),
        "legacy": st.g(&st.legacy),
        "v0": st.g(&st.v0),
        "v1": st.g(&st.v1),
        "venues": {
            "dlmm": st.g(&st.dlmm),
            "pump": st.g(&st.pump),
            "damm": st.g(&st.damm),
            "clmm": st.g(&st.clmm),
            "cpmm": st.g(&st.cpmm),
            "orca": st.g(&st.orca)
        },
        "supported_trigger_decoded": st.g(&st.trigger),
        "capture_drop": st.g(&st.capture_drop),
        "work_drop": st.g(&st.work_drop),
        "reconnects": st.g(&st.reconnects),
        "last_err": last_err,
        "connected_leaders": leaders_note,
        "sources_top": src_json,
        "landing": landing,
        "adapter_latency": adapter,
        "capture": {
            "flw": paths.flw.to_string_lossy(),
            "idx": paths.idx.to_string_lossy(),
            "recs": st.g(&st.rec_n),
            "bytes": st.g(&st.rec_bytes)
        },
        "tsc_hz": tsc_hz,
        "ready_early_feed": ready,
        "orbitflare_join": "not run; no simultaneous premium shred feed tonight. Join later by sig via .idx: lead_us = T_ORBITFLARE_FIRST_ACTIONABLE - T_FLOWRA_RX"
    });

    let _ = std::fs::write(&paths.meta, serde_json::to_string_pretty(&report).unwrap_or_default());
    println!("{}", serde_json::to_string_pretty(&report).unwrap_or_default());
}
