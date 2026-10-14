# Validation — 2026-10-14

Available environment: Linux x86_64, GCC 13, Qt6, FFmpeg 6.1, libobs and Opus.

- Full plugin module compiled and linked successfully.
- ctest: 2/2 passed (media-and-settings; receive-only-audit).
- Actual H.264 fixture decodes to NV12 with monotonic timestamps.
- Actual zero-reorder HEVC Main10 fixture decodes to P010, PQ transfer tag and 1000-nit metadata.
- Actual Opus encoder/decoder roundtrip produces 480 stereo PCM samples at 48 kHz.
- Invalid settings, malformed packet sizes and repeated cleanup are exercised.
- Static audit checks input-call absence, no standalone window APIs, zero controller mask and the core receive-only build guard.

Not yet validated: Windows/Mac compilation and bundle installation, OBS source loading and Properties interaction, native hardware decoding, TLS pairing, live Apollo/Sunshine connection, reconnect and long recording A/V synchronization. Included workflows are build candidates, not evidence of successful native builds.
