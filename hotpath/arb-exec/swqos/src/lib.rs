//! SWQOS QUIC last hop. Persistent READY pool, control-plane reconnect.
//! https://swqos.com/docs/send-quic

use ed25519_dalek::SigningKey;
use quinn::{Connection, Endpoint};
use rustls::client::ResolvesClientCert;
use rustls::crypto::ring::sign::any_supported_type;
use rustls::pki_types::{CertificateDer, PrivateKeyDer};
use rustls::sign::CertifiedKey;
use rustls::SignatureScheme;
use std::ffi::CStr;
use std::net::SocketAddr;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;
use tokio::runtime::Runtime;

const HOST: &str = "send.swqos.com";
const PORT: u16 = 11000;
const ALPN: &[u8] = b"ultrasend/1";
const TX_MAX: usize = 1232;
const POOL: usize = 4;
const RECEIPT_CAP: usize = 4096;

const ST_DEAD: u8 = 0;
const ST_READY: u8 = 1;
const ST_CONNECTING: u8 = 2;

const PKCS8_PREFIX: &[u8] = &[
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
];

struct Slot {
    state: AtomicU8,
    conn: Mutex<Option<Connection>>,
}

struct Pool {
    slots: [Slot; POOL],
    endpoint: Endpoint,
    dest: SocketAddr,
    stop: AtomicBool,
}

static RT: OnceLock<Runtime> = OnceLock::new();
static POOL_G: Mutex<Option<Arc<Pool>>> = Mutex::new(None);
static RR: AtomicUsize = AtomicUsize::new(0);
static LAST_CONN: AtomicUsize = AtomicUsize::new(0);

#[derive(Debug)]
struct StaticClientCert(Arc<CertifiedKey>);

impl ResolvesClientCert for StaticClientCert {
    fn resolve(
        &self,
        _acceptable_issuers: &[&[u8]],
        _sigschemes: &[SignatureScheme],
    ) -> Option<Arc<CertifiedKey>> {
        Some(self.0.clone())
    }

    fn has_certs(&self) -> bool {
        true
    }
}

fn dummy_cert(seed: &[u8; 32]) -> Result<(CertificateDer<'static>, PrivateKeyDer<'static>), i32> {
    let _ = SigningKey::from_bytes(seed);
    let mut pkcs8 = Vec::with_capacity(48);
    pkcs8.extend_from_slice(PKCS8_PREFIX);
    pkcs8.extend_from_slice(seed);
    let kp = rcgen::KeyPair::from_pkcs8_der_and_sign_algo(
        &rustls::pki_types::PrivatePkcs8KeyDer::from(pkcs8.clone()),
        &rcgen::PKCS_ED25519,
    )
    .map_err(|e| {
        eprintln!("swqos keypair {e}");
        -3i32
    })?;
    let mut params = rcgen::CertificateParams::new(vec!["localhost".into()]).map_err(|_| -3i32)?;
    params.distinguished_name = rcgen::DistinguishedName::new();
    params
        .distinguished_name
        .push(rcgen::DnType::CommonName, "Solana node");
    let cert = params.self_signed(&kp).map_err(|e| {
        eprintln!("swqos selfsign {e}");
        -3i32
    })?;
    Ok((
        CertificateDer::from(cert.der().as_ref().to_vec()),
        PrivateKeyDer::try_from(pkcs8).map_err(|_| -3i32)?,
    ))
}

fn decode_key(raw: &str) -> Result<[u8; 32], i32> {
    let s = raw.trim().trim_end_matches(['\r', '\n']);
    let payload = s
        .strip_prefix("usq_live_")
        .or_else(|| s.strip_prefix("usq_test_"))
        .unwrap_or(s);
    let bytes = bs58::decode(payload).into_vec().map_err(|_| -1i32)?;
    match bytes.len() {
        32 => {
            let mut seed = [0u8; 32];
            seed.copy_from_slice(&bytes);
            Ok(seed)
        }
        64 => {
            let mut seed = [0u8; 32];
            seed.copy_from_slice(&bytes[..32]);
            Ok(seed)
        }
        _ => Err(-1),
    }
}

fn https_account(key: &str) -> Result<(), i32> {
    let url = format!("https://{HOST}/v1/account");
    let resp = match ureq::get(&url)
        .set("Authorization", &format!("Bearer {key}"))
        .timeout(Duration::from_secs(8))
        .call()
    {
        Ok(r) => r,
        Err(ureq::Error::Status(code, r)) => {
            let body = r.into_string().unwrap_or_default();
            let code_s = body
                .split("\"code\"")
                .nth(1)
                .and_then(|s| s.split('"').nth(1))
                .unwrap_or("");
            eprintln!("swqos https account status={code} err={code_s}");
            return Err(-3);
        }
        Err(_) => {
            eprintln!("swqos https account transport failed");
            return Err(-3);
        }
    };
    let body = resp.into_string().map_err(|_| -3i32)?;
    let v: serde_json::Value = serde_json::from_str(&body).map_err(|_| -3i32)?;
    let enabled = v.get("enabled").and_then(|x| x.as_bool()).unwrap_or(false);
    let bal = v.get("balance_lamports").and_then(|x| x.as_u64()).unwrap_or(0);
    eprintln!("swqos account enabled={enabled} balance_lamports={bal}");
    if !enabled {
        return Err(-3);
    }
    Ok(())
}

fn client_config(
    cert: CertificateDer<'static>,
    key: PrivateKeyDer<'static>,
) -> Result<quinn::ClientConfig, i32> {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let mut roots = rustls::RootCertStore::empty();
    roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    let signing = any_supported_type(&key).map_err(|e| {
        eprintln!("swqos sign key {e}");
        -3i32
    })?;
    let ck = CertifiedKey::new(vec![cert], signing);
    let mut crypto = rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_no_client_auth();
    crypto.client_auth_cert_resolver = Arc::new(StaticClientCert(Arc::new(ck)));
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    crypto.enable_early_data = true;
    let mut transport = quinn::TransportConfig::default();
    transport.max_idle_timeout(Some(Duration::from_secs(360).try_into().map_err(|_| -3i32)?));
    transport.keep_alive_interval(Some(Duration::from_secs(10)));
    transport.max_concurrent_bidi_streams(256u32.into());
    let quic = match quinn::crypto::rustls::QuicClientConfig::try_from(crypto) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("swqos quic crypto: {e}");
            return Err(-3);
        }
    };
    let mut cfg = quinn::ClientConfig::new(std::sync::Arc::new(quic));
    cfg.transport_config(std::sync::Arc::new(transport));
    Ok(cfg)
}

fn runtime() -> &'static Runtime {
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .thread_name("swqos")
            .build()
            .expect("tokio")
    })
}

fn mark_dead(pool: &Pool, i: usize, why: &str) {
    let slot = &pool.slots[i];
    let prev = slot.state.swap(ST_DEAD, Ordering::AcqRel);
    if prev != ST_DEAD {
        let _ = slot.conn.lock().ok().map(|mut g| g.take());
        eprintln!("swqos slot {i} DEAD ({why})");
    }
}

fn install_ready(pool: &Arc<Pool>, i: usize, conn: Connection) {
    if let Ok(mut g) = pool.slots[i].conn.lock() {
        *g = Some(conn.clone());
    }
    pool.slots[i].state.store(ST_READY, Ordering::Release);
    eprintln!("swqos slot {i} READY");
    let weak = Arc::downgrade(pool);
    tokio::spawn(async move {
        conn.closed().await;
        if let Some(p) = weak.upgrade() {
            if !p.stop.load(Ordering::Acquire) {
                mark_dead(&p, i, "peer_closed");
            }
        }
    });
}

fn pick_ready(pool: &Pool, skip: Option<usize>) -> Option<(usize, Connection)> {
    let n = POOL;
    let start = RR.fetch_add(1, Ordering::Relaxed) % n;
    for k in 0..n {
        let i = (start + k) % n;
        if skip == Some(i) {
            continue;
        }
        if pool.slots[i].state.load(Ordering::Acquire) != ST_READY {
            continue;
        }
        let g = pool.slots[i].conn.lock().ok()?;
        let Some(c) = g.as_ref() else {
            continue;
        };
        if c.close_reason().is_some() {
            drop(g);
            mark_dead(pool, i, "close_reason");
            continue;
        }
        return Some((i, c.clone()));
    }
    None
}

async fn write_tx(conn: &Connection, tx: &[u8]) -> Result<(), &'static str> {
    if conn.close_reason().is_some() {
        return Err("closed");
    }
    let (mut send, mut recv) = conn.open_bi().await.map_err(|_| "open_bi")?;
    send.write_all(tx).await.map_err(|_| "write")?;
    send.finish().map_err(|_| "finish")?;
    tokio::spawn(async move {
        let _ = recv.read_to_end(RECEIPT_CAP).await;
    });
    Ok(())
}

async fn connect_one(endpoint: &Endpoint, dest: SocketAddr) -> Result<Connection, i32> {
    let connecting = endpoint.connect(dest, HOST).map_err(|_| -7i32)?;
    connecting.await.map_err(|_| -7i32)
}

async fn boot(cfg: quinn::ClientConfig) -> Result<Arc<Pool>, i32> {
    let mut addrs = tokio::net::lookup_host((HOST, PORT))
        .await
        .map_err(|_| -7i32)?
        .collect::<Vec<SocketAddr>>();
    if addrs.is_empty() {
        return Err(-7);
    }
    addrs.sort_by_key(SocketAddr::is_ipv6);
    let dest = addrs[0];
    let bind = if dest.is_ipv6() {
        "[::]:0".parse().unwrap()
    } else {
        "0.0.0.0:0".parse().unwrap()
    };
    let mut endpoint = Endpoint::client(bind).map_err(|_| -7i32)?;
    endpoint.set_default_client_config(cfg);
    let slots: [Slot; POOL] = std::array::from_fn(|_| Slot {
        state: AtomicU8::new(ST_DEAD),
        conn: Mutex::new(None),
    });
    let pool = Arc::new(Pool {
        slots,
        endpoint,
        dest,
        stop: AtomicBool::new(false),
    });
    for i in 0..POOL {
        pool.slots[i].state.store(ST_CONNECTING, Ordering::Release);
        match connect_one(&pool.endpoint, dest).await {
            Ok(c) => install_ready(&pool, i, c),
            Err(_) => {
                pool.slots[i].state.store(ST_DEAD, Ordering::Release);
                eprintln!("swqos slot {i} boot fail");
            }
        }
    }
    let ready = (0..POOL)
        .filter(|i| pool.slots[*i].state.load(Ordering::Acquire) == ST_READY)
        .count();
    if ready == 0 {
        return Err(-7);
    }
    eprintln!("swqos quic up  dest={dest} ready={ready}/{POOL} alpn=ultrasend/1");
    Ok(pool)
}

async fn control(pool: Arc<Pool>) {
    while !pool.stop.load(Ordering::Acquire) {
        for i in 0..POOL {
            if pool.stop.load(Ordering::Acquire) {
                break;
            }
            let st = pool.slots[i].state.load(Ordering::Acquire);
            if st == ST_READY {
                let dead = pool
                    .slots[i]
                    .conn
                    .lock()
                    .ok()
                    .and_then(|g| g.as_ref().and_then(|c| c.close_reason().map(|_| ())));
                if dead.is_some() {
                    mark_dead(&pool, i, "ctrl_close");
                }
                continue;
            }
            if st != ST_DEAD {
                continue;
            }
            if pool.slots[i]
                .state
                .compare_exchange(ST_DEAD, ST_CONNECTING, Ordering::AcqRel, Ordering::Acquire)
                .is_err()
            {
                continue;
            }
            match connect_one(&pool.endpoint, pool.dest).await {
                Ok(c) => install_ready(&pool, i, c),
                Err(_) => {
                    pool.slots[i].state.store(ST_DEAD, Ordering::Release);
                    eprintln!("swqos slot {i} reconnect fail");
                }
            }
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

#[no_mangle]
pub extern "C" fn swqos_open(api_key: *const c_char) -> i32 {
    if api_key.is_null() {
        return -1;
    }
    let key = unsafe { CStr::from_ptr(api_key) }
        .to_str()
        .unwrap_or("")
        .trim()
        .trim_end_matches(['\r', '\n'])
        .to_string();
    if key.is_empty() {
        return -1;
    }
    let _ = rustls::crypto::ring::default_provider().install_default();
    if https_account(&key).is_err() {
        return -3;
    }
    let seed = match decode_key(&key) {
        Ok(s) => s,
        Err(e) => return e,
    };
    let (cert, pk) = match dummy_cert(&seed) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let rt = runtime();
    let cfg = match client_config(cert, pk) {
        Ok(c) => c,
        Err(e) => return e,
    };
    let pool = match rt.block_on(boot(cfg)) {
        Ok(p) => p,
        Err(_) => {
            eprintln!("swqos quic connect failed");
            return -7;
        }
    };
    let ctrl = pool.clone();
    rt.spawn(control(ctrl));
    let mut g = POOL_G.lock().expect("pool");
    *g = Some(pool);
    0
}

#[no_mangle]
pub extern "C" fn swqos_open_env() -> i32 {
    let key = std::env::var("SWQOS_KEY")
        .or_else(|_| std::env::var("SWQOS_API_KEY"))
        .unwrap_or_default();
    if key.is_empty() {
        return -1;
    }
    match std::ffi::CString::new(key) {
        Ok(s) => swqos_open(s.as_ptr()),
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn swqos_send(tx: *const u8, len: u16) -> i32 {
    if tx.is_null() || len == 0 || (len as usize) > TX_MAX {
        return -1;
    }
    let pool = {
        let g = POOL_G.lock().expect("pool");
        match g.as_ref() {
            Some(p) => p.clone(),
            None => return -2,
        }
    };
    let mut copy = [0u8; TX_MAX];
    unsafe {
        std::ptr::copy_nonoverlapping(tx, copy.as_mut_ptr(), len as usize);
    }
    let bytes = &copy[..len as usize];
    let rt = runtime();

    let Some((i, a)) = pick_ready(&pool, None) else {
        return -2;
    };
    LAST_CONN.store(i, Ordering::Relaxed);
    match rt.block_on(write_tx(&a, bytes)) {
        Ok(()) => return 0,
        Err(why) => {
            mark_dead(&pool, i, why);
        }
    }

    let Some((j, b)) = pick_ready(&pool, Some(i)) else {
        return -4;
    };
    LAST_CONN.store(j, Ordering::Relaxed);
    match rt.block_on(write_tx(&b, bytes)) {
        Ok(()) => 0,
        Err(why) => {
            mark_dead(&pool, j, why);
            -4
        }
    }
}

#[no_mangle]
pub extern "C" fn swqos_last_conn() -> i32 {
    LAST_CONN.load(Ordering::Relaxed) as i32
}

#[no_mangle]
pub extern "C" fn swqos_ready_n() -> i32 {
    let g = POOL_G.lock().expect("pool");
    let Some(p) = g.as_ref() else {
        return 0;
    };
    (0..POOL)
        .filter(|i| p.slots[*i].state.load(Ordering::Acquire) == ST_READY)
        .count() as i32
}

#[no_mangle]
pub extern "C" fn swqos_close() {
    let mut g = POOL_G.lock().expect("pool");
    if let Some(p) = g.take() {
        p.stop.store(true, Ordering::Release);
        for i in 0..POOL {
            mark_dead(&p, i, "close");
        }
    }
}
