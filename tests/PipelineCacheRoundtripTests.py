"""Exercise the shipping compositor cache across independent Vulkan processes."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="vulkana-pipeline-cache-") as directory:
    env = os.environ.copy()
    env.update(XDG_CACHE_HOME=directory, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1",
               QT_LOGGING_RULES="imageeditor.vulkan.info=true")
    env.pop("__GL_SHADER_DISK_CACHE", None)
    env.pop("MESA_SHADER_CACHE_DISABLE", None)

    def run(label, expected, cold=False):
        launch_env = env.copy()
        if cold:
            launch_env.update(__GL_SHADER_DISK_CACHE="0", MESA_SHADER_CACHE_DISABLE="true")
        result = subprocess.run([sys.argv[1], "--channels", "--validation"], env=launch_env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
        lines = [line for line in result.stdout.splitlines() if "Compositor pipeline initialization:" in line]
        print(label, *lines, sep="\n", flush=True)
        if result.returncode or not any(f"cache {expected} result 0" in line for line in lines):
            raise RuntimeError(result.stdout)

    run("First process", "new")
    files = list(Path(directory, "vulkanaEditor", "vulkan").glob("compositor-*.bin"))
    if len(files) != 1:
        raise RuntimeError("Expected one persisted compositor cache")
    run("Fresh process reuses cache", "loaded")
    before = hashlib.sha256(files[0].read_bytes()).digest()
    run("Cold-cache bypass", "bypassed", cold=True)
    if hashlib.sha256(files[0].read_bytes()).digest() != before:
        raise RuntimeError("Cold-cache startup modified the warm cache")
    run("Normal launch after cold-cache", "loaded")
