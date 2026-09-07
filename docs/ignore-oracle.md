# Git ignore 差分验证 / Differential verification

规范来源 / Specification: [Git gitignore documentation](https://git-scm.com/docs/gitignore).

`tools/ignore_oracle.py` 使用 25 条规则 × 39 个路径，对照本机 Git 的
`check-ignore --no-index` 和 `same::Ignore`，共 975 项。覆盖锚定、目录、父目录
排除与重新纳入、转义、尾随空格、字符类、不同位置的星号和无效模式。
This 975-case matrix compares the production matcher with installed Git, covering
anchors, directories, excluded parents, reinclusion, escaping, trailing spaces,
classes, star placement, and malformed patterns.

实际目录在临时仓库中创建；传给 Git 的路径不带末尾斜杠，让 Git 查询类型。
不存在的 `foo/` 字符串会产生空路径分量，不能作为目录类型的正确参照。
Real directory fixtures avoid artificial empty-component matching from nonexistent
slash-suffixed path strings. All fixtures are temporary; user repositories are untouched.

## Windows / MSVC

在 x64 Native Tools 命令提示符中先构建 `build/cuda`，然后执行：
Build `build/cuda` first, then run from an x64 Native Tools command prompt:

```bat
mkdir .cache\ignore-oracle
cl /nologo /std:c++20 /EHsc /MD /I include tools\ignore_oracle_probe.cpp build\cuda\same_core.lib /Fe:.cache\ignore-oracle\probe.exe /Fo:.cache\ignore-oracle\probe.obj
python tools/ignore_oracle.py --probe .cache/ignore-oracle/probe.exe
```

## Linux / GCC

使用已构建的、未启用 sanitizer 的 CMake 目录，例如 `build/linux`：
Use an existing non-sanitized CMake build directory, for example `build/linux`:

```sh
mkdir -p .cache/ignore-oracle
g++ -std=c++20 -I include tools/ignore_oracle_probe.cpp build/linux/libsame_core.a -pthread -o .cache/ignore-oracle/probe
python3 tools/ignore_oracle.py --probe .cache/ignore-oracle/probe
```

若链接 sanitizer 构建，须追加同样的编译器 sanitizer 链接选项。
When linking a sanitized build, supply its matching sanitizer linker flags.

预期输出 / Expected: `975 cases; 0 mismatches`.

这不是 Git 所有字节模式的形式化等价证明；主测试 `config_tests` 还覆盖 BOM、
输入大小和规则数限制。仅 `.same/ignore` 使用该语法，并不继承 Git 全局规则或
嵌套 `.gitignore`。`.same` 永远排除是产品的额外安全约束，不属于此 Git 对照矩阵。
This is not a formal equivalence proof for all byte patterns. Unit tests additionally
cover BOM and resource bounds. Only `.same/ignore` is read, without Git global or
nested rules. Always excluding `.same` is a separate product safety invariant.
