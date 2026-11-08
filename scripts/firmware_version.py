# PlatformIO pre-build script: bake the firmware version into the build as
# PLANE_RADAR_VERSION (a string literal).
#
# Source, in order:
#   1. PLANE_RADAR_VERSION environment variable (CI sets it from the tag)
#   2. `git describe --tags --always --dirty` (e.g. v1.2.0-3-gabc1234)
#   3. "dev"

Import("env")

import os
import subprocess


def detect_version():
    forced = os.environ.get("PLANE_RADAR_VERSION", "").strip()
    if forced:
        return forced
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        )
        described = out.decode("utf-8", "replace").strip()
        if described:
            return described
    except (OSError, subprocess.CalledProcessError):
        pass
    return "dev"


version = detect_version()
# Keep it safe inside a C string literal.
version = "".join(c for c in version if c.isalnum() or c in "._+-")[:40] or "dev"

print(f"Plane Radar firmware version: {version}")
env.Append(CPPDEFINES=[("PLANE_RADAR_VERSION", env.StringifyMacro(version))])
