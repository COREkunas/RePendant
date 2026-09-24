# Security and privacy

This experimental firmware is not independently security-audited. Do not rely on
it as your sole copy of important recordings. Keep recovery backups offline and
private. Deletion/quick-clear reclaims logical recording space; it is not a claim
of forensic erasure of every NAND cell or OS copy.

The current app source has Internet permission for the optional, explicitly
configured MindyLink/Forge integration. It has **no phone-microphone permission**:
audio comes from the pendant. Android's Bluetooth/location permissions vary by
Android version. Core recording, Bluetooth sync and playback need no account or
cloud service. The older published 0.6.49 APK has no Internet permission.

PC transcription sends a selected, complete phone recording to the selected
MindyLink PC. Automatic upload is **off by default** and requires explicit opt-in.
Chat sends the prompt, completed conversation history and selected transcripts
to the chosen model host. That host/provider's processing and retention policies
also matter; remote processing is not on-phone inference. Recording/recovery keys
are not sent to the PC. Account/session data, local chat history and transcripts
use the app's private encrypted storage. This is implementation intent and tested
local behavior, not an independent security audit or end-to-end service guarantee.

The optional on-phone whisper.cpp model is imported separately, with no hidden
cloud fallback. MindyLink/Forge is a separate opt-in path, not a silent replacement
for local transcription. Phone/pendant deletion does not delete separate PC jobs;
use the explicit PC-job removal controls for those copies. See the
[integration boundaries](docs/SEPTEMBER_2026_UPDATES.md).

The development APK is debuggable. The firmware development configuration keeps
SWD access available. Physical access can therefore defeat assumptions applicable
to a hardened consumer product. Do not publish signing or recording private keys.

Please report a vulnerability privately through GitHub's private vulnerability
reporting **if enabled**, or ask the maintainer for a private channel without
posting secrets in a public issue. Do not attach full flash/NVS/NAND dumps.
