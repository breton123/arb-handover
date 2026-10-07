"""Shyft adapter: finalized getMultipleAccounts is the snapshot primitive.

context.slot on that response is the row anchor, and only because the
commitment is finalized (completed bank). Yellowstone GetSlot is an
optional live hint and must not be the state boundary.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request

from envload import grpc_host, grpc_token, rpc_url
from provider import CoherenceError


class ShyftProvider:
    def __init__(self) -> None:
        self._rpc = rpc_url()
        self._grpc_host = grpc_host()
        self._token = grpc_token()

    def name(self) -> str:
        return "shyft"

    def get_slot(self) -> int:
        """Optional current-bank hint. Not the CompactState anchor."""
        slot = self._grpc_slot()
        if slot is not None:
            return slot
        ctx, _ = self.get_accounts([])
        return ctx

    def _grpc_slot(self) -> int | None:
        if not self._token:
            return None
        try:
            import grpc
            import geyser_pb2
            import geyser_pb2_grpc
        except ImportError:
            return None
        host = self._grpc_host
        if not host:
            return None
        creds = grpc.ssl_channel_credentials()
        channel = grpc.secure_channel(host, creds)
        try:
            stub = geyser_pb2_grpc.GeyserStub(channel)
            req = geyser_pb2.GetSlotRequest()
            if hasattr(geyser_pb2, "PROCESSED"):
                req.commitment = geyser_pb2.PROCESSED
            resp = stub.GetSlot(req, metadata=(("x-token", self._token),))
            return int(resp.slot)
        except Exception:
            return None
        finally:
            channel.close()

    def get_accounts(self, keys: list[str]) -> tuple[int, dict[str, bytes]]:
        if not self._rpc:
            raise CoherenceError("missing RPC URL (SHYFT_KEY / RPC_URL)")
        if not keys:
            body = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "getSlot",
                "params": [{"commitment": "processed"}],
            }
            data = self._rpc_call(body)
            return int(data), {}
        body = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "getMultipleAccounts",
            "params": [keys, {"encoding": "base64", "commitment": "finalized"}],
        }
        result = self._rpc_call(body)
        ctx = int(result["context"]["slot"])
        out: dict[str, bytes] = {}
        for pk, row in zip(keys, result.get("value") or []):
            if not row or not row.get("data"):
                continue
            blob, enc = row["data"][0], row["data"][1]
            if enc != "base64":
                continue
            import base64

            out[pk] = base64.b64decode(blob)
        return ctx, out

    def _rpc_call(self, body: dict):
        req = urllib.request.Request(
            self._rpc,
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as ex:
            raise CoherenceError("rpc http") from ex
        if payload.get("error"):
            raise CoherenceError("rpc error")
        return payload["result"]
