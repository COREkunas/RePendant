# USB phone setup and recording-key migration

Firmware 0.4.61 / Android 0.6.53 added a direct-phone USB setup path. The wired
identity and fresh pairing information are checked before Bluetooth setup; there
is no fixed passkey or Just Works fallback. Five quick idle button taps still
replace the old Bluetooth bond and open the blue-blinking 60-second window.

Bluetooth pairing and recording encryption are separate. Pairing a new phone does
not by itself give it the key needed to use an existing recording volume.

## Move a pendant to another phone

Settings → **Move pendant / change recording key** offers:

- **Keep recordings:** import the matching old recovery backup locally on the new
  phone, verify the wired pendant identity, then finish the new phone binding.
- **New key / delete pendant recordings:** explicitly erase pendant recordings and
  activate a new verified recipient key. Phone copies, recovery backups and the
  Bluetooth bond remain. An old key backup cannot recover erased audio bytes.
- **Archive for later old-key recovery:** not implemented; remains disabled.

Private recovery material stays inside the phone and the user's own backup files.
The PC-wired engineering flow uses public enrollment fields only. Never upload a
private key or backup to obtain help.

## Reset transaction and qualification

Firmware 0.4.62 persists a checksummed parent/child configuration journal before
explicit erasure, then activates the successor only after storage validation.
Preparation, erasure, restart and verification are separate acknowledged stages.
There is no automatic erase on startup or blind retry after an uncertain result.
Interrupted erasure requires explicit reconciliation. Old firmware rejects the
new configuration schema instead of adopting the wrong recipient.

Android 0.6.55 saves and verifies an immutable public plan before preparation,
retains old phone bindings/copies, and publishes the successor only after checking
the active matching volume. Recording control gets a separate successor-volume
journal, avoiding old unresolved start/stop state.

One owner's physical reset completed through the separately PC-wired operator
path, with empty storage surviving restart, matching new-phone binding and a
read-only BLE recording-status/catalog check. No microphone recording was made
as part of that qualification. Direct-phone USB reset and archive migration must
not be described as end-to-end physically qualified. No erase was performed for
the metadata-only sync update or this source publication.

The preserved factory investigation found Dhara page flow at `0x1f26a` → `0x4db7c`
→ `0x73f28` and checkpoint flow at `0x1e968` → `0x4dfe4` → `0x73dd4`; it did not
establish a recoverable stock recipient-rotation transaction. Existing Dhara/NAND
coordination is reused rather than relocating factory executable fragments.
