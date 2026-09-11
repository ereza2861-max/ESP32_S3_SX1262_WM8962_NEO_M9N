#!/bin/sh
set -eu

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

echo "== Git status =="
git status --short

echo
echo "== Whitespace check =="
git diff --check
git diff --cached --check

echo
echo "== Credential-like tracked paths =="
if git ls-files | grep -E '(^|/)(\.env(\..*)?|credentials/|secrets/|LocalConfig\.h|.*\.(pem|key|p12|pfx|crt|der|jks|keystore))$'; then
  echo "ERROR: credential-like file is tracked."
  exit 1
fi

echo
echo "== Secret-pattern scan =="
PATTERN='(^|[^A-Za-z0-9_])(ghp_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{20,}|-----BEGIN [A-Z ]*PRIVATE KEY-----)'
if git grep -nI -E "$PATTERN" -- .; then
  echo "ERROR: possible secret detected in tracked files."
  exit 1
fi

if git diff --cached --unified=0 | grep -nE "$PATTERN"; then
  echo "ERROR: possible secret detected in staged changes."
  exit 1
fi

echo
echo "== Untracked files that would be committed =="
untracked="$(git ls-files --others --exclude-standard)"
printf '%s\n' "$untracked"
found=0
if [ -n "$untracked" ]; then
  while IFS= read -r file; do
    [ -f "$file" ] || continue
    if grep -nI -E "$PATTERN" "$file"; then
      echo "ERROR: possible secret detected in untracked file: $file"
      found=1
    fi
  done <<EOF
$untracked
EOF
fi
if [ "$found" -ne 0 ]; then
  exit 1
fi

echo
echo "== Staged diff summary =="
git diff --cached --stat

echo
echo "PASS: no obvious credential pattern found."
echo "IMPORTANT: review 'git status' and 'git diff --cached' before commit."
