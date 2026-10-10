# Repository instructions

After editing authored source code, run `make format`; before completing work,
run `make format-check`. Formatting uses the pinned tools documented in
`FORMATTING.md` and installed with `bash tools/setup-format-tools.sh`.

Formatter scope includes authored C++, JavaScript, CommonJS, HTML, and Python.
Never format vendored or generated content, including `third_party/`, `vendor/`,
`build/`, dependency directories, or generated assets.

WGSL embedded in C++ raw strings must remain readable with four-space indentation.
clang-format does not format WGSL inside raw strings, so preserve that layout manually.

JavaScript embedded in Emscripten macros such as `EM_ASM` and `EM_JS` must retain
valid JavaScript syntax; `.clang-format` keeps these macro bodies whitespace-sensitive.
