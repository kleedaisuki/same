# 构建、测试与发布 / Build, test and release

`Release` is a compiler optimization mode; **GitHub Releases** is the downloadable product distribution. They are now connected by `.github/workflows/release.yml`.
编译优化模式与 GitHub 下载发布不是一回事；本流程将两者串联。

## Gates / 验收门

1. Every push and pull request runs the reusable cross-platform tests and five package builds.
2. CUDA package builds set both `SAME_ENABLE_CUDA=ON` and `SAME_REQUIRE_CUDA=ON`; missing CUDA is a failure, never a mislabeled CPU package.
3. Each package runs the full test suite, records its commit/configuration, then smoke-tests the staged and extracted executable.
4. Only a `vX.Y.Z` tag matching CMake's version and reachable from `main` may publish. All validation/package jobs must succeed.
5. Release assets are uploaded into a draft, attested, then made public; branch artifacts are not public Releases.

## Product packages / 产品包

| Package | Backends | Requirements |
|---|---|---|
| Windows x64 standard | CPU + optional iGPU | Windows 10/11 x64; iGPU needs OpenCL vendor driver |
| Windows x64 cuda | CPU + CUDA + optional iGPU | Compatible NVIDIA driver; CUDA 12.8 runtime linked statically |
| Linux x64 standard | CPU + optional iGPU | Ubuntu 24.04 / glibc 2.39+ and compatible libstdc++ |
| Linux x64 cuda | CPU + CUDA + optional iGPU | Same Linux baseline plus compatible NVIDIA driver |
| macOS ARM64 standard | CPU + optional iGPU | macOS 14+ Apple Silicon; no CUDA; not notarized |

No GPU is necessary for standard scanning. OpenCL is loaded optionally at runtime. CUDA binaries contain kernels for SM 75/80/86/89/90 and forward-compatible PTX for the newest target; older GPUs fall back to CPU when unsupported.
标准扫描不要求 GPU；设备支持与编译支持不同，托管 CI 不替代真实 GPU 验证。

## Maintainer sequence / 维护流程

Update CMake version, site text and `docs/releases/vX.Y.Z.md`; open a PR, wait for all checks, and merge into main. Then tag that verified main commit and push the tag. The Actions workflow publishes assets only after tag validation succeeds. Do not move a published tag or replace public release binaries in place.

```sh
git tag -a vX.Y.Z -m "same vX.Y.Z"
git push origin vX.Y.Z
```

GitHub automatically provides the source archive for the tag. Dependencies have pinned versions/checksums in `cmake/Dependencies.cmake`; package license files accompany the executable. Build provenance can be checked with `gh attestation verify <archive> --repo kleedaisuki/same`; `SHA256SUMS` checks download integrity, not publisher identity by itself.
