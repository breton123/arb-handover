#!/usr/bin/env python3
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live
import record_dlmm as d

SOL = live.SOL
USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
HT = "HTvjzsfX3yU6BUodCjZ5vZkUrAxMDTrBs3CJaq43ashR"
GP = "Gf7sXMoP8iRw4iiXmJ1nq4vxcRycbGXy5RL8a8LnTd3v"


def side(mx, my):
    x, y = live.b58e(mx), live.b58e(my)
    return f"X={x[:8]}{'=SOL' if x==SOL else '=USDC' if x==USDC else ''} Y={y[:8]}{'=SOL' if y==SOL else '=USDC' if y==USDC else ''}"


def main():
    live.load_dotenv()
    accs = d.get_multiple([HT, GP])
    lb = d.parse_lbpair(accs[0]["data"])
    p = live.parse_pump_pool(accs[1]["data"])
    print("DLMM", side(lb["token_x"], lb["token_y"]))
    print("PUMP", side(p["base"], p["quote"]))
    ok = (
        live.b58e(lb["token_y"]) == SOL
        and live.b58e(p["quote"]) == SOL
        and live.b58e(lb["token_x"]) != SOL
        and live.b58e(p["base"]) != SOL
        and lb["token_x"] == p["base"]
    )
    print("route0_compatible", ok)


if __name__ == "__main__":
    main()
