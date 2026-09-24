# September source update: Android 0.6.62 / firmware 0.4.63

This page covers source changes since Android 0.6.56 / firmware 0.4.62. Existing
GitHub binary releases remain Android 0.6.49 / firmware 0.4.57; installing those
older downloads does not install the features below.

## Recording downloads and status

- In Recordings, expand an eligible pendant recording and choose **Download this
  recording** or **Resume this download**. Confirm to fetch only that recording.
- Global details-only sync stays selected. The pendant copy is kept; unrelated
  pending deletions and remove-after-sync are not applied by this operation.
- The selected volume, recording and manifest must still match the fresh catalog.
  Incomplete, changed, suppressed or stale selections are refused. Existing
  integrity checks and resumable checkpoints remain in use.
- A compact header stays above the app's tabs with connection, recording state,
  battery and enabled storage allocation. Offline readings are muted and dated;
  they are last-known values, not a live claim. Tap indicators for full details.
- Bluetooth sync and selected-PC export use a foreground notification, bounded
  wake lock and operation-specific stop control. An interrupted job keeps saved
  progress; Android can still restrict/stop background work.

The old short-test-recording list was removed from the main navigation without
deleting its files. This does not add on-phone transcription for long recordings.

## Recording and charging lights

With firmware 0.4.63, **Settings → Lights & battery** offers schema-2 settings:

- **Steady** recording light remains the default.
- **Hidden** gives three pulses after recording starts, stays dark during capture,
  and gives two pulses after successful stop/save. Fault and pairing indications
  remain visible; a save failure is not reported as success.
- Charging color is separate from recording color and defaults to **Off**. It
  follows a fresh, trusted battery-gauge charging reading, not USB presence alone.

Old settings are upgraded in memory without an automatic persistent rewrite.
Explicit save still uses acknowledgement and readback. No bootloader, radio,
recording format, NAND layout or recording-key format changed in this update.

LED/current behavior was compared with the preserved factory application before
implementation; no factory executable fragments were inserted into this build.
USB settings save/read/reconnect/restore passed on the test pendant. Exact pulse
counts have synthetic coverage; physical microphone pulse timing and unplugged
charging-current polarity still need qualification.

## Optional MindyLink / Forge

The core recorder needs neither a MindyLink account nor a cloud service. The
optional integration adds selected-PC transcription and transcript-based model
chat. It requires a compatible HTTPS MindyLink gateway/account, an online Forge
PC with the pendant job API, and a configured transcription/model backend.
Those separate services and the desktop project are **not included** here.

1. Configure/sign in under **Settings → MindyLink**, then select your PC and
   transcription language. Leave automatic transcription off unless you want it.
2. Expand a complete phone recording and explicitly send it to the selected PC.
   Partial/gapped copies are rejected; the app does not download pendant audio
   merely because you requested remote transcription.
3. Retrieve the verified transcript. Audio and transcript deletion on the PC use
   separate controls from deleting phone/pendant copies.
4. In **AI chat**, select the host/model and the recording transcripts to include.
   Conversations, transcript context and recording aliases are saved locally in
   encrypted account-scoped files. Renaming a recording does not rewrite pendant
   storage or historical chat citations.

Automatic upload is **off by default**. After opt-in it applies to new complete
phone copies while the app is open; interrupted uploads require explicit resume.
Chat sends selected transcript context and completed conversation history. There
are no remote tools in this client; stopping a phone wait does not guarantee that
the remote model stopped computing. Remote processing is not on-phone inference.

Local protocol/UI tests passed during development, but authenticated gateway →
PC → model end-to-end behavior is **not yet qualified**. This is an experimental
integration, not a deployed-service or throughput guarantee. See
[security/privacy](../SECURITY.md) and the
[separate MindyLink licensing boundary](../THIRD_PARTY_NOTICES.md#mindylink-integration-exception).

## Verification and remaining limits

See [public-export checks](PUBLICATION_CHECKS.md) for the freshly rerun Android
and native tests. Selected-record downloads have synthetic coverage but no new
physical audio-transfer test for this snapshot. Existing battery-only cold-start,
first-install, long-recording local-transcription and storage-capacity limits in
the [status matrix](STATUS.md) remain. Publication itself did not access hardware,
record audio, install updates, delete data or publish new binary releases.
