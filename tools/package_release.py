"""创建可核验发布包并运行解包后的程序。 / Package and smoke-test extracted release binaries.

Example: python tools/package_release.py --build build/package --platform windows-x64
--flavor cuda --output dist
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def version():
    """CMake 是版本号唯一来源。 / CMake is the version source of truth."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"project\(same VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text)[1]


def run(args, **kwargs):
    """任何非零退出均阻止发布。 / Nonzero exits prevent publication."""
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          encoding="utf-8", timeout=120, **kwargs).stdout


def cache(build):
    """只解析显式 CMake 缓存条目。 / Read explicit CMake cache entries."""
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)", line)
        if match:
            result[match[1]] = match[2]
    return result


def smoke(binary, expected_version):
    """检查发布二进制的版本和精确分组；无需 GPU。 / Check packaged version and exact groups without a GPU."""
    if expected_version not in run([str(binary), "--version"]):
        raise RuntimeError("binary/version mismatch")
    with tempfile.TemporaryDirectory(prefix="same-package-smoke-") as directory:
        root = Path(directory).resolve()
        (root / "a").write_bytes(b"same release smoke\n")
        (root / "b").write_bytes(b"same release smoke\n")
        (root / "c").write_bytes(b"different payload!\n")
        output = run([str(binary), "scan", "--cpu", "--no-pgo", "--no-telemetry"], cwd=root)
        if '"a"' not in output or '"b"' not in output or '"c"' in output:
            raise RuntimeError(f"packaged binary grouping failed: {output}")


def package(args):
    """只打包白名单文件，不包含本地状态。 / Package an allowlist, never local workspace state."""
    build = Path(args.build).resolve()
    settings = cache(build)
    def dependency(name):
        """支持本地固定源码复用。 / Support explicit local dependency source overrides."""
        override = settings.get("FETCHCONTENT_SOURCE_DIR_" + name.upper())
        return Path(override).resolve() if override else build / "_deps" / (name + "-src")
    cuda = args.flavor == "cuda"
    if settings.get("SAME_ENABLE_OPENCL") != "ON":
        raise RuntimeError("release must include optional OpenCL")
    if settings.get("SAME_ENABLE_CUDA") != ("ON" if cuda else "OFF"):
        raise RuntimeError("CUDA flavor/cache mismatch")
    if cuda and settings.get("SAME_REQUIRE_CUDA") != "ON":
        raise RuntimeError("CUDA releases must forbid silent CPU-only builds")
    machine = platform.machine().lower()
    expected = "arm64" if machine in ("arm64", "aarch64") else "x64" if machine in ("amd64", "x86_64") else machine
    if not args.platform.endswith("-" + expected):
        raise RuntimeError(f"runner architecture {machine} mismatches asset name")
    exe = "same.exe" if os.name == "nt" else "same"
    binary = build / "Release" / exe
    if not binary.exists():
        binary = build / exe
    revision = run(["git", "rev-parse", "HEAD"], cwd=ROOT).strip()
    dirty = bool(run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT).strip())
    if dirty and os.environ.get("GITHUB_ACTIONS") == "true":
        raise RuntimeError("CI packaging must use an unmodified source checkout")
    name = f"same-v{version()}-{args.platform}-{args.flavor}"
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="same-package-") as directory:
        stage = Path(directory) / name
        (stage / "bin").mkdir(parents=True)
        shutil.copy2(binary, stage / "bin" / exe)
        for file in ("LICENSE", "README.md", "docs/build.md", "docs/releasing.md"):
            shutil.copy2(ROOT / file, stage / Path(file).name)
        licenses = stage / "licenses"
        licenses.mkdir()
        for source, dest in ((dependency("blake3") / "LICENSE_A2", "BLAKE3-Apache-2.0.txt"),
                             (dependency("blake3") / "LICENSE_CC0", "BLAKE3-CC0.txt"),
                             (dependency("tomlplusplus") / "LICENSE", "tomlplusplus.txt")):
            shutil.copy2(source, licenses / dest)
        (licenses / "SQLite.txt").write_text("SQLite is in the public domain. https://www.sqlite.org/copyright.html\n", encoding="utf-8")
        if cuda:
            compiler = Path(settings["CMAKE_CUDA_COMPILER"])
            eula = compiler.parent.parent / "EULA.txt"
            if eula.exists():
                shutil.copy2(eula, licenses / "NVIDIA-CUDA-EULA.txt")
            else:
                raise RuntimeError("CUDA redistributable license missing")
        manifest = {"version": version(), "commit": revision, "source_dirty": dirty, "platform": args.platform,
                    "flavor": args.flavor, "cuda": cuda, "opencl": True,
                    "cuda_architectures": settings.get("CMAKE_CUDA_ARCHITECTURES") if cuda else None,
                    "cuda_runtime": settings.get("CMAKE_CUDA_RUNTIME_LIBRARY") if cuda else None,
                    "source": f"https://github.com/kleedaisuki/same/tree/{revision}",
                    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        (stage / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        smoke(stage / "bin" / exe, version())
        suffix = ".zip" if os.name == "nt" else ".tar.gz"
        archive = output / (name + suffix)
        if suffix == ".zip":
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as target:
                for path in sorted(stage.rglob("*")):
                    if path.is_file():
                        target.write(path, path.relative_to(stage.parent))
        else:
            with tarfile.open(archive, "w:gz") as target:
                target.add(stage, arcname=name)
        extracted = Path(directory) / "extracted"
        if suffix == ".zip":
            with zipfile.ZipFile(archive) as source:
                source.extractall(extracted)
        else:
            with tarfile.open(archive) as source:
                source.extractall(extracted, filter="data")
        smoke(extracted / name / "bin" / exe, version())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
        print(json.dumps(manifest))
        print(archive)


def main():
    """标签必须精确匹配产品版本。 / Release tags must exactly match the product version."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-tag")
    parser.add_argument("--build")
    parser.add_argument("--platform", choices=("windows-x64", "linux-x64", "macos-arm64"))
    parser.add_argument("--flavor", choices=("standard", "cuda"))
    parser.add_argument("--output", default="dist")
    args = parser.parse_args()
    if args.verify_tag:
        if args.verify_tag != "v" + version():
            raise SystemExit("release tag must match CMake project version")
        return
    if not all((args.build, args.platform, args.flavor)):
        parser.error("--build, --platform and --flavor are required")
    package(args)


if __name__ == "__main__":
    main()
