# Original Limitless application 1.1.20 — recovered research image

**This is original vendor firmware, not RePendant code and not a complete
stock-restoration package.** It does not receive an Apache-2.0 license from this
project. Vendor copyright and applicable rights remain unchanged.

The owner's preserved 1 MiB application-flash read has SHA-256:

`9cc79cefe4a372e5d4ca267f3ad29fffd12c1a0fe740295a239392a272603b04`

The release deliberately contains only the bounded signed application image:

- Source interval: **0x14000 through 0x9411E inclusive**.
- Header: 512 bytes; executable payload: 523,912 bytes; TLVs: 151 bytes.
- Original file length: **524,575 bytes** (header + payload + TLVs).
- Bounded-image SHA-256:
  `e03aa55de923560835e3a6d0868ae6ea669e8061ebb421623038fa4bc9f88023`.
- The embedded SHA-256 matches the exact header/payload. This checks integrity,
  **not an independent verification of the vendor's ECDSA identity**.

It excludes the factory bootloader, all flash after this signed-image boundary,
UICR, external NAND, recordings, and the missing original network-core image.
The full raw backup stays private. Static firmware may contain vendor constants;
it is not a data-free guarantee for arbitrary other dumps.

Use it to inspect hardware setup and understand original behavior. It cannot
establish full rollback, and its original link address differs from RePendant's
application slot. **Do not upload it to the custom bootloader or flash it using
the custom firmware's address map.** No factory private signing key is included.
