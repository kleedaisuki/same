# 构建环境契约 / Build environment contract

## 分层 / Layers

- `tools/build.py`：本机构建入口，发现并初始化环境，使用参数数组调用配置、构建、测试；不修改全局 PATH 或 IDE 设置。
- `CMakePresets.json`：原生 Ninja 配置，使用调用者环境，不偷偷执行自动发现。
- `cmake/Cuda.cmake`：在依赖下载前验证平台与 CUDA，绝不修改已经启用的 C/C++ 编译器。

The native launcher owns discovery and process environment. Presets use caller-owned tools. CMake validates capabilities before dependency downloads; it never replaces an enabled compiler. Cross builds remain caller-owned through standard CMake toolchain files.

## 选择顺序 / Selection order

显式 C/C++ 编译器或工具链文件 > 已初始化的 MSVC 开发环境 > Windows 安装器发现的最新 C++ Build Tools > 调用者环境。非 Windows 不进行 MSVC 发现。Windows 自动发现目前以 x64 为默认；其他架构使用显式工具链。

Explicit C/C++ compilers or toolchain files > initialized MSVC developer environment > latest Windows C++ Build Tools discovered by the installer > caller environment. Non-Windows hosts never run MSVC discovery. Windows auto-discovery defaults to x64; other architectures require explicit tools.

自动发现不等于自动修复安装。缺少 SDK、CUDA 与 MSVC 版本不兼容时保留探测诊断；不使用 `--allow-unsupported-compiler`。多版本 MSVC 不进行组合穷举，使用受支持版本的开发环境可明确指定选择。

Discovery does not repair installations. Missing SDKs and CUDA/MSVC version incompatibility retain probe diagnostics. Unsupported-compiler overrides are never used. Multiple MSVC/CUDA combinations are not exhaustively searched; initialize a supported developer environment to select a specific installation.

## 不变量 / Invariants

- CUDA 使用 Ninja 直接调用 nvcc，不依赖 Visual Studio CUDA/MSBuild 扩展。VS 生成器开启 CUDA 时明确报错；纯 CPU 保持可用。/ CUDA uses direct nvcc invocation through Ninja, without Visual Studio CUDA/MSBuild integration. CUDA-enabled VS generators fail explicitly; CPU-only VS builds remain supported.

- 配置、构建、测试继承同一环境。/ Configure, build and test inherit the same environment.
- Windows MinGW CPU 路径继续可用，不与 nvcc/MSVC 混用。/ MinGW CPU builds remain supported without mixing them with nvcc/MSVC.
- `SAME_ENABLE_CUDA` 保持原有自动回退语义；新增 `SAME_REQUIRE_CUDA` 只增加严格模式。/ Existing optional-CUDA semantics remain; the new requirement option adds strict configuration.
- 工具链变化使用新目录；入口记录选择并拒绝检测到的缓存冲突。工具链文件内容或 PATH 中工具版本变化后，调用者仍应使用新目录。/ Use a fresh directory after toolchain changes. The launcher records selections and rejects detected cache conflicts; changes inside toolchain files or PATH tools still require caller-managed fresh directories.
- 编译可用不等于 GPU 运行可用。/ Build capability does not imply GPU runtime availability.

## 本次验证 / Validation for this change

Windows MSVC 19.44 + CUDA 13.3、Windows MinGW 16.1 CPU 自动回退、WSL Ubuntu 24.04 GCC 13.3 CPU 均完成构建，各 8/8 CTest 通过。策略测试覆盖显式选择、环境传递、缓存冲突、MinGW 自动/严格模式、macOS 分支及互斥选项。macOS 已接入 CI，但本次未在 macOS 本机执行；GPU 硬件执行不由这些构建结果证明。

Windows MSVC 19.44/CUDA 13.3, Windows MinGW 16.1 CPU fallback and WSL Ubuntu 24.04/GCC 13.3 CPU built successfully, each passing 8/8 CTest tests. Policy tests cover explicit selection, environment propagation, cache conflicts, MinGW automatic/required modes, the macOS branch and conflicting options. macOS is covered by the configured CI matrix but was not executed locally for this change. These results do not establish GPU hardware execution.
