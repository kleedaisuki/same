# 构建环境契约 / Build environment contract

## 入口 / Entry points

构建只需要 CMake ≥ 3.25、C/C++23 工具链与构建工具，不需要 Python。首次配置需要网络下载固定版本、SHA-256 校验的依赖。CUDA 使用 C++20。

Build requires CMake ≥ 3.25, a C/C++23 toolchain and build tool; Python is not required. Initial configuration downloads version/hash-pinned dependencies. CUDA uses C++20.

```sh
cmake -DSAME_BUILD_TESTS=ON -P tools/build.cmake
cmake -DSAME_CUDA_MODE=on -DSAME_BUILD_TESTS=ON -P tools/build.cmake
cmake -DSAME_CUDA_MODE=off -DCMAKE_BUILD_TYPE=Debug -P tools/build.cmake
cmake -DSAME_CONFIGURE_ONLY=ON -DSAME_BUILD_DIR=build/custom -P tools/build.cmake
```

所有 `-D` 必须放在 `-P` 前面。`SAME_CUDA_MODE=auto|on|off` 默认为 auto，on 表示 CUDA 必须可构建，off 不进行 CUDA 探测。`SAME_BUILD_PARALLEL` 默认 4；`SAME_BUILD_TESTS=ON` 在构建后运行 CTest。额外工程选项使用分号列表，例如 `"-DSAME_CMAKE_ARGS=-DBUILD_TESTING=OFF;-DSAME_SANITIZERS=ON"`。

Put all `-D` options before `-P`. CUDA mode defaults to auto; on requires CUDA and off skips probing. Parallelism defaults to four. The tests switch runs CTest after building. Additional project options use the semicolon-separated `SAME_CMAKE_ARGS` list.

## Windows 选择顺序 / Windows selection order

| 顺序 / Order | 路线 / Route | 条件 / Requirement |
|---|---|---|
| 1 | CUDA Toolkit（SDK）+ Ninja，直接 nvcc / direct nvcc | Toolkit、兼容 MSVC、Windows SDK、Ninja |
| 2 | Visual Studio 生成器 + CUDA Build Customizations | 对应 VS 实例安装扩展，且有匹配 Toolkit / installed integration and matching Toolkit |
| 3 | CPU + Ninja（无 Ninja 时 MSBuild） | 可工作的 C/C++ 工具链 / working host toolchain |

自动入口使用 `vswhere` 发现最新 C++ Build Tools，并在本进程导入 x64 `VsDevCmd` 环境。每条 CUDA 路线在隔离目录配置微型原生 CUDA 工程、验证 C++20 编译链接，不下载项目依赖也不运行 GPU。失败日志保留在 `build/probes/`。SDK 不可用后才探测扩展，最后才 CPU；严格 on 模式不含 CPU。实际项目配置/构建失败立即报错，不掩盖为回退。

The launcher imports an x64 MSVC environment discovered with vswhere. Isolated CUDA probes compile/link C++20 without dependencies or GPU execution. Probe logs remain under `build/probes/`. SDK failure precedes extension probing, then CPU; strict mode excludes CPU. Actual project errors never trigger a silent fallback.

**扩展不是独立 CUDA 编译器。** NVIDIA Build Customizations 仍需要匹配的 Toolkit、nvcc 和主机编译器，只有扩展文件而无 Toolkit 不能构建 CUDA。自动入口不安装软件、不绕过 NVIDIA 编译器版本检查、不穷举所有 MSVC/Toolkit 组合。

**The extension is not a separate compiler.** It still requires a matching Toolkit, nvcc and host compiler. Discovery neither installs software nor bypasses NVIDIA compatibility checks or exhaustively searches version combinations.

显式 `CC`、`CXX`、`CMAKE_*_COMPILER`、`CMAKE_TOOLCHAIN_FILE` 优先；`SAME_USE_ENVIRONMENT=ON` 完全保留调用者环境，并禁用跨 VS 生成器回退。显式工具链文件请使用绝对路径。切换生成器/工具链使用新的 `SAME_BUILD_DIR`；同一路径工具升级或工具链文件内容变化后也应清理缓存。Linux/macOS 使用调用者环境；macOS 应关闭 CUDA；OpenCL 是否可运行取决于设备与系统框架。

Explicit compiler/toolchain choices take precedence. `SAME_USE_ENVIRONMENT=ON` preserves caller tools and disables automatic VS-generator fallback. Use absolute toolchain paths and fresh build directories after generator/toolchain changes, including in-place upgrades. Unix hosts use caller tools; disable CUDA on macOS; OpenCL runtime availability is device-dependent.

## CLion 与原生 CMake / CLion and native CMake

**Windows 使用预设前，请打开 Visual Studio 的 “x64 Native Tools Command Prompt for VS 2022”，再进入项目目录。必须使用 x64 目标环境，不要使用 x86 开发者终端或未初始化的普通终端。CLion 用户请选择 Visual Studio 工具链，并将架构设为 amd64/x64，由 IDE 初始化环境。**

**On Windows, open “x64 Native Tools Command Prompt for VS 2022” before using presets, then enter the project directory. Use an x64 target environment, not an x86 developer prompt or an uninitialized terminal. In CLion, select the Visual Studio toolchain with amd64/x64 architecture so the IDE initializes the environment.**

`release` 会尝试启用 CUDA，探测失败时允许退回 CPU；`cuda` 要求 CUDA 可构建。两者启用 CUDA 后都支持运行时自动分流。检查配置输出中的 `same backend: CUDA`，不要只凭构建成功判断 GPU 支持。

`release` attempts CUDA with CPU fallback; `cuda` requires a usable CUDA toolchain. Both support runtime auto routing when CUDA is enabled. Check for `same backend: CUDA` in configure output rather than treating build success as proof of GPU support.

如果此前使用了错误架构或缓存了 CUDA 探测失败，请切换到正确的 x64 环境后执行 `cmake --fresh --preset release`，再重新构建；CLion 中使用重置 CMake 缓存并重新加载。

If an earlier configuration used the wrong architecture or cached a failed CUDA probe, switch to the correct x64 environment and run `cmake --fresh --preset release` before rebuilding; in CLion, reset the CMake cache and reload.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
# Explicit MSBuild route / 显式扩展路线：
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64 -DSAME_REQUIRE_CUDA=ON
```

CLion 打开根 `CMakeLists.txt`，Windows 选择 **Visual Studio 工具链**（不是默认 MinGW），使用 Ninja 的 release/cuda/cpu 预设或独立 CMake 配置。CLion 初始化 MSVC 环境；原生 CMake 在 `check_language(CUDA)` 后 `enable_language(CUDA)`，将 `.cu` 注册为真实 CUDA 目标源，并导出 `compile_commands.json`（Ninja）。非默认 SDK 使用 `CUDAToolkit_ROOT` 或 `CMAKE_CUDA_COMPILER`。显式无效编译器被视为配置错误，不会偷偷替换。

Open the root CMake project in CLion and select the Visual Studio toolchain on Windows, with Ninja presets. CUDA is a native enabled language and `.cu` files are target sources, not opaque custom commands. Ninja exports a compilation database. Set `CUDAToolkit_ROOT` or `CMAKE_CUDA_COMPILER` for nondefault SDKs; invalid explicit compilers are configuration errors.

**生成器不能在同一次 CMake 配置中切换。** 直接 `cmake -S/-B` 或 IDE 配置尊重当前生成器，只在该路线检测 CUDA 后回退 CPU；跨 Ninja → VS → CPU 选择由配置前的 CMake 脚本入口完成。`SAME_ENABLE_CUDA` 与 `SAME_REQUIRE_CUDA` 原有选项保留。MinGW 不混用 MSVC CUDA，自动模式给出 CPU 回退原因。

**A generator cannot change during a CMake configure.** Native/IDE entry points probe within the selected generator and fall back to CPU there. Cross-generator selection belongs to the pre-configuration script. Existing optional/required CUDA options remain. MinGW never mixes host ABI with MSVC CUDA.

## 检验与边界 / Validation and boundaries

`build_policy` CTest 检查路线优先级、严格模式和禁用 CUDA；真实环境的配置、编译、测试结果由实际运行记录证明。编译成功不证明 GPU 可运行，也不证明 CLion UI 已人工验收。

The build-policy CTest verifies ordering, strict mode and disabled CUDA. Actual build/test runs establish platform coverage; compilation does not establish GPU execution or manual CLion UI acceptance.

## 官方依据 / Official references

- [CMake CheckLanguage](https://cmake.org/cmake/help/latest/module/CheckLanguage.html)
- [CMake Visual Studio CUDA toolset](https://cmake.org/cmake/help/latest/variable/CMAKE_VS_PLATFORM_TOOLSET_CUDA.html)
- [NVIDIA Windows installation and Build Customizations](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/)
- [JetBrains CLion CUDA projects](https://www.jetbrains.com/help/clion/cuda-projects.html)

本次采用成熟原生 CMake 机制，不引入研究性构建框架；问题是工具链发现与 IDE 元数据一致性，而非需要新编译算法。

This change uses established native CMake mechanisms rather than an experimental build framework: the issue is toolchain discovery and IDE metadata consistency, not a new compilation algorithm.


Windows CUDA 12.8 的静态运行库在 MSBuild 链接时可能报告 LNK4098（LIBCMT/MSVCRT 默认库警告）。保留现有静态 CUDA 运行库，避免为消除警告而引入 cudart DLL 部署依赖；未使用 /NODEFAULTLIB 掩盖。 / CUDA 12.8 static runtime may produce MSBuild LNK4098 default-library warnings. Static linkage is preserved rather than introducing a cudart DLL deployment requirement or suppressing libraries.

## 可选 OpenCL 核显 / Optional OpenCL iGPU

`SAME_ENABLE_OPENCL=ON` 默认启用，下载固定版本 OpenCL-Headers，不要求安装 OpenCL SDK 或链接系统加载器。运行时动态加载系统 OpenCL；驱动不可用或没有合格设备时回退 CPU。生产选择 GPU 且报告统一主机内存的设备，不把任意 CPU OpenCL 设备当核显。
OpenCL headers are pinned; the system loader and driver are discovered dynamically. Missing eligible devices safely fall back to CPU.

```sh
cmake --preset igpu
cmake --build --preset igpu
ctest --preset igpu
# 完全关闭加速器 / Completely disable accelerators:
cmake -S . -B build/host-only -DSAME_ENABLE_CUDA=OFF -DSAME_ENABLE_OPENCL=OFF
# 脚本入口关闭 OpenCL / Disable OpenCL through the launcher:
cmake -DSAME_CUDA_MODE=off -DSAME_CMAKE_ARGS=-DSAME_ENABLE_OPENCL=OFF -P tools/build.cmake
```

`cpu` 预设关闭 CUDA 和 OpenCL；`igpu` 只启用 OpenCL；`release`/`cuda` 中 OpenCL 与 CUDA 独立配置。CUDA 自动路线回退到主机编译不等于禁用 OpenCL。Linux 需要可用的 OpenCL ICD/驱动；Windows 使用系统 OpenCL.dll；macOS 尝试系统框架，但不能以构建成功推断设备可用。
CUDA toolchain fallback does not disable OpenCL. Host builds and runtime availability are separate concerns on all platforms.

原生测试环境变量 `SAME_REQUIRE_IGPU=1` 要求真实合格设备；`SAME_REQUIRE_OPENCL_TEST_DEVICE=1` 用于 CI 的 OpenCL 测试工厂，允许 PoCL CPU 设备且不能静默跳过。它们不是生产后端选择开关。GitHub Actions 配置覆盖 Windows/Linux/macOS 的 OpenCL 开/关构建，并单独运行 PoCL 和主机 sanitizer；实际结论须检查对应提交运行结果。
Test-only requirement switches distinguish physical-device validation from PoCL kernel semantics. Workflow configuration is not proof that a particular run passed.

运行与持久化学习见 [上下文学习](contextual-learning.md)。 / See the contextual-learning contract for runtime behavior.

### 核显冷启动设置 / iGPU cold-start settings

`igpu_bootstrap_ms=100.0`、`cuda_bootstrap_ms=100.0` 和 `cold_exploration_fraction=0.05` 是运行时配置，不是 CMake 选项。前两者为设备预检成本估计，后者为自动 PGO 的本轮探索信用比例。共享非阻塞门槛在 OpenCL 发现或 CUDA 创建之前执行；短且未知的扫描可能完全不启动设备。设备探测与内核激活分离；构建启用 OpenCL 不表示每次扫描都会编译内核。`--igpu` 可明确请求激活并绕过自动冷启动门槛。见 [冷启动准入契约](contextual-learning.md#冷启动准入--cold-start-admission)。
These are runtime settings, not CMake options. Optional OpenCL support does not imply kernel activation on every scan; forced iGPU bypasses the automatic cold gate.
