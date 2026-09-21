# Sync recording details only

In **Recordings → Transfer**, expand the card and enable **Sync recording details
only**. The main action becomes **Sync details**. Turn it off to download recordings
again. This choice is saved and also applies to foreground automatic sync and
low-battery sync. Existing installations retain full recording sync by default.

Details-only sync refreshes the authenticated recording catalog and manifests:
available recording identity, duration and copy status. It does not download audio,
send download receipts, process pending deletions or enable remove-after-sync.
It does not invent missing recording dates. Dashboard storage-capacity telemetry
is separate; a catalog refresh is not a new capacity measurement.

Pending deletion requests remain saved. **Finish pending deletions** explicitly
completes those requests without downloading audio, after a separate confirmation.
Full recording sync still completes earlier deletion requests. Removal after
successful audio transfer retains its own saved preference, visibly inactive in
details-only mode.

## Implementation and boundaries

- Reuses the authenticated `INVENTORY_ONLY` paginated catalog/manifest session.
  Ordinary details sync does not create a Settings quick-clear review.
- An independent transport wrapper rejects payload reads, single/batched receipts
  and deletions before they reach Bluetooth.
- Existing encrypted phone copies may be hashed for integrity, but are not
  decrypted or played. No microphone operation is part of this flow.
- Successful bounded batches may continue; failed or uncertain inventory requests
  are not automatically retried. Stop preserves progress and deletion intents.
- Missing catalog entries alone are never treated as proof of deletion.
- Populated/empty and paginated catalogs, pending receipts/deletions, cancellation,
  identity rejection and switching back to full audio sync have offline tests.

Factory reference was checked before implementation: preserved app SHA-256
`9cc79cefe4a372e5d4ca267f3ad29fffd12c1a0fe740295a239392a272603b04`, batch flag
check at `0x23346`, branch at `0x2334a`, page preparation call at `0x2337a` to
`0x229c4`. Recovered stock commands separate device info/status (14/21) from flash
downloads (8). No equivalent stock per-record catalog was established. Our existing
metadata protocol is reused; no stock binary fragments are distributed. Avoiding
audio payload is the intended difference, not a claimed Bluetooth speed increase.

See [current qualification status](STATUS.md) for build and physical-test results.
