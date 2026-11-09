#!/usr/bin/env python3
"""Pre-fetch the build dependencies listed in buildspec.json into .deps/ with retries.

GitHub release downloads intermittently return 502/504 (8-9 Nov 2026). The template's
CMake downloads each archive once and fails the build on any error, but skips archives
that already exist in .deps/. This script downloads them first, with retries and
exponential back-off, and verifies each SHA-256 against buildspec.json, so CMake finds
complete archives and never downloads. File names follow cmake/<platform>/buildspec.cmake.

On Windows, configuring the OBS sources also builds a 32-bit slice that needs the x86
obs-deps archive; its version and SHA-256 come from the OBS sources' own buildspec.json
(read from the source archive), and cmake/common/buildspec_common.cmake copies every
fetched *-deps-* archive into the OBS sources' .deps before configuring them.

Usage: fetch_deps.py windows-x64 | macos
"""

import hashlib
import json
import os
import sys
import time
import urllib.request
import zipfile

NAMES = {
    "windows-x64": {
        "prebuilt": "windows-deps-{v}-x64{r}.zip",
        "qt6": "windows-deps-qt6-{v}-x64{r}.zip",
        "obs-studio": "{v}.zip",
    },
    "macos": {
        "prebuilt": "macos-deps-{v}-universal{r}.tar.xz",
        "qt6": "macos-deps-qt6-{v}-universal{r}.tar.xz",
        "obs-studio": "{v}.tar.gz",
    },
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, dest, expected, attempts=8):
    tmp = dest + ".part"
    delay = 10
    for i in range(1, attempts + 1):
        try:
            print(f"[{i}/{attempts}] {url}", flush=True)
            req = urllib.request.Request(url, headers={"User-Agent": "hdr-toolkit-ci"})
            with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as out:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            got = sha256(tmp)
            if got != expected:
                raise RuntimeError(f"SHA-256 mismatch: {got}")
            os.replace(tmp, dest)
            print(f"  ok ({os.path.getsize(dest) // (1 << 20)} MiB)", flush=True)
            return
        except Exception as e:  # network errors, HTTP 5xx, truncated or wrong files
            print(f"  failed: {e}", flush=True)
            if os.path.exists(tmp):
                os.remove(tmp)
            if i == attempts:
                raise
            time.sleep(delay)
            delay = min(delay * 2, 120)


def main():
    platform = sys.argv[1]
    spec = json.load(open("buildspec.json"))
    os.makedirs(".deps", exist_ok=True)
    for dep, tmpl in NAMES[platform].items():
        d = spec["dependencies"][dep]
        version = d["version"]
        rev = d.get("revision", {}).get(platform) if isinstance(d.get("revision"), dict) else None
        r = ""
        if rev:
            r = ("_v" if "{r}.tar" in tmpl and dep == "prebuilt" and platform == "macos" else "-v") + str(rev)
        name = tmpl.format(v=version, r=r)
        dest = os.path.join(".deps", name)
        expected = d["hashes"][platform]
        if os.path.exists(dest) and sha256(dest) == expected:
            print(f"{name}: cached", flush=True)
            continue
        url = f"{d['baseUrl']}/{name}" if dep == "obs-studio" else f"{d['baseUrl']}/{version}/{name}"
        fetch(url, dest, expected)
    if platform == "windows-x64":
        fetch_obs_x86(spec)


def fetch_obs_x86(spec):
    """The x86 obs-deps archive the OBS sources' configure downloads for its 32-bit slice."""
    obs_ver = spec["dependencies"]["obs-studio"]["version"]
    with zipfile.ZipFile(os.path.join(".deps", f"{obs_ver}.zip")) as z:
        obs_spec = json.loads(z.read(f"obs-studio-{obs_ver}/buildspec.json"))
    d = obs_spec["dependencies"]["prebuilt"]
    expected = d.get("hashes", {}).get("windows-x86")
    if not expected:
        print("OBS sources need no x86 obs-deps archive", flush=True)
        return
    name = f"windows-deps-{d['version']}-x86.zip"
    dest = os.path.join(".deps", name)
    if os.path.exists(dest) and sha256(dest) == expected:
        print(f"{name}: cached", flush=True)
        return
    fetch(f"{d['baseUrl']}/{d['version']}/{name}", dest, expected)


if __name__ == "__main__":
    main()
