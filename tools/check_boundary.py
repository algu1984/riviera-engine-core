#!/usr/bin/env python3
"""Check the authored core library's public dependency boundary."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SCAN_ROOTS = (ROOT / "include", ROOT / "src")
FORBIDDEN = re.compile(
    r"__EMSCRIPTEN__|\bEMSCRIPTEN\b|\bEM_ASM\b|\bEM_JS\b|"
    r"\bWebGPU\b|\bWGPU\b|\bJolt\b|\bwebgpu\b|\bbrowser\b",
    re.IGNORECASE,
)
BLOCKING_SYNC = re.compile(
    r"#\s*include\s*[<\"](?:mutex|shared_mutex|condition_variable|semaphore|thread)\b|"
    r"std::(?:mutex|shared_mutex|condition_variable|counting_semaphore|binary_semaphore|"
    r"lock_guard|unique_lock|scoped_lock|thread|jthread)|"
    r"(?:atomic_wait|atomic_notify_one|atomic_notify_all)\s*\(|\bspinlock\b",
    re.IGNORECASE,
)
INCLUDE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]", re.MULTILINE)


def allowed_include(source: Path, delimiter: str, name: str) -> bool:
    if delimiter == "<":
        if "/" not in name:
            return True  # Standard library header.
        target = ROOT / "third_party" / "xenium" / name
        return name.startswith("xenium/") and target.is_file()
    if name.startswith("engine/"):
        target = ROOT / "include" / name
    elif name.startswith("../"):
        target = (source.parent / name).resolve()
    else:
        return False
    try:
        target.relative_to(ROOT / "include")
    except ValueError:
        return False
    return target.is_file()


def main() -> int:
    failures = []
    files = [
        path
        for root in SCAN_ROOTS
        for path in root.rglob("*")
        if path.suffix in {".h", ".hpp", ".cc", ".cpp", ".cxx"}
    ]
    for path in sorted(files):
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(ROOT)
        for matcher, label in (
            (FORBIDDEN, "platform-specific dependency"),
            (BLOCKING_SYNC, "blocking synchronization"),
        ):
            for match in matcher.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(f"{rel}:{line}: {label}: {match.group(0)}")
        for match in INCLUDE.finditer(text):
            delimiter, name = match.groups()
            if not allowed_include(path, delimiter, name):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{rel}:{line}: include outside standard library, core SDK, or Xenium: {name}"
                )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PASS core boundary audit ({len(files)} authored headers/sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
