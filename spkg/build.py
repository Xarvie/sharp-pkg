#!/usr/bin/env python3
"""
build.py — spkg build script (zig cc, no CMake dependency).

Uses the zig compiler bundled with sharpc to build spkg.
Lua scripts are embedded as C arrays at build time.

Usage:
    python3 build.py
"""

import subprocess
import sys
import os
import platform as _plat
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent

_SYS = sys.platform
_MACH = _plat.machine()
if _SYS == "win32":
    TARGET = "x86_64-windows-gnu"
elif _SYS == "darwin":
    TARGET = "aarch64-macos" if _MACH == "arm64" else "x86_64-macos"
else:
    TARGET = "x86_64-linux-gnu"

_SHARP_ROOT_HELP = """\
ERROR: SHARP_ROOT environment variable not set.

Set SHARP_ROOT to your sharp installation root.  Expected layout:

  sharpc/:
    bin  std  zig

  sharpc/bin:
    sharpc.exe  spkg.exe

  sharpc/std:
    cjson.sp  cjson.sph  hashmap.sph  str.sph  string.sph  types.sph  vec.sph

  sharpc/zig:
    LICENSE  README.md  doc  lib  zig.exe

Example:
  set SHARP_ROOT=C:\\path\\to\\env\\sharpc
"""

if "SHARP_ROOT" not in os.environ:
    sys.exit(_SHARP_ROOT_HELP)

_SHARP_ROOT = Path(os.environ["SHARP_ROOT"])

if _SYS == "win32":
    ZIG = str(_SHARP_ROOT / "zig" / "zig.exe")
    OBJ_EXT = ".o"
    EXE_EXT = ".exe"
    LINK_FLAGS = ["-lshlwapi", "-lws2_32", "-lwinhttp", "-lrpcrt4", "-lcrypt32", "-lsecur32", "-static"]
else:
    ZIG = str(_SHARP_ROOT / "zig" / "zig")
    OBJ_EXT = ".o"
    EXE_EXT = ""
    LINK_FLAGS = ["-lpthread", "-lm", "-ldl"]

BUILD_DIR = ROOT / "build"

LUA_SCRIPTS = [
    "scripts/spkg_init.lua",
    "scripts/spkg_build.lua",
    "scripts/spkg_resolve.lua",
    "scripts/spkg_lock.lua",
    "scripts/spkg_fetch.lua",
]

LUA_SOURCES = []
_lua_dir = ROOT / "lua"
for f in sorted(_lua_dir.glob("*.c")):
    if f.name in ("lua.c", "luac.c", "ltests.c", "onelua.c"):
        continue
    LUA_SOURCES.append(f"lua/{f.name}")

SPKG_SOURCES = [
    "main.c",
    "native.c",
    "mongoose.c",
]

CFLAGS = [
    "-std=c99",
    "-O2",
    "-Wall", "-Wextra", "-Wno-deprecated-declarations",
    "-D_GNU_SOURCE",
    "-DNDEBUG",
    "-target", TARGET,
    "-I", str(ROOT),
    "-I", str(ROOT / "lua"),
    "-I", str(BUILD_DIR),
]


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        if r.stderr:
            sys.stderr.write(r.stderr.decode() if isinstance(r.stderr, bytes) else r.stderr)
        if r.stdout:
            sys.stderr.write(r.stdout.decode() if isinstance(r.stdout, bytes) else r.stdout)
        sys.exit(r.returncode)
    return r


def embed_lua_script(script_rel):
    script_path = ROOT / script_rel
    varname = script_rel.replace("/", "_").replace("\\", "_").replace(".", "_")
    out_c = BUILD_DIR / (script_rel.replace("/", "_").replace("\\", "_") + ".c")

    if out_c.is_file() and out_c.stat().st_mtime > script_path.stat().st_mtime:
        return str(out_c)

    data = script_path.read_bytes()
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
    lines = []
    lines.append(f"unsigned char {varname}[] = {{")
    for i in range(0, len(hex_bytes), 78):
        lines.append("  " + hex_bytes[i:i+78])
    lines.append("};")
    lines.append(f"unsigned int {varname}_len = {len(data)};")
    lines.append("")

    out_c.parent.mkdir(parents=True, exist_ok=True)
    out_c.write_text("\n".join(lines))
    print(f"  EMBED {script_rel}")
    return str(out_c)


def compile_obj(src_rel, cflags_extra=None):
    src = ROOT / src_rel
    obj = BUILD_DIR / (src_rel.replace("/", "_").replace("\\", "_") + OBJ_EXT)
    obj.parent.mkdir(parents=True, exist_ok=True)

    if obj.is_file() and obj.stat().st_mtime > src.stat().st_mtime:
        return obj

    flags = CFLAGS.copy()
    if cflags_extra:
        flags.extend(cflags_extra)
    cmd = [ZIG, "cc", "-c", str(src), "-o", str(obj)] + flags
    print(f"  CC {src_rel}")
    run(cmd, capture_output=True, text=True)
    return obj


def link_exe(objs, out_rel, extra_flags=None):
    out = BUILD_DIR / out_rel
    flags = list(LINK_FLAGS)
    if extra_flags:
        flags.extend(extra_flags)
    cmd = [ZIG, "cc", "-target", TARGET] + [str(o) for o in objs] + ["-o", str(out)] + flags
    print(f"  LD {out_rel}")
    run(cmd, capture_output=True, text=True)


def deploy_to_env(exe_path):
    dst_dir = _SHARP_ROOT / "bin"
    if not dst_dir.is_dir():
        dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / exe_path.name
    shutil.copy2(str(exe_path), str(dst))
    print(f"  CP {exe_path.name} -> {dst}")


def deploy_lua_scripts():
    dst_dir = _SHARP_ROOT / "share" / "spkg"
    if not dst_dir.is_dir():
        dst_dir.mkdir(parents=True, exist_ok=True)
    for script_rel in LUA_SCRIPTS:
        src = ROOT / script_rel
        if not src.is_file():
            print(f"  SKIP {script_rel} (not found)")
            continue
        dst = dst_dir / src.name
        shutil.copy2(str(src), str(dst))
        print(f"  CP {src.name} -> {dst}")


def build_spkg():
    print("[build] spkg")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    embed_sources = []
    for script in LUA_SCRIPTS:
        embed_sources.append(embed_lua_script(script))

    objs = []
    for src in SPKG_SOURCES:
        objs.append(compile_obj(src))

    for src in LUA_SOURCES:
        objs.append(compile_obj(src))

    for src in embed_sources:
        objs.append(compile_obj(src))

    name = "spkg" + EXE_EXT
    link_exe(objs, name)
    exe = BUILD_DIR / name
    deploy_to_env(exe)
    deploy_lua_scripts()
    return exe


if __name__ == "__main__":
    build_spkg()
    print("[build] all done.")
