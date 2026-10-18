"""Check product invariants: no player window, no gameplay input, no host quit."""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
source="\n".join(p.read_text() for p in (root/"src").glob("*.*"))
for pattern in [r"\bLiSend[A-Z]\w*\s*\(",r"SDL_CreateWindow",r"QWidget",r"QWindow",r"QDesktopServices",r"\.quitApp\s*\("]:
    assert not re.search(pattern,source),f"Forbidden behavior found: {pattern}"
tls_source=source+"\n"+"\n".join(p.read_text() for p in (root/"third_party/moonlight-6.2").glob("*.*"))
assert "QSsl" not in tls_source, "Receiver must not require OBS Qt TLS plugins"
input_code=(root/"third_party/moonlight-common-c/src/InputStream.c").read_text()
guard = re.search(r"#ifdef OBS_RECEIVE_ONLY(.*?)#else", input_code, re.S)
assert guard, "Receive-only guard is missing"
assert "initialized = false;" in guard.group(1)
assert "initialized = true;" not in guard.group(1)
assert not re.search(r"\bsendEnableHaptics\s*\(", guard.group(1)), "Receive-only branch must not send haptics"
assert "OBS_RECEIVE_ONLY=1" in (root/"CMakeLists.txt").read_text()
assert "s.hostAudio,0,false,url" in source, "Controller bitmap must be zero"
assert "f900dd4767759c7b9d0e93bcea666b55c69ea62f" in (root/"docs/UPSTREAM.md").read_text()
print("Receive-only, no-window, no-host-quit and Moonlight 6.2 pin checks passed")
