"""Build-policy regressions without requiring CUDA hardware.
构建策略回归测试，不依赖 CUDA 硬件。
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("same_build", ROOT / "tools/build.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildPolicy(unittest.TestCase):
    def test_visual_studio_cuda_rejected_before_probe(self):
        result = self.probe('set(CMAKE_GENERATOR "Visual Studio 17 2022")\n'
                            'set(SAME_ENABLE_CUDA ON)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("direct CUDA compiler invocation", result.stderr)
        self.assertNotIn("Looking for a CUDA compiler", result.stdout)

    def test_visual_studio_cpu_preserved(self):
        result = self.probe('set(CMAKE_GENERATOR "Visual Studio 17 2022")\n'
                            'set(SAME_ENABLE_CUDA OFF)')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_launcher_preserves_explicit_tools_and_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            argv = ["build.py", "--build-dir", directory, "--test", "--cuda", "off"]
            with patch.object(BUILD.sys, "argv", argv), \
                    patch.dict(BUILD.os.environ, {"CXX": "chosen-cxx"}, clear=True), \
                    patch.object(BUILD, "msvc_environment") as discover, \
                    patch.object(BUILD, "run") as run:
                BUILD.main()
            discover.assert_not_called()
            self.assertEqual(run.call_count, 3)
            configure = run.call_args_list[0].args[0]
            self.assertEqual(configure[configure.index("-G") + 1], "Ninja")
            self.assertIn("-DSAME_ENABLE_CUDA=OFF", run.call_args_list[0].args[0])
            for call in run.call_args_list:
                self.assertEqual(call.args[1]["CXX"], "chosen-cxx")

    def test_launcher_rejects_changed_cached_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            argv = ["build.py", "--build-dir", directory, "--configure-only",
                    "--toolchain", "environment"]
            with patch.object(BUILD.sys, "argv", argv), \
                    patch.dict(BUILD.os.environ, {"CXX": "first"}, clear=True), \
                    patch.object(BUILD, "run"):
                BUILD.main()
            (Path(directory) / "CMakeCache.txt").write_text("CMAKE_CXX_COMPILER:FILEPATH=first\n")
            with patch.object(BUILD.sys, "argv", argv), \
                    patch.dict(BUILD.os.environ, {"CXX": "second"}, clear=True), \
                    patch.object(BUILD, "run") as run:
                with self.assertRaisesRegex(RuntimeError, "Toolchain changed"):
                    BUILD.main()
                run.assert_not_called()

    def test_explicit_toolchains(self):
        for env, definitions in [({"CXX": "g++"}, []), ({"CC": "clang"}, []),
                                 ({"CMAKE_TOOLCHAIN_FILE": "cross.cmake"}, []),
                                 ({}, ["CMAKE_CXX_COMPILER:FILEPATH=g++"]),
                                 ({}, ["CMAKE_TOOLCHAIN_FILE=cross.cmake"])]:
            self.assertTrue(BUILD.explicit_toolchain(env, definitions))
        self.assertFalse(BUILD.explicit_toolchain({}, ["BUILD_TESTING=OFF"]))

    def probe(self, settings):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "policy.cmake"
            script.write_text(settings + f'\ninclude("{ROOT.as_posix()}/cmake/Cuda.cmake")\n',
                              encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)

    def test_mingw_auto_falls_back_without_cuda_probe(self):
        result = self.probe('set(WIN32 TRUE)\nset(CMAKE_CXX_COMPILER_ID GNU)\nset(SAME_ENABLE_CUDA ON)')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("requires MSVC", result.stdout)
        self.assertNotIn("Looking for a CUDA compiler", result.stdout)

    def test_mingw_required_fails(self):
        result = self.probe('set(WIN32 TRUE)\nset(CMAKE_CXX_COMPILER_ID GNU)\n'
                            'set(SAME_ENABLE_CUDA ON)\nset(SAME_REQUIRE_CUDA ON)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires MSVC", result.stderr)

    def test_conflicting_flags(self):
        result = self.probe('set(SAME_ENABLE_CUDA OFF)\nset(SAME_REQUIRE_CUDA ON)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conflicts", result.stderr)

    def test_cpu_skips_probe(self):
        result = self.probe('set(SAME_ENABLE_CUDA OFF)')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CPU", result.stdout)

    def test_macos_auto(self):
        result = self.probe('set(WIN32 FALSE)\nset(APPLE TRUE)\nset(SAME_ENABLE_CUDA ON)')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not supported on macOS", result.stdout)


if __name__ == "__main__":
    unittest.main()
