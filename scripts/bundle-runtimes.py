"""Bundle media runtime libraries; use the installed OBS Qt/libobs runtime.

Run on the native Windows/macOS builder after staging the plugin.
"""
import argparse
from collections import deque
from pathlib import Path
import shutil
import subprocess
import sys
import re


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("plugin",type=Path)
    parser.add_argument("sdk",type=Path)
    args=parser.parse_args()
    mac=sys.platform=="darwin"
    if not mac and sys.platform!="win32":
        raise SystemExit("Run runtime bundling on the Windows/macOS builder")
    plugin=args.plugin.resolve()
    if mac:
        executable=plugin/"Contents/MacOS/obs-gamestream-receiver"
        target=plugin/"Contents/Frameworks"
        files=list(args.sdk.rglob("*.dylib"))
    else:
        executable=plugin
        target=plugin.parent
        files=list(args.sdk.rglob("*.dll"))
    target.mkdir(parents=True,exist_ok=True)
    lookup={}
    for file in files:
        lookup.setdefault(file.name.lower(),file)
    queue=deque([executable]);visited=set();bundled=[]
    while queue:
        binary=queue.popleft()
        if binary in visited:continue
        visited.add(binary)
        if mac:
            output=subprocess.check_output(["otool","-L",str(binary)],text=True)
            dependencies=[line.strip().split(" (",1)[0] for line in output.splitlines()[1:]]
        else:
            output=subprocess.check_output(["dumpbin","/DEPENDENTS",str(binary)],text=True)
            dependencies=re.findall(r"(?im)^\s+([\w.+-]+\.dll)\s*$",output)
        for dependency in dependencies:
            name=Path(dependency).name;lower=name.lower()
            # These are provided by OBS or the operating system. Do not bundle a second Qt runtime.
            if lower.startswith(("qt6","qtcore","qtnetwork","obs","libobs","api-ms-","ext-ms-","vcruntime","msvcp")) or ".framework/" in dependency or dependency.startswith(("/System/","/usr/lib/")):
                continue
            source=lookup.get(lower)
            if source is None:
                if mac and dependency.startswith(("@rpath/","@loader_path/")):
                    raise RuntimeError("Cannot resolve media dependency: "+dependency)
                if not mac and lower.startswith(("avcodec","avutil","swscale","libopus","libaom","libdav1d","libvpx","libx264","libx265","svt")):
                    raise RuntimeError("Cannot resolve media dependency: "+dependency)
                continue
            destination=target/name
            if not destination.exists():
                shutil.copy2(source.resolve(),destination);bundled.append(name);queue.append(destination)
            if mac:
                reference="@loader_path/../Frameworks/"+name if binary==executable else "@loader_path/"+name
                subprocess.run(["install_name_tool","-change",dependency,reference,str(binary)],check=True)
                subprocess.run(["install_name_tool","-id","@loader_path/"+name,str(destination)],check=True)
    if mac:
        for binary in visited:
            subprocess.run(["lipo","-verify_arch","arm64","x86_64",str(binary)],check=True)
        subprocess.run(["codesign","--force","--deep","--sign","-",str(plugin)],check=True)
    (target/"receiver-media-runtime-files.txt").write_text("\n".join(bundled)+"\n")
    print("Bundled media runtime libraries:",", ".join(bundled) or "none needed")


if __name__=="__main__":main()
