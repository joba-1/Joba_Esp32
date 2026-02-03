#!/usr/bin/env python3
"""pre_build.py

PlatformIO pre-build script.

- Ensures config.ini exists (copied from config.ini.template).
- Generates an auto-generated filesystem build manifest (data/build_info.json)
    that is included in LittleFS so the running device can report which git
    revision the filesystem was built from.
- Injects git SHA + build timestamp into the firmware build as C preprocessor
    defines so firmware and filesystem versions can be compared at runtime.
"""
import os
import shutil
import sys
import subprocess
import json
from datetime import datetime, timezone


def _write_if_changed(path: str, content: str) -> None:
    try:
        existing = ""
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                existing = f.read()
        if existing == content:
            return
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    except Exception:
        # Best-effort only; do not fail the build for metadata generation.
        return


def _iso_to_utc_z(iso_str: str) -> str:
    try:
        # Git may emit offsets like +01:00; normalize to UTC Z.
        dt = datetime.fromisoformat(iso_str.replace("Z", "+00:00"))
        dt_utc = dt.astimezone(timezone.utc).replace(microsecond=0)
        return dt_utc.isoformat().replace("+00:00", "Z")
    except Exception:
        return ""

def _run_git(project_dir, args):
    try:
        out = subprocess.check_output(["git", "-C", project_dir, *args], stderr=subprocess.DEVNULL)
        return out.decode("utf-8", errors="replace").strip()
    except Exception:
        return ""


def _get_git_info(project_dir: str) -> dict:
    commit = _run_git(project_dir, ["rev-parse", "--short=12", "HEAD"]) or "unknown"
    describe = _run_git(project_dir, ["describe", "--always", "--dirty", "--tags"]) or commit
    dirty = bool(_run_git(project_dir, ["status", "--porcelain"]))
    return {"gitCommit": commit, "gitDescribe": describe, "dirty": dirty}


def _get_git_commit_time_utc(project_dir: str) -> str:
    # %cI is strict ISO 8601 with timezone offset.
    iso = _run_git(project_dir, ["show", "-s", "--format=%cI", "HEAD"]) or ""
    utc = _iso_to_utc_z(iso) if iso else ""
    return utc


def _get_git_commit_unix(project_dir: str) -> int:
    # %ct is committer date, UNIX timestamp (seconds)
    ts = _run_git(project_dir, ["show", "-s", "--format=%ct", "HEAD"]) or ""
    try:
        return int(ts)
    except Exception:
        return 0


def _hash_data_dir(data_dir: str, exclude_files: set[str]) -> str:
    """Compute a stable content hash of the data directory.

    This intentionally excludes build_info.json to avoid self-referential churn.
    """
    try:
        import hashlib

        h = hashlib.sha256()
        for root, _, files in os.walk(data_dir):
            for name in sorted(files):
                rel = os.path.relpath(os.path.join(root, name), data_dir)
                rel_posix = rel.replace(os.sep, "/")
                if rel_posix in exclude_files:
                    continue
                path = os.path.join(root, name)
                h.update(rel_posix.encode("utf-8", errors="replace"))
                h.update(b"\0")
                with open(path, "rb") as f:
                    while True:
                        chunk = f.read(8192)
                        if not chunk:
                            break
                        h.update(chunk)
                h.update(b"\0")
        return h.hexdigest()
    except Exception:
        return ""


# Try to get environment from PlatformIO
try:
    Import("env")
    project_dir = env.get("PROJECT_DIR")
except:
    # Running standalone
    project_dir = os.path.dirname(os.path.abspath(sys.argv[0])) if '__file__' not in dir() else os.path.dirname(os.path.abspath(__file__))

config_file = os.path.join(project_dir, "config.ini")
template_file = os.path.join(project_dir, "config.ini.template")

if not os.path.exists(config_file):
    if os.path.exists(template_file):
        print("Creating config.ini from template...")
        shutil.copy(template_file, config_file)
        print("✓ config.ini created. Please customize it for your environment.")
    else:
        print("ERROR: config.ini.template not found!")
        print("Cannot proceed without configuration template.")
        sys.exit(1)


# Generate filesystem build manifest (included in LittleFS)
data_dir = os.path.join(project_dir, "data")
build_info_path = os.path.join(data_dir, "build_info.json")

git_info = _get_git_info(project_dir)
now_utc = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

fs_hash = ""
if os.path.isdir(data_dir):
    fs_hash = _hash_data_dir(data_dir, exclude_files={"build_info.json"})

build_info = {
    **git_info,
    "builtAtUtc": now_utc,
    "fsContentSha256": fs_hash,
}

try:
    if os.path.isdir(data_dir):
        os.makedirs(data_dir, exist_ok=True)

        # Avoid rewriting build_info.json unless something relevant changed.
        # This keeps `pio run` and `pio run -t buildfs` incremental.
        existing = None
        if os.path.exists(build_info_path):
            try:
                with open(build_info_path, "r", encoding="utf-8", errors="replace") as f:
                    existing = json.load(f)
            except Exception:
                existing = None

        if isinstance(existing, dict):
            same = (
                existing.get("gitCommit") == build_info.get("gitCommit")
                and existing.get("gitDescribe") == build_info.get("gitDescribe")
                and bool(existing.get("dirty")) == bool(build_info.get("dirty"))
                and existing.get("fsContentSha256") == build_info.get("fsContentSha256")
            )
            if same:
                # Preserve the previous builtAtUtc for stable filesystems.
                build_info["builtAtUtc"] = existing.get("builtAtUtc", build_info["builtAtUtc"])
            else:
                # Content changed: keep new timestamp.
                pass

        # Only write when content differs
        content = json.dumps(build_info, indent=2, sort_keys=True) + "\n"
        _write_if_changed(build_info_path, content)
except Exception as e:
    print(f"WARN: Failed to write build_info.json: {e}")


# Inject firmware build identifiers as preprocessor defines.
# IMPORTANT: keep these stable across builds (no per-build timestamp churn),
# otherwise PlatformIO/SCons will recompile every source file every time.
try:
    # Only available when running under PlatformIO/SCons
    fw_git = git_info.get("gitCommit", "unknown")
    fw_build_unix = _get_git_commit_unix(project_dir)
    env.Append(CPPDEFINES=[
        ("FIRMWARE_GIT_SHA", fw_git),
        ("FIRMWARE_BUILD_UNIX", fw_build_unix),
    ])
except Exception:
    pass

