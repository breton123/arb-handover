# Hot-path snapshot

Copied from the Frankfurt box (`louis@195.242.152.178`, home directory) on 2026-10-07. The sources there are
not under git; this snapshot is the starting point for this repository. If the originals keep changing, diff
against them before porting changes in either direction.

| Here | Source on the box | Notes |
|---|---|---|
| `the-firm/` | `~/the-firm` | `run/` (15 GB of captures and run outputs) and `tmp_lut/` excluded |
| `arb-exec/` | `~/arb-exec` | `.deploy/` (wallet, program keypair, SWQoS env) and `.venv/` excluded |
| `arb-exec-live/` | `~/arb-exec-live` | |
| `arb-core/` | `~/arb-core` | `tmp_*` scratch excluded |
| `arb-feed/` | `~/arb-feed` | `scripts/orbitflare.env` and `tmp_po/` excluded |
| `arb-cap/` | `~/arb-cap` | journals, `*.bin`, `keys.json`, `tomorrow/_probe_keys.py` excluded |

Excluded everywhere: `build/`, `target/`, `.git/`, `__pycache__/`, `*.env`, `*.bin`, `*.cap`, `*.pcap`, `journal*`.

## Fixtures and captures you will need

These stay on the box (too large or immutable):

- OrbitFlare capture used by TXFRAME-001: `/home/louis/fixtures/orbitflare-20260927-002844.cap`
- The-firm run outputs: `~/the-firm/run/`
- Earlier paper / live trial journals: `~/paper_live001/`, `~/arb-cap/*/journal*`
- OrbitFlare live env (credentials): `/data/bsc/captures/orbitflare.env` — ask the owner for access; do not copy it
  into the repository.

## Credentials that were deliberately left out

- `arb-exec/.deploy/wallet.json`, `arb-exec/.deploy/hops-program.json` (keypairs)
- `arb-exec/.deploy/swqos.env`, `arb-feed/scripts/orbitflare.env`
- `arb-cap/exec_live002b/keys.json`, `arb-cap/tomorrow/_probe_keys.py`

Every script that needs one of these reads it from the environment or a path; supply your own copy locally.
