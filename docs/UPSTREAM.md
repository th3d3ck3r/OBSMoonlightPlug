# Moonlight 6.2 provenance

- Moonlight PC tag: **v6.2.0**.
- Moonlight PC commit: `de2467e433821664cdd2224aad8c89a625be1ad9`.
- Streaming core pinned by that release: `f900dd4767759c7b9d0e93bcea666b55c69ea62f`.
- ENet submodule: `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1`.
- nanors submodule: `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a`.

Vendored files in `third_party/moonlight-6.2` are the release's NvHTTP,
NvPairingManager, IdentityManager, NvAddress and NvApp sources. The streaming core
and its ENet/nanors dependencies are included under `third_party/moonlight-common-c`.
No checkout of moving `master` is used by the receiver.

Adaptations:

- Removed desktop computer-model constructors and cover-art/image methods.
- Isolated client credentials and host certificate pins under this OBS module's
  configuration directory, with owner-only file permissions where supported.
- Added cancellable network requests and a bounded pairing wait.
- Preserved binary hash bytes when deriving the pairing AES key and validated
  complete challenge-response lengths.
- Range-check SDP integers before narrowing to fixed-width protocol fields.
- Suppressed URL logging that could expose session-key query parameters.
- Built the core with `OBS_RECEIVE_ONLY`: input APIs remain uninitialized and
  the haptics enable packet is suppressed. No controller is advertised at launch.

The OBS decoder uses FFmpeg and feeds OBS directly. On macOS it selects
VideoToolbox where available; Windows uses D3D11VA where available. The Moonlight
desktop Metal/libplacebo renderer, display switching, Game Mode and input/window
features are deliberately not instantiated: OBS owns rendering and recording.

This preserves a Moonlight 6.2 transport base while adapting relevant decoding,
full-range color, 10-bit PQ and HDR luminance metadata to the OBS source pipeline.
It is not a wholesale embedding of the Moonlight application's renderer.

Upstream licensing is preserved in the vendored directories. The combined plugin
source is distributed under GPL-3.0-or-later.

When the SDK has no standalone Opus library, CMake builds Opus 1.5.2 at commit
`ddbe48383984d56acd9e1ab6a090c54ca6b735a6` statically. Universal Mac builds use
its portable C decoder to avoid applying one CPU architecture’s intrinsics to both slices.
