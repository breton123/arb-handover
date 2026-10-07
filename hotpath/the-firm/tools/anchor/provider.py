"""Cold-start Pump snapshot provider. Not on the trading hot path."""
from __future__ import annotations

from typing import Protocol


class CoherenceError(RuntimeError):
    pass


class AnchorProvider(Protocol):
    def name(self) -> str: ...

    def get_slot(self) -> int: ...

    def get_accounts(self, keys: list[str]) -> tuple[int, dict[str, bytes]]:
        """One completed (finalized) bank. Returns (context.slot, pubkey->data).
        That slot is the only valid CompactState anchor for this map."""
        ...
