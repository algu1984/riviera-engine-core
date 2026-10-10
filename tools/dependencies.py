#!/usr/bin/env python3
"""Initialize pinned in-repository dependency submodules and apply local patches."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile


class DependencyError(RuntimeError):
    pass


def run(args: list[str], cwd: Path, *, capture: bool = True) -> str:
    result = subprocess.run(
        args,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        check=False,
    )
    if result.returncode:
        detail = (result.stderr or result.stdout or "").strip()
        raise DependencyError(f"{' '.join(args)} failed: {detail}")
    return result.stdout.rstrip("\n") if capture else ""


def load(path: Path) -> list[dict[str, object]]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DependencyError(f"cannot read lock file {path}: {exc}") from exc
    deps = doc.get("dependencies") if isinstance(doc, dict) else None
    if not isinstance(deps, list):
        raise DependencyError(f"{path}: expected a top-level dependencies array")
    for dep in deps:
        if (
            not isinstance(dep, dict)
            or not isinstance(dep.get("name"), str)
            or not isinstance(dep.get("path"), str)
        ):
            raise DependencyError(
                f"{path}: each dependency needs a name and submodule path"
            )
        if "repository" in dep and not isinstance(dep["repository"], str):
            raise DependencyError(f"{path}: repository owner path must be a string")
        patches = dep.get("patches", [])
        if not isinstance(patches, list) or not all(
            isinstance(p, str) for p in patches
        ):
            raise DependencyError(f"{path}: patches must be an array of paths")
    return deps


def gitlink_revision(owner: Path, submodule: str) -> str:
    entries = run(["git", "ls-files", "--stage", "--", submodule], owner).splitlines()
    for entry in entries:
        fields = entry.split()
        if len(fields) >= 4 and fields[0] == "160000" and fields[3] == submodule:
            return fields[1]
    raise DependencyError(
        f"{submodule}: no gitlink is recorded in the repository index"
    )


def expected_patch_tree(
    repository: Path, revision: str, patches: list[Path], relative_files: set[str]
) -> dict[str, bytes]:
    """Apply declared patches to a clean archive of the pin and return expected file bytes."""
    if not patches or not relative_files:
        return {}
    with tempfile.TemporaryDirectory(prefix="riviera-deps-") as temporary:
        temp = Path(temporary)
        tracked = set(
            run(
                ["git", "ls-tree", "-r", "--name-only", revision], repository
            ).splitlines()
        )
        existing_files = sorted(relative_files & tracked)
        if existing_files:
            archive = subprocess.run(
                ["git", "archive", revision, "--", *existing_files],
                cwd=repository,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if archive.returncode:
                raise DependencyError(
                    f"cannot archive dependency pin: {archive.stderr.decode(errors='replace').strip()}"
                )
            with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
                tar.extractall(temp, filter="data")
        for patch in patches:
            run(["git", "apply", str(patch)], temp)
        return {
            name: (temp / name).read_bytes()
            for name in relative_files
            if (temp / name).is_file()
        }


def check_one(
    dep: dict[str, object], repository: Path, lock_dir: Path, check: bool
) -> None:
    name, submodule = str(dep["name"]), str(dep["path"])
    owner_relative = str(dep.get("repository", "."))
    owner = (repository / owner_relative).resolve()
    try:
        owner.relative_to(repository.resolve())
    except ValueError as exc:
        raise DependencyError(
            f"{name}: submodule owner escapes repository: {owner_relative}"
        ) from exc
    if not owner.is_dir():
        raise DependencyError(
            f"{name}: submodule owner is missing at {owner_relative}; check dependency order"
        )
    revision = gitlink_revision(owner, submodule)
    path = (owner / submodule).resolve()
    try:
        path.relative_to(owner)
    except ValueError as exc:
        raise DependencyError(
            f"{name}: submodule path escapes its owner: {submodule}"
        ) from exc
    if not path.exists() or not (path / ".git").exists():
        if check:
            raise DependencyError(
                f"{name}: submodule missing at {submodule}; run `make deps`"
            )
        run(
            ["git", "submodule", "update", "--init", "--", submodule],
            owner,
            capture=False,
        )
    head = run(["git", "rev-parse", "HEAD"], path)
    if head != revision:
        raise DependencyError(
            f"{name}: submodule HEAD {head} differs from recorded gitlink {revision}"
        )
    patches = [(lock_dir / str(item)).resolve() for item in dep.get("patches", [])]
    for patch in patches:
        try:
            patch.relative_to(lock_dir.resolve())
        except ValueError as exc:
            raise DependencyError(
                f"{name}: patch path escapes dependencies directory: {patch}"
            ) from exc
        if not patch.is_file():
            raise DependencyError(f"{name}: patch file not found: {patch}")
    patch_files: dict[Path, set[str]] = {}
    for patch in patches:
        rows = run(["git", "apply", "--numstat", str(patch)], path)
        patch_files[patch] = {row.split("\t", 2)[-1] for row in rows.splitlines()}
    expected_files = set().union(*patch_files.values()) if patch_files else set()
    status_args = [
        "git",
        "status",
        "--porcelain",
        "--untracked-files=all",
        "--ignore-submodules=dirty",
    ]
    if not patches and run(status_args, path):
        raise DependencyError(f"{name}: unexpected edits in submodule {submodule}")
    previously_applied: set[str] = set()
    for patch in patches:
        reverse = subprocess.run(
            ["git", "apply", "--reverse", "--check", str(patch)],
            cwd=path,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if reverse.returncode:
            if check:
                raise DependencyError(
                    f"{name}: required patch is not applied: {patch.name}"
                )
            status = run(status_args, path)
            for row in status.splitlines():
                if (
                    len(row) < 4
                    or row[:2] not in {" M", "??"}
                    or row[3:] not in previously_applied
                ):
                    raise DependencyError(
                        f"{name}: checkout has unrelated edits; refusing to apply {patch.name}: {row}"
                    )
            run(["git", "apply", str(patch)], path)
        previously_applied.update(patch_files[patch])

    expected = expected_patch_tree(path, revision, patches, expected_files)
    for relative, content in expected.items():
        if (path / relative).read_bytes() != content:
            raise DependencyError(
                f"{name}: {relative} differs from the declared pin plus patch set"
            )
    for relative in expected_files - expected.keys():
        if (path / relative).exists():
            raise DependencyError(
                f"{name}: {relative} should be removed by the declared patch set"
            )
    status = run(status_args, path)
    untracked: set[str] = set()
    for row in status.splitlines():
        if len(row) >= 4 and row[:2] == "??" and row[3:] in expected_files:
            untracked.add(row[3:])
            continue
        if len(row) < 4 or row[:2] not in {" M", " D"} or row[3:] not in expected_files:
            raise DependencyError(
                f"{name}: unexpected staged/untracked/edit in {submodule}: {row}"
            )
    changed = set(
        filter(
            None,
            run(
                ["git", "diff", "--name-only", "HEAD", "--ignore-submodules=dirty"],
                path,
            ).splitlines(),
        )
    )
    if changed != expected_files - untracked:
        raise DependencyError(
            f"{name}: changed paths do not match declared patches in {submodule}: {changed}"
        )
    print(f"{'OK' if check else 'Ready'} {name}: {submodule} @ {revision}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify offline without changing submodules",
    )
    args = parser.parse_args()
    repository = args.repository.resolve()
    lock = args.manifest.resolve()
    try:
        for dep in load(lock):
            check_one(dep, repository, lock.parent, args.check)
    except (DependencyError, OSError, tarfile.TarError) as exc:
        print(f"dependency setup: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
