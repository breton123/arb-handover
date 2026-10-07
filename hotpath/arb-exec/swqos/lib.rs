//! SWQOS QUIC last hop. Persistent connection, one stream per tx.
//! https://swqos.com/docs/send-quic

use ed25519_dalek::SigningKey;
use quinn::Connection;
use rustls::pki_types::{CertificateDer, PrivateKeyDer};
use std::ffi::CStr;
use std::net::SocketAddr;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;
use tokio::runtime::Runtime;
use tokio::sync::{mpsc, oneshot};

const HOST: &str = "send.swqos.com";
const PORT: u16 = 11000;
const ALPN: &[u8] = b"ultrasend/1";
const TX_MAX: usize = 1232;
const POOL: usize = 2;
const RECEIPT_CAP: usize = 4096;

const PKCS8_PREFIX: &[u8] = &[
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
];


struct Job {
    bytes: [u8; TX_MAX],
    len: u16,
    reply: oneshot::Sender<i32>,
}

struct State {
    jobs: mpsc::Sender<Job>,
}

static RT: OnceLock<Runtime> = OnceLock::new();
static STATE: Mutex<Option<State>> = Mutex::new(None);
static RR: AtomicUsize = AtomicUsize::new(0);

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
    let s = raw.trim();
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
    let mut crypto = match rustls::ClientConfig::builder()
        .with_root_certificates(roots)
        .with_client_auth_cert(vec![cert], key)
    {
        Ok(c) => c,
        Err(e) => {
            eprintln!("swqos client cert: {e}");
            return Err(-3);
        }
    };
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    crypto.enable_early_data = true;
    let mut transport = quinn::TransportConfig::default();
    transport.max_idle_timeout(Some(Duration::from_secs(240).try_into().map_err(|_| -3i32)?));
    transport.keep_alive_interval(Some(Duration::from_secs(20)));
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

async fn connect_pool(
    cfg: quinn::ClientConfig,
) -> Result<(quinn::Endpoint, Vec<Connection>), i32> {
    let mut addrs = tokio::net::lookup_host((HOST, PORT))
        .await
        .map_err(|e| {
            eprintln!("swqos dns {e}");
            -7i32
        })?
        .collect::<Vec<SocketAddr>>();
    if addrs.is_empty() {
        eprintln!("swqos dns empty");
        return Err(-7);
    }
    addrs.sort_by_key(SocketAddr::is_ipv6);
    let dest = addrs[0];
    let bind = if dest.is_ipv6() {
        "[::]:0".parse().unwrap()
    } else {
        "0.0.0.0:0".parse().unwrap()
    };
    let mut endpoint = quinn::Endpoint::client(bind).map_err(|e| {
        eprintln!("swqos bind {e}");
        -7i32
    })?;
    endpoint.set_default_client_config(cfg);
    let mut conns = Vec::with_capacity(POOL);
    for i in 0..POOL {
        let connecting = endpoint.connect(dest, HOST).map_err(|e| {
            eprintln!("swqos connect[{i}] {e}");
            -7i32
        })?;
        let conn = connecting.await.map_err(|e| {
            eprintln!("swqos handshake[{i}] {e}");
            -7i32
        })?;
        conns.push(conn);
    }
    eprintln!(
        "swqos quic up  dest={dest} pool={POOL} alpn=ultrasend/1"
    );
    Ok((endpoint, conns))
}

async fn write_tx(conn: &Connection, tx: &[u8]) -> Result<(), ()> {
    let (mut send, mut recv) = conn.open_bi().await.map_err(|_| ())?;
    send.write_all(tx).await.map_err(|_| ())?;
    send.finish().map_err(|_| ())?;
    tokio::spawn(async move {
        let _ = recv.read_to_end(RECEIPT_CAP).await;
    });
    Ok(())
}

async fn worker(conns: Vec<Connection>, mut rx: mpsc::Receiver<Job>) {
    while let Some(job) = rx.recv().await {
        let n = conns.len();
        let start = RR.fetch_add(1, Ordering::Relaxed) % n;
        let mut rc = -4i32;
        for k in 0..n {
            let conn = &conns[(start + k) % n];
            if write_tx(conn, &job.bytes[..job.len as usize])
                .await
                .is_ok()
            {
                rc = 0;
                break;
            }
        }
        let _ = job.reply.send(rc);
    }
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
    let opened = rt.block_on(connect_pool(cfg));
    let (endpoint, conns) = match opened {
        Ok(v) => v,
        Err(_) => {
            eprintln!("swqos quic connect failed");
            return -7;
        }
    };
    let (tx, rx) = mpsc::channel::<Job>(64);
    rt.spawn(async move {
        let _keep = endpoint;
        worker(conns, rx).await;
    });
    let mut g = STATE.lock().expect("state");
    *g = Some(State { jobs: tx });
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
    let c = std::ffi::CString::new(key).map_err(|_| -1i32);
    match c {
        Ok(s) => swqos_open(s.as_ptr()),
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn swqos_send(tx: *const u8, len: u16) -> i32 {
    if tx.is_null() || len == 0 || (len as usize) > TX_MAX {
        return -1;
    }
    let jobs = {
        let g = STATE.lock().expect("state");
        match g.as_ref() {
            Some(s) => s.jobs.clone(),
            None => return -2,
        }
    };
    let mut bytes = [0u8; TX_MAX];
    unsafe {
        std::ptr::copy_nonoverlapping(tx, bytes.as_mut_ptr(), len as usize);
    }
    let (reply, wait) = oneshot::channel();
    if jobs
        .try_send(Job {
            bytes,
            len,
            reply,
        })
        .is_err()
    {
        return -6;
    }
    match wait.blocking_recv() {
        Ok(rc) => rc,
        Err(_) => -4,
    }
}

#[no_mangle]
pub extern "C" fn swqos_close() {
    let mut g = STATE.lock().expect("state");
    *g = None;
}
