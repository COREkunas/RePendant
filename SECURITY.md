# Security and privacy

This experimental firmware is not independently security-audited. Do not rely on
it as your sole copy of important recordings. Keep recovery backups offline and
private. Deletion/quick-clear reclaims logical recording space; it is not a claim
of forensic erasure of every NAND cell or OS copy.

The app has no Internet or phone-microphone permission. Audio comes from the
pendant. Android's Bluetooth/location permissions vary by Android version. The
transcription model is imported separately; there is no hidden cloud fallback.

The development APK is debuggable. The firmware development configuration keeps
SWD access available. Physical access can therefore defeat assumptions applicable
to a hardened consumer product. Do not publish signing or recording private keys.

Please report a vulnerability privately through GitHub's private vulnerability
reporting **if enabled**, or ask the maintainer for a private channel without
posting secrets in a public issue. Do not attach full flash/NVS/NAND dumps.
