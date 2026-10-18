# Changelog

## Hardware video color repair

Pass decoded NV12/P010 frames directly to OBS with their original plane pitches instead of running a redundant same-format swscale conversion. The full-range NV12 conversion could leave chroma zeroed, causing green video. Regression tests check chroma values across padded rows for SDR/HDR and full/limited range.

## Self-contained HTTPS

Use bundled OpenSSL for client identities, exact host certificate pins and HTTPS, with ordinary Qt TCP/DNS sockets. The receiver no longer needs OBS to provide Qt TLS plugins. Adds real mutual TLS, wrong-pin rejection, framing, timeout and cancellation integration tests with Qt plugin discovery disabled.

## Mac packaging repair

Write the runtime inventory before signing, sign nested libraries before sealing the plugin, and verify the signature both before archiving and after extracting the final download ZIP. Repairs the invalid sealed resource in preview.10. Builds remain ad-hoc signed and are not Apple-notarized.

## 0.1.0 preview

Initial receive-only OBS source, Moonlight 6.2 transport, stream controls, FFmpeg/Opus decoding and Windows/Mac build workflows. Native and live-host testing pending.
