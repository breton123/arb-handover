# Tomorrow — funding is the only blocker

SSH: `ssh -i C:\Users\louis\.ssh\id_ed25519 louis@195.242.152.178`

Wallet: `HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX`  
Need ≥ **0.15 SOL**. Leftover today ~0.015.  
OUR_EXEC: `38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K`  
Never `6xfcHyCs…` (dead BPF2). Never send the arb.

## Deploy (after the transfer confirms)

```bash
source ~/.arb-smoke.env
export PATH="$HOME/.local/share/solana/install/active_release/bin:$PATH"
cd ~
python3 arb-exec/scripts/go_deploy.py preflight   # expect READY once funded
python3 arb-exec/scripts/go_deploy.py all
```

That is:

```
fund wallet
  → loader-v3 deploy OUR_EXEC
  → verify program (owner BPFLoaderUpgradeab1e…, executable)
  → simulate 619 B v0 route0
  → walk failures
  → reach Custom(6)
  → record real CU  →  arb-cap/exec_live002b/WALK.json + DEPLOY.md
```

Step-by-step if you want to watch each gate:

```bash
python3 arb-exec/scripts/go_deploy.py preflight
python3 arb-exec/scripts/go_deploy.py deploy
python3 arb-exec/scripts/go_deploy.py verify
python3 arb-exec/scripts/go_deploy.py simulate
python3 arb-exec/scripts/go_deploy.py walk
```

## OrbitFlare (paper capture)

Tell the vendor:

```
dst 195.242.152.178:20001  UDP  shreds
```

On the box (sudo password for the firewall line):

```bash
sudo ufw allow 20001/udp comment orbitflare-shreds
bash /home/louis/arb-feed/scripts/orbitflare_prep.sh
bash /home/louis/arb-feed/scripts/orbitflare_paper.sh
```

Disk alarm (other terminal or cron):

```bash
bash /home/louis/arb-feed/scripts/orbitflare_watch.sh
# crontab: * * * * * /home/louis/arb-feed/scripts/orbitflare_watch.sh
```

Config is `arb-feed/scripts/orbitflare.env`:

| | |
|--|--|
| public IP | `195.242.152.178` |
| UDP port | `20001` |
| bind | `0.0.0.0` |
| capture | `/home/louis/captures/orbitflare` (not the FEEDCAP1 trial files) |
| CPU | 47 / rec 46 |
| rcvbuf | 16 MiB (`rmem_max` already 256 MiB) |
| min-gb / alarm | 40 |
| paper | capture only — no `sendTransaction`, no `leader_send` |

SIGINT flushes. Leave it alone after it says `LIVE capture`.
