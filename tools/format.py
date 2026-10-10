#!/usr/bin/env python3
"""Format authored source files with pinned C++, JS/HTML, and Python formatters."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT.name
TOOLS_DIR = Path(
    os.environ.get(
        "FORMAT_TOOLS_DIR",
        str(Path(os.environ.get("TMPDIR", "/tmp")) / f"{PROJECT}-format-tools"),
    )
)
CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
PRETTIER_EXTENSIONS = {".js", ".cjs", ".mjs", ".html"}
BLACK_EXTENSIONS = {".py"}
EXCLUDED_PARTS = {
    ".git",
    "build",
    "node_modules",
    "third_party",
    "vendor",
    "external",
    "dist",
    "coverage",
    "assets",
    "generated",
}


def source_files() -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(ROOT),
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        check=True,
        capture_output=True,
    )
    paths = []
    for raw_path in result.stdout.decode().split("\0"):
        if not raw_path:
            continue
        path = Path(raw_path)
        if (
            any(part in EXCLUDED_PARTS for part in path.parts)
            or not (ROOT / path).is_file()
        ):
            continue
        paths.append(path)
    return sorted(paths)


def run(command: list[str]) -> None:
    subprocess.run(command, cwd="/tmp", check=True)


def require_version(command: list[str], expected: str, label: str) -> None:
    result = subprocess.run(
        command, cwd="/tmp", check=True, capture_output=True, text=True
    )
    actual = result.stdout.strip()
    if actual != expected:
        raise SystemExit(f"{label} version must be {expected}; found {actual!r}.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="rewrite source files")
    mode.add_argument(
        "--check", action="store_true", help="fail if source files need formatting"
    )
    args = parser.parse_args()
    clang_format = TOOLS_DIR / "bin" / "clang-format"
    black = TOOLS_DIR / "bin" / "black"
    if not clang_format.is_file() or not black.is_file():
        raise SystemExit(
            f"Pinned Python formatters are missing from {TOOLS_DIR}. Install them with: bash tools/setup-format-tools.sh"
        )
    prettier = [
        "npx",
        "--yes",
        "--registry=https://registry.npmjs.org",
        "--package=prettier@3.6.2",
        "prettier",
    ]
    try:
        require_version(
            [str(clang_format), "--version"],
            "clang-format version 18.1.8",
            "clang-format",
        )
        require_version(
            [
                str(TOOLS_DIR / "bin" / "python"),
                "-c",
                "import importlib.metadata; print(importlib.metadata.version('black'))",
            ],
            "24.10.0",
            "Black",
        )
        require_version(prettier + ["--version"], "3.6.2", "Prettier")
    except FileNotFoundError as exc:
        raise SystemExit(
            f"Required formatter command is missing: {exc.filename}"
        ) from exc
    all_paths = source_files()
    cpp = [
        str(ROOT / path) for path in all_paths if path.suffix.lower() in CPP_EXTENSIONS
    ]
    js_html = [
        str(ROOT / path)
        for path in all_paths
        if path.suffix.lower() in PRETTIER_EXTENSIONS
    ]
    python = [
        str(ROOT / path)
        for path in all_paths
        if path.suffix.lower() in BLACK_EXTENSIONS
    ]
    formatter_mode = ["--dry-run", "--Werror"] if args.check else ["-i"]
    if cpp:
        run([str(clang_format), *formatter_mode, *cpp])
    if js_html:
        run(prettier + (["--check"] if args.check else ["--write"]) + js_html)
    if python:
        run([str(black), "--check" if args.check else "--quiet", *python])
    return 0


if __name__ == "__main__":
    sys.exit(main())
