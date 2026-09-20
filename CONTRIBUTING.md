# Contributing

Fork freely and send focused pull requests. Include the hardware/phone versions,
reproduction steps, expected behavior, and offline or physical-test evidence.
Clearly separate compilation, simulated results and real-device measurements.

For recording, NAND, buffering, power and BLE changes, first compare the current
data flow with understood factory behavior and applicable upstream components.
Record source anchors or original code addresses. Do not paste relocatable-looking
machine-code fragments into a new firmware or assume stock behavior is optimal.

Preserve authenticated updates, encryption, integrity checks, explicit deletion
and recovery access. Do not silently retry an uncertain write/flash operation.
Use only one process to own a physical pendant at a time. No microphone recording
or destructive test should start as a side effect of building or opening the app.

Do not commit personal recordings, transcripts, phone/probe serials, pairing
databases, firmware signing keys, Android keystores, recovery secrets or whole
device dumps. Use synthetic test fixtures. Disclose meaningful AI assistance and
review the resulting code. Respect upstream contribution policies independently.

By contributing, you agree to license your original contributions under this
project's Apache-2.0 license; retain all existing third-party notices.
