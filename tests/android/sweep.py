#!/usr/bin/env python3
"""translates f-droid apps in bulk and checks they start on whatever adb sees

sweep.py list <index-v2.json> [--libs]   apps w/ arm64 code and no x86_64 build, smallest first
sweep.py run <list.tsv> [--wait s] [--keep] [--jobs n]
results go to apks/sweep, one line per app in results.tsv:
package, kind, apk bytes, translate secs, status, screenshot colors, note, interpreter starts
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# a build of some other checkout works too, its runtime sources come along
TSUGIKI = ROOT / "build" / "tsugiki"
REPO = "https://f-droid.org/repo"
UA = "tsugiki-sweep"

# what a lib name gives away about the app
KINDS = [
    ("flutter", {"libflutter.so", "libapp.so"}),
    ("react-native", {"libreactnative.so", "libhermes.so", "libjsc.so", "libreactnativejni.so"}),
    ("godot", {"libgodot_android.so"}),
    ("unity", {"libunity.so", "libil2cpp.so"}),
    ("dotnet", {"libmonodroid.so", "libmonosgen-2.0.so", "libxamarin-app.so"}),
    ("qt", {"libQt5Core_arm64-v8a.so", "libQt6Core_arm64-v8a.so"}),
    ("sdl", {"libSDL2.so", "libSDL3.so"}),
    ("go", {"libgojni.so", "libgomobile.so"}),
    ("gdx", {"libgdx.so"}),
    ("tor", {"libtor.so"}),
]
JETPACK = {"libandroidx.graphics.path.so", "libdatastore_shared_counter.so", "libimage_processing_util_jni.so", "libsurface_util_jni.so"}


def kind(libs):
    names = set(libs)
    for k, known in KINDS:
        if names & known:
            return k
    return "jetpack" if names <= JETPACK else "other"


def get(url, first=None, last=None):
    headers = {"User-Agent": UA}
    if first is not None:
        headers["Range"] = f"bytes={first}-{'' if last is None else last}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as r:
        return r.read()


def arm64_libs(path, size):
    """lib/arm64-v8a names from the zip's central directory, w/o the whole apk"""
    url = REPO + path
    tail = get(url, max(0, size - 65536))
    eocd = tail.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack_from("<II", tail, eocd + 12)
    base = size - len(tail)
    cd = tail[cd_off - base:cd_off - base + cd_size] if cd_off >= base else get(url, cd_off, cd_off + cd_size - 1)
    libs, i = [], 0
    while cd[i:i + 4] == b"PK\x01\x02":
        n, e, c = struct.unpack_from("<HHH", cd, i + 28)
        name = cd[i + 46:i + 46 + n].decode("utf-8", "replace")
        if name.startswith("lib/arm64-v8a/") and name.endswith(".so"):
            libs.append(name.rsplit("/", 1)[1])
        i += 46 + n + e + c
    return libs


def list_apps(args):
    packages = json.load(open(args.index))["packages"]
    rows = []
    for pkg, p in packages.items():
        versions = list(p["versions"].values())
        if not versions:
            continue
        # the newest release, all its abi splits
        name = max(versions, key=lambda v: v["added"])["manifest"].get("versionName")
        release = [v for v in versions if v["manifest"].get("versionName") == name]
        abis = set().union(*(v["manifest"].get("nativecode") or [] for v in release))
        if "arm64-v8a" not in abis or "x86_64" in abis:
            continue
        arm = max((v for v in release if "arm64-v8a" in (v["manifest"].get("nativecode") or [])), key=lambda v: v["manifest"]["versionCode"])
        label = p["metadata"].get("name") or {}
        label = label.get("en-US") or next(iter(label.values()), pkg)
        rows.append([arm["file"]["size"], pkg, arm["file"]["name"], arm["file"]["sha256"], label])
    rows.sort()
    if args.libs:
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            for row, libs in zip(rows, pool.map(lambda r: arm64_libs(r[2], r[0]), rows)):
                row += [kind(libs), " ".join(libs)]
    for row in rows:
        print("\t".join(str(x) for x in row))


def sh(cmd, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def adb(*args, timeout=120):
    return sh(["adb", *args], timeout)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def download(path, digest):
    dest = ROOT / "apks" / path.lstrip("/")
    if dest.exists() and sha256(dest) == digest:
        return dest
    part = dest.with_suffix(".part")
    with urllib.request.urlopen(urllib.request.Request(REPO + path, headers={"User-Agent": UA}), timeout=300) as r, open(part, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if sha256(part) != digest:
        part.unlink()
        raise RuntimeError("sha256 mismatch")
    part.rename(dest)
    return dest


def translate(apk, out, jobs, log):
    # the cap keeps the emulator alive next to it
    cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=6G", "-p", "MemorySwapMax=0",
           str(TSUGIKI), "apk", "-j", str(jobs), str(apk), "-o", str(out)]
    r = sh(cmd, timeout=3600)
    log.write_text(r.stdout + r.stderr)
    return r.returncode == 0


def colors(png):
    r = sh(["magick", str(png), "-format", "%k", "info:"])
    return int(r.stdout.strip() or 0)


def check(pkg, wait, dir):
    adb("logcat", "-b", "all", "-c")
    adb("shell", "monkey", "-p", pkg, "-c", "android.intent.category.LAUNCHER", "1")
    alive = False
    for _ in range(wait // 2):
        time.sleep(2)
        alive = bool(adb("shell", "pidof", pkg).stdout.strip())
        if not alive and adb("logcat", "-d", "-b", "crash").stdout.strip():
            break
    png = dir / f"{pkg}.png"
    with open(png, "wb") as f:
        subprocess.run(["adb", "exec-out", "screencap", "-p"], stdout=f, timeout=60)
    log = adb("logcat", "-d", "-b", "main,system,crash", "-v", "brief").stdout
    (dir / f"{pkg}.logcat").write_text(log)
    ours = [l for l in log.splitlines() if "tsugiki" in l and (l.startswith("F/") or l.startswith("E/"))]
    crash = adb("logcat", "-d", "-b", "crash").stdout
    alive = bool(adb("shell", "pidof", pkg).stdout.strip())
    note = (ours[0].split(": ", 1)[-1] if ours else "") or next((l for l in crash.splitlines() if "signal" in l or "Exception" in l), "")
    # places the interpreter had to start from, what discovery missed
    interpreted = sum(1 for l in log.splitlines() if "tsugiki" in l and "interpreting " in l)
    return alive, colors(png), interpreted, note.strip()[:200]


def device_up():
    try:
        return "\tdevice" in adb("devices", timeout=20).stdout
    except subprocess.TimeoutExpired:
        return False


# the emulator dies now and then (some vulkan apps take it down), start it again
def revive(command):
    print("   no device, starting the emulator again", flush=True)
    subprocess.Popen(command, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    adb("wait-for-device", timeout=300)
    for _ in range(120):
        if adb("shell", "getprop", "sys.boot_completed").stdout.strip() == "1":
            return True
        time.sleep(2)
    return False


def run(args):
    dir = ROOT / "apks" / "sweep"
    dir.mkdir(parents=True, exist_ok=True)
    results = dir / "results.tsv"
    done = {l.split("\t")[0] for l in results.read_text().splitlines()} if results.exists() else set()
    for line in open(args.list):
        size, pkg, path, digest, label, *rest = line.rstrip("\n").split("\t")
        if pkg in done:
            continue
        k = rest[0] if rest else "?"
        if args.restart and not device_up() and not revive(args.restart):
            print("   the emulator won't come back, stopping", flush=True)
            return
        print(f"== {pkg} ({k}, {int(size) / 1e6:.1f} MB)", flush=True)
        out = dir / f"{pkg}.apk"
        secs, status, n, interpreted, note = "0", "error", "0", "", ""
        try:
            apk = download(path, digest)
            t = time.time()
            ok = translate(apk, out, args.jobs, dir / f"{pkg}.build")
            secs = f"{time.time() - t:.0f}"
            if not ok:
                tail = (dir / f"{pkg}.build").read_text().strip().splitlines()
                status, note = "translate", tail[-1] if tail else ""
            else:
                r = adb("install", "-r", "-g", str(out), timeout=900)
                if "Success" not in r.stdout:
                    status, note = "install", (r.stdout + r.stderr).strip().splitlines()[-1]
                else:
                    alive, count, starts, note = check(pkg, args.wait, dir)
                    status, n, interpreted = "runs" if alive else "dies", str(count), str(starts)
                    adb("uninstall", pkg, timeout=300)
        except Exception as e:
            note = str(e)
        if not args.keep:
            out.unlink(missing_ok=True)
            shutil.rmtree(out.with_suffix(".work"), ignore_errors=True)
        row = [pkg, k, size, secs, status, n, note.replace("\t", " ")[:200], interpreted]
        print("   " + "  ".join(row[3:]), flush=True)
        with open(results, "a") as f:
            f.write("\t".join(row) + "\n")


def main():
    os.environ["PATH"] = os.path.expanduser("~/Android/Sdk/platform-tools") + os.pathsep + os.environ["PATH"]
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="command", required=True)
    l = sub.add_parser("list")
    l.add_argument("index")
    l.add_argument("--libs", action="store_true")
    r = sub.add_parser("run")
    r.add_argument("list")
    r.add_argument("--wait", type=int, default=20)
    r.add_argument("--keep", action="store_true")
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--tsugiki", type=Path)
    r.add_argument("--restart", help="shell command that starts the emulator when adb loses it")
    args = p.parse_args()
    if getattr(args, "tsugiki", None):
        global TSUGIKI
        TSUGIKI = args.tsugiki.resolve()
    list_apps(args) if args.command == "list" else run(args)


if __name__ == "__main__":
    sys.exit(main())
