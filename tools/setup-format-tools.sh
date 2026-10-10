#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_name="$(basename "$repo_root")"
tools_dir="${FORMAT_TOOLS_DIR:-${TMPDIR:-/tmp}/${repo_name}-format-tools}"
cd /tmp
python3 -m venv "$tools_dir"
"$tools_dir/bin/python" -m pip install --disable-pip-version-check --index-url https://pypi.org/simple \
    'clang-format==18.1.8' \
    'black==24.10.0'
if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
    echo 'Node.js and npm are required for Prettier 3.6.2.' >&2
    exit 1
fi
npx --yes --registry=https://registry.npmjs.org --package=prettier@3.6.2 prettier --version
printf '%s\n' 'Formatter tools installed. Run make format or make format-check.'
printf 'Python formatter directory: %s\n' "$tools_dir"
