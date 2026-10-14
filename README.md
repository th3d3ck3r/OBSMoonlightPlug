# GameStream Receiver for OBS — 0.1.0 preview

Receive Apollo/Sunshine video and audio directly as an OBS source. All controls stay inside OBS; no separate Moonlight playback window is created. This is an experimental source package, not an install-ready release.

## Current status

The full module compiles and links on Linux. Headless tests pass for H.264, HEVC Main10/PQ output and luminance metadata, Opus stereo, timestamps, invalid settings, malformed packets and receive-only guards. Windows x64 and macOS universal (Intel + Apple Silicon) build workflows are included but have not run. Loading in native OBS, hardware decode, pairing and live host recording remain unverified.

The transport and pairing code are pinned to Moonlight v6.2.0. OBS owns rendering. Moonlight's standalone Metal/libplacebo renderer and desktop-specific Mac features are not embedded. See docs/UPSTREAM.md for exact revisions and adaptations.

## Features

- Host pairing with a four-digit PIN; app selection or automatic Desktop lookup.
- Resolution presets and custom even dimensions up to 7680×4320; 1–240 FPS; 0.5–300 Mbps.
- Auto, H.264, HEVC or AV1 negotiation; automatic, required hardware or software decode.
- D3D11VA on Windows and VideoToolbox on macOS where available. Hardware frames are copied to system memory for OBS; this is not a zero-copy pipeline.
- SDR NV12 and 10-bit HDR P010/PQ with luminance metadata; HDR requires host/decoder support and HDR OBS recording configuration.
- Stereo, 5.1 or 7.1 audio, host audio playback toggle, buffer and audio offset controls.
- LAN/remote network selection, automatic reconnect and an optional disconnect-when-hidden setting.
- Connect/disconnect controls and status in Source Properties. One active stream per OBS process.

## Build Windows and Mac packages

This project lives at [th3d3ck3r/OBSMoonlightPlug](https://github.com/th3d3ck3r/OBSMoonlightPlug). In [Actions](https://github.com/th3d3ck3r/OBSMoonlightPlug/actions), run **Receiver native builds**, or push a commit to start a build. Successful jobs upload Windows x64 and Mac universal artifacts. No GitHub secrets or Apple signing account are required for these development builds. Mac bundles are ad-hoc signed, not notarized. Build failures must be resolved before treating the artifacts as usable releases.

Windows uses Visual Studio 2022 and static OpenSSL through vcpkg. Mac builds OpenSSL 3.5.8 for both architectures. The OBS template fetches the dependency versions pinned in buildspec.json (OBS 31.1.1 and OBS dependency set 2025-07-11). Media libraries are bundled recursively by scripts/bundle-runtimes.py; Qt and libobs come from installed OBS. Native compatibility with a specific OBS installation must be checked.

## Install a successful native build

Close OBS first and keep every bundled runtime library alongside the plugin.

**Windows:** Copy the artifact's obs-gamestream-receiver directory into `%ProgramData%\obs-studio\plugins\`. It should contain `bin\64bit\obs-gamestream-receiver.dll` and its data directory. Relaunch OBS. Do not install the Linux .so on Windows.

**Mac:** Copy `obs-gamestream-receiver.plugin` to `~/Library/Application Support/obs-studio/plugins/`, creating the folder if necessary. Relaunch OBS. Downloaded development bundles may require approval through macOS Privacy & Security. This project does not supply a notarized installer yet.

If the source is missing, check OBS Help → Log Files → View Current Log for GameStream Receiver errors. Remove the installed plugin folder to uninstall.

## First connection

1. On the source PC, start Apollo or Sunshine and make sure its Desktop/app entry and video/audio capture work. Keep both computers on your LAN for the first test.
2. In OBS on the recording computer, add **GameStream Receiver (Moonlight 6.2)**.
3. Enter the host address (without https:// or a path). Leave HTTP port at 47989 unless you changed the host configuration.
4. Set a four-digit pairing PIN and click **Pair with host**. Enter the same PIN on the host's pairing page when prompted. The plugin does not open another window. Pairing times out after 60 seconds.
5. Refresh host apps, choose Desktop or an app, and start with Balanced 1080p60, SDR, stereo and automatic decoding. Click **Connect**.
6. Confirm video in the OBS canvas and audio in its mixer, then make a short recording. OBS recording settings control the output file separately from these stream settings.
7. Click **Disconnect** to end reception. The plugin does not quit the host app. An app selected for streaming may be launched on the host; this is protocol session control.

Do not open another Moonlight receiver for the same host session during this first test. A host already running a different app is rejected rather than automatically quitting that app.

## Receive-only behavior

No keyboard, mouse or controller is advertised or sent. The bundled streaming core is compiled with OBS_RECEIVE_ONLY: input APIs remain uninitialized and its haptics-enable startup message is suppressed. Pairing, launch/resume, transport acknowledgments and stream management still use the host protocol. This is not a passive network sniffer.

## Linux development checks

Install CMake ≥3.28, Ninja, a C++17 compiler, OBS headers, Qt6 Core/Network, FFmpeg avcodec/avutil/swscale, Opus, OpenSSL and SIMDe headers. Then run:

```sh
cmake -S . -B build-check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-check --parallel 4
ctest --test-dir build-check --output-on-failure
```

Native workflow tests are currently disabled; the Linux test job checks actual compressed-media decoding. After native builds, test both Mac architectures, Windows, pairing/cancellation, hide/show behavior, reconnect, HDR and audio synchronization against a live host. Hardware acceleration and long recordings need device-level validation.

## License

GPL-3.0-or-later. Upstream licenses are included with the vendored source. Build artifacts distributed to others must retain licenses and corresponding source obligations.
