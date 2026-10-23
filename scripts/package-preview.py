"""Stage preview notices and archive on the native builder to preserve Mac modes."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

platform, stage_arg = sys.argv[1:]
stage = Path(stage_arg)
notices = stage / 'licenses'
notices.mkdir(parents=True, exist_ok=True)
for source, name in [
    ('LICENSE', 'GameStream-Receiver-GPL.txt'),
    ('third_party/moonlight-common-c/enet/LICENSE', 'ENet.txt'),
    ('third_party/moonlight-common-c/nanors/LICENSE', 'nanors.txt'),
]:
    shutil.copy2(source, notices / name)
for source in Path('.').glob('build*/_deps/receiver_opus-src/COPYING'):
    shutil.copy2(source, notices / 'Opus.txt')
for source in Path('.deps').glob('openssl-universal/OPENSSL-LICENSE.txt'):
    shutil.copy2(source, notices / 'OpenSSL.txt')
vcpkg_root = os.environ.get('VCPKG_INSTALLATION_ROOT')
if vcpkg_root:
    source = Path(vcpkg_root) / 'installed/x64-windows-static-md/share/openssl/copyright'
    if source.is_file():
        shutil.copy2(source, notices / 'OpenSSL.txt')
# Retain notices supplied with the OBS media SDK, including FFmpeg dependencies.
for sdk in Path('.deps').glob('obs-deps-*'):
    if not sdk.is_dir():
        continue
    for source in sdk.rglob('*'):
        if source.is_file() and (source.name.lower().startswith(('license', 'copying', 'copyright')) or 'licenses' in source.parts):
            destination = notices / 'obs-sdk' / source.relative_to(sdk)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
shutil.copy2('README.md', stage / 'README.md')
shutil.copy2('docs/UPSTREAM.md', stage / 'UPSTREAM.md')
archive = Path(f'obs-gamestream-receiver-{platform}.zip').resolve()
if platform == 'macos-universal':
    subprocess.run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', str(stage / 'obs-gamestream-receiver.plugin')], check=True)
    subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', str(stage), str(archive)], check=True)
else:
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
        for source in sorted(stage.rglob('*')):
            if source.is_file():
                output.write(source, source.relative_to(stage))
with zipfile.ZipFile(archive) as output:
    if output.testzip() is not None:
        raise RuntimeError('Invalid preview archive')
if platform == 'macos-universal':
    # Exercise the exact download archive, not just the pre-archive directory.
    with tempfile.TemporaryDirectory(prefix='receiver-signature-check-') as directory:
        subprocess.run(['ditto', '-x', '-k', str(archive), directory], check=True)
        plugin = Path(directory) / 'obs-gamestream-receiver.plugin'
        subprocess.run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', str(plugin)], check=True)
        executable = plugin / 'Contents/MacOS/obs-gamestream-receiver'
        helper = plugin / 'Contents/MacOS/obs-gamestream-worker'
        for binary in (executable, helper):
            subprocess.run(['lipo', str(binary), '-verify_arch', 'arm64', 'x86_64'], check=True)
            if not os.access(binary, os.X_OK):
                raise RuntimeError(f'Archive lost executable permissions: {binary.name}')
        subprocess.run([str(helper), '--self-test'], check=True)
        if not os.access(executable, os.X_OK):
            raise RuntimeError('Archive lost executable permissions')
    print('Extracted Mac ZIP passed strict signature, universal architecture and executable permission checks')
print(archive)
