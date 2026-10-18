# Changelog

## Mac packaging repair

Write the runtime inventory before signing, sign nested libraries before sealing the plugin, and verify the signature both before archiving and after extracting the final download ZIP. Repairs the invalid sealed resource in preview.10. Builds remain ad-hoc signed and are not Apple-notarized.

## 0.1.0 preview

Initial receive-only OBS source, Moonlight 6.2 transport, stream controls, FFmpeg/Opus decoding and Windows/Mac build workflows. Native and live-host testing pending.
