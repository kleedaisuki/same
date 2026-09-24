# 阅读与维护 / Reading and maintenance

## 从哪里开始 / Reading order

跨模块维护和兼容性敏感重构可先使用仓库内的 [same-maintenance skill](../skills/same-maintenance/SKILL.md) 作为路线图；各文档与测试仍是实际契约。 / For cross-module maintenance and compatibility-sensitive refactors, the repository-local skill is a routing aid; documentation and tests remain the actual contracts.

| 顺序 / Order | 文件 / File | 先理解的契约 / Contract to understand |
|---|---|---|
| 1 | `include/same/config.hpp` | 工作线程、缓冲区、队列预算 / Worker, buffer and queue budgets |
| 2 | `include/same/files.hpp`、`store.hpp` | 路径、文件戳与事务所有权 / Paths, file stamps and transaction ownership |
| 3 | `src/application.cpp` | 扫描 → 哈希 → 精确比较 → 复核 → 输出 / Scan → hash → compare → revalidate → emit |
| 4 | `include/same/resources.hpp` | 有界任务准入与整项 CPU 重试 / Bounded admission and whole-operation CPU retry |
| 5 | `src/compute_cuda.cu`、`blake3_scalar.hpp` | 流生命周期、最终块保留与树归并 / Stream lifetime, retained final block and tree reduction |

注释采用双语 Doxygen，重点是参数约束、所有权、生命周期和失败行为，而不是逐句翻译代码。理解算法时先读公开接口，再读内部实现；不要把哈希相等当作文件相等，也不要把文件戳检查当作文件系统快照。

Comments use bilingual Doxygen and describe constraints, ownership, lifetime and failure behavior rather than paraphrasing statements. Read public contracts before implementations. Digest equality is not file equality; stamp validation is not a filesystem snapshot.

## 格式检查 / Formatting

安装 `clang-format` 后重新配置 CMake，即可使用下列可选开发目标；普通构建不依赖该工具，也不依赖 Python。

Install `clang-format` and reconfigure CMake to expose these optional targets. Normal builds require neither the formatter nor Python.

```sh
cmake --build build/release --target format
cmake --build build/release --target format-check
```

将 `build/release` 替换成实际构建目录。仓库根目录的 `.clang-format` 使用四空格缩进、100 列行宽，展开非空短函数与控制流。目标只处理 `include/`、`src/`、`tests/` 的 C++/CUDA 文件，不修改下载依赖或构建产物。风格机制遵循 [LLVM clang-format 官方说明](https://clang.llvm.org/docs/ClangFormatStyleOptions.html)。

Replace `build/release` with your build directory. The root style uses four-space indentation and a 100-column limit, expanding nonempty short functions and control flow. Only project C++/CUDA sources under `include/`, `src/` and `tests/` are formatted; downloaded dependencies and generated files are excluded.

## 验证 / Validation

```sh
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

多配置生成器增加 `--config Release`（构建）和 `-C Release`（CTest）。构建策略由 CMake 脚本测试，CLI 集成测试直接启动原生可执行文件并调用 SQLite C API，不再发现或启动 Python。历史性能实验脚本 `tools/benchmark.py` 仅为可选复现实验，不参与配置、构建、CTest 或 CI。

For multi-configuration generators add `--config Release` to builds and `-C Release` to CTest. Route policy is tested in CMake; native CLI integration tests spawn the executable and use the SQLite C API. The historical `tools/benchmark.py` is an optional experiment only, never part of configuration, builds, CTest or CI.

## CI 与依赖治理 / CI and dependency governance

`.github/workflows/release.yml` 在每次推送和拉取请求中调用 `ci.yml`，并构建发布包；标签发布须等所有检查通过。`ci.yml` 的原生矩阵覆盖三种操作系统和 OpenCL 开关，另有 PoCL 内核与主机 sanitizer 检查。独立遥测归档脚本不属于 CTest，故在三种操作系统上单独运行 `python -m unittest discover -s scripts/telemetry -p test_merge.py -v`。其测试数据库在仓库 `.temp/` 中创建并清理；普通 C++ 构建仍不依赖 Python。

The release workflow invokes reusable CI for every push and pull request, then builds packages; tag publication waits for all checks. Native CI covers three operating systems with OpenCL on/off, plus PoCL kernel and host sanitizer jobs. The standalone telemetry archive tests run separately on all three operating systems because they are not CTest targets. Their fixture databases are created and removed under repository-local `.temp/`; normal C++ builds still require no Python.

新增或升级构建依赖时，保持 `cmake/Dependencies.cmake` 中固定版本及 SHA-256，同步更新 README 与相关许可证/发布资料；不要为清理告警而改变可交付二进制的运行时依赖。新增独立脚本时，要明确最低运行时版本、把测试纳入 CI，并将测试产物放在仓库 `.temp/` 或 `.cache/`。工作流配置本身只说明计划覆盖范围，实际结论仍以对应提交的 Actions 运行结果为准。

When adding or upgrading build dependencies, retain version and SHA-256 pins in `cmake/Dependencies.cmake` and update the README and relevant license/release material. Do not change distributed runtime dependencies merely to silence a warning. New standalone scripts need an explicit minimum runtime, CI tests, and repository-local `.temp/` or `.cache/` test output. Workflow configuration describes intended coverage; only the run for a specific commit establishes that it passed.
