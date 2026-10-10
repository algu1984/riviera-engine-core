# Formatting

The repository uses clang-format 18.1.8 for C++, Prettier 3.6.2 for JavaScript,
CommonJS, and HTML, and Black 24.10.0 for Python. The setup script installs the
Python tools in `/tmp/<repository-name>-format-tools` (or `FORMAT_TOOLS_DIR`)
and runs Prettier through npm without adding dependencies to the repository.
Node.js and npm must be installed.

Install the pinned tools once with:

```sh
bash tools/setup-format-tools.sh
```

Then format or check all authored source files with:

```sh
make format
make format-check
```

The scanner includes tracked and untracked source files, while excluding build
output, dependencies, and vendored directories such as `third_party/` and
`vendor/`. Run `make format` after editing authored code and `make format-check`
before submitting changes. Do not reformat vendored files.

WGSL embedded in C++ raw strings must remain readable with four-space indentation.
clang-format formats the surrounding C++, but not WGSL inside raw strings; preserve
that shader layout manually and keep statements and struct fields on separate lines.

JavaScript embedded in Emscripten macros such as `EM_ASM` and `EM_JS` must retain
valid JavaScript syntax. The `.clang-format` configuration treats these macro bodies
as whitespace-sensitive so C++ formatting does not rewrite JavaScript operators.
