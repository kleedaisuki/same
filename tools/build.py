"""Discover a native build environment, then configure/build/test in that environment.
自动发现本机构建环境，并在同一环境中配置、构建和测试。Python >= 3.9。
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def explicit_toolchain(env, definitions):
    """Explicit compiler choices outrank discovery. 显式编译器选择优先。"""
    keys = {item.split("=", 1)[0].split(":", 1)[0] for item in definitions}
    return any(env.get(key) for key in ("CC", "CXX", "CMAKE_TOOLCHAIN_FILE")) or bool(
        keys & {"CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_TOOLCHAIN_FILE"})


def msvc_environment(env):
    """Use Microsoft's installer inventory, not guessed installation directories.
    使用微软安装器清单，而非猜测 VS 安装路径。
    """
    if env.get("VSCMD_VER") and shutil.which("cl", path=env.get("PATH")):
        return env.copy()
    vswhere = shutil.which("vswhere", path=env.get("PATH"))
    if not vswhere:
        vswhere = str(Path(env.get("ProgramFiles(x86)", "C:/Program Files (x86)")) /
                      "Microsoft Visual Studio/Installer/vswhere.exe")
    if not Path(vswhere).is_file():
        return None
    install = subprocess.check_output([
        vswhere, "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"
    ], env=env, text=True, encoding="utf-8").strip()
    if not install:
        return None
    setup = Path(install) / "Common7/Tools/VsDevCmd.bat"
    # The batch file only contains an installer-discovered path, not user arguments.
    # 批处理只包含安装器发现的路径，不插入用户参数；构建参数始终以数组传递。
    with tempfile.TemporaryDirectory(prefix="same-env-") as directory:
        script = Path(directory) / "env.cmd"
        output = Path(directory) / "env.txt"
        script.write_text(
            f'@echo off\ncall "{setup}" -no_logo -arch=x64 -host_arch=x64\n'
            f'if errorlevel 1 exit /b 1\nset > "{output}"\n', encoding="mbcs")
        subprocess.run([env.get("COMSPEC", "cmd.exe"), "/d", "/u", "/c", str(script)],
                       env=env, check=True)
        result = env.copy()
        for line in output.read_text(encoding="utf-16-le").splitlines():
            key, sep, value = line.partition("=")
            if key and sep:
                result[key.upper()] = value
        return result


def run(command, env):
    print("+ " + subprocess.list2cmdline([str(item) for item in command]), flush=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cuda", choices=("auto", "on", "off"), default="auto")
    parser.add_argument("--toolchain", choices=("auto", "environment"), default="auto",
                        help="environment: use the caller's tools without MSVC discovery")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--config", choices=("Debug", "Release", "RelWithDebInfo"), default="Release")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="VAR=VALUE")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("--parallel must be positive")
    reserved = {"SAME_ENABLE_CUDA", "SAME_REQUIRE_CUDA", "CMAKE_BUILD_TYPE"}
    if any("=" not in d or d.split("=", 1)[0].split(":", 1)[0] in reserved for d in args.definitions):
        parser.error("-D requires VAR=VALUE; use --cuda/--config for backend and configuration")
    env = os.environ.copy()
    selected = "environment"
    definitions = args.definitions.copy()
    if os.name == "nt" and args.toolchain == "auto" and not explicit_toolchain(env, definitions):
        discovered = msvc_environment(env)
        if discovered:
            env = discovered
            selected = "msvc"
            definitions += ["CMAKE_C_COMPILER=cl", "CMAKE_CXX_COMPILER=cl"]
    directory = (args.build_dir or ROOT / "build" /
                 f"{sys.platform}-{selected}-{args.config.lower()}-cuda-{args.cuda}").resolve()
    # Refuse stale compiler caches instead of silently mutating their ABI.
    # 拒绝复用不同编译器的缓存，避免静默改变二进制接口。
    cache = directory / "CMakeCache.txt"
    identity = {
        "selection": selected,
        "environment": {key: env.get(key, "") for key in
                        ("CC", "CXX", "CMAKE_TOOLCHAIN_FILE", "CUDAHOSTCXX", "CUDACXX")},
        "compilers": [item for item in definitions if item.split("=", 1)[0].split(":", 1)[0]
                      in {"CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_TOOLCHAIN_FILE",
                          "CMAKE_CUDA_COMPILER", "CMAKE_CUDA_HOST_COMPILER"}],
        "cl": shutil.which("cl", path=env.get("PATH")) if selected == "msvc" else None,
    }
    marker = directory / "same-toolchain.json"
    if cache.exists() and marker.exists() and json.loads(marker.read_text()) != identity:
        raise RuntimeError(f"Toolchain changed in {directory}; choose a fresh --build-dir")
    if selected == "msvc" and cache.exists():
        entries = cache.read_text(encoding="utf-8")
        compiler = next((line.split("=", 1)[1] for line in entries.splitlines()
                         if line.startswith("CMAKE_CXX_COMPILER:")), "")
        if compiler and Path(compiler).name.lower() not in ("cl", "cl.exe"):
            raise RuntimeError(f"{directory} contains another compiler; choose a fresh --build-dir")
    print(f"same: environment={selected}, CUDA={args.cuda}, build={directory}", flush=True)
    run(["cmake", "-S", str(ROOT), "-B", str(directory), "-G", "Ninja",
         f"-DCMAKE_BUILD_TYPE={args.config}",
         f"-DSAME_ENABLE_CUDA={'OFF' if args.cuda == 'off' else 'ON'}",
         f"-DSAME_REQUIRE_CUDA={'ON' if args.cuda == 'on' else 'OFF'}",
         *[f"-D{item}" for item in definitions]], env)
    marker.write_text(json.dumps(identity, indent=2), encoding="utf-8")
    if not args.configure_only:
        run(["cmake", "--build", str(directory), "--config", args.config,
             "--parallel", str(args.parallel)], env)
        if args.test:
            run(["ctest", "--test-dir", str(directory), "-C", args.config,
                 "--output-on-failure"], env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"same build failed: {error}", file=sys.stderr)
        sys.exit(1)
