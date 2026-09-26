#!/bin/sh
set -eu

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

# Intentionally do not print matching lines: a preflight must never echo the
# secret it is trying to protect.
PATTERN='(^|[^A-Za-z0-9_])(ghp_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}|gho_[A-Za-z0-9_]{20,}|ghu_[A-Za-z0-9_]{20,}|ghs_[A-Za-z0-9_]{20,}|ghr_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{20,}|-----BEGIN [A-Z ]*PRIVATE KEY-----)'
CREDENTIAL_PATHS='(^|/)(\.env(\..*)?|credentials/|secrets/|LocalConfig\.h|.*\.(pem|key|p12|pfx|crt|der|jks|keystore))$'

fail=0

echo "== Git status =="
git status --short

echo
echo "== Whitespace check =="
if ! git diff --check || ! git diff --cached --check; then
  fail=1
fi

echo
echo "== Merge-conflict marker check: tracked worktree =="
if git grep -lI -E '^(<<<<<<<|=======|>>>>>>>)($|[[:space:]])' -- . >/dev/null 2>&1; then
  echo "ERROR: merge-conflict marker detected in tracked text (matching content suppressed)."
  fail=1
else
  echo "PASS"
fi

echo
echo "== Credential-like tracked paths =="
tracked_bad="$(git ls-files | grep -E "$CREDENTIAL_PATHS" || true)"
if [ -n "$tracked_bad" ]; then
  printf '%s\n' "$tracked_bad"
  echo "ERROR: credential-like file is tracked."
  fail=1
else
  echo "PASS"
fi

echo
echo "== Secret-pattern scan: tracked worktree =="
if git grep -lI -E "$PATTERN" -- . >/dev/null 2>&1; then
  echo "ERROR: possible secret detected in tracked files (content intentionally suppressed)."
  git grep -lI -E "$PATTERN" -- . || true
  fail=1
else
  echo "PASS"
fi

echo
echo "== Secret-pattern scan: staged changes =="
staged_bad="$(git diff --cached -G "$PATTERN" --name-only || true)"
if [ -n "$staged_bad" ]; then
  printf '%s\n' "$staged_bad"
  echo "ERROR: possible secret detected in staged changes (content intentionally suppressed)."
  fail=1
else
  echo "PASS"
fi

echo
echo "== Untracked files that would be committed =="
untracked="$(git ls-files --others --exclude-standard)"
printf '%s\n' "${untracked:-<none>}"
if [ -n "$untracked" ]; then
  while IFS= read -r file; do
    [ -f "$file" ] || continue
    if grep -lI -E "$PATTERN" "$file" >/dev/null 2>&1; then
      echo "ERROR: possible secret detected in untracked file (content intentionally suppressed): $file"
      fail=1
    fi
    if grep -lE '^(<<<<<<<|=======|>>>>>>>)($|[[:space:]])' "$file" >/dev/null 2>&1; then
      echo "ERROR: merge-conflict marker detected in untracked text (matching content suppressed): $file"
      fail=1
    fi
  done <<EOF
$untracked
EOF
fi

echo
echo "== Local credential file check =="
if [ -e include/LocalConfig.h ]; then
  if git check-ignore -q include/LocalConfig.h; then
    echo "PASS: include/LocalConfig.h exists but is ignored and will not be committed."
  else
    echo "ERROR: include/LocalConfig.h exists but is not ignored."
    fail=1
  fi
else
  echo "PASS: include/LocalConfig.h absent"
fi

echo
echo "== Generated-file ignore check =="
for path in .pio/ sdkconfig sdkconfig.old; do
  if git check-ignore -q --no-index "$path"; then
    echo "PASS: $path is ignored"
  else
    echo "ERROR: $path is not ignored"
    fail=1
  fi
done

echo
echo "== Push-history check =="
# If an upstream exists, inspect commits that would be pushed. Otherwise inspect all commits reachable from HEAD. This is important for an
# initial push because the remote will receive the complete reachable history.
base=""
if upstream="$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null)"; then
  base="$upstream"
fi
if [ -n "$base" ]; then
  commits="$(git rev-list "$base"..HEAD)"
else
  commits="$(git rev-list HEAD)"
fi

if [ -n "$commits" ]; then
  for commit in $commits; do
    bad="$(git grep -lI -E "$PATTERN" "$commit" -- . 2>/dev/null || true)"
    if [ -n "$bad" ]; then
      echo "ERROR: possible secret in commit $commit (content intentionally suppressed)."
      printf '%s\n' "$bad"
      fail=1
    fi
  done
else
  echo "No commits in push range."
fi

echo
echo "== Git remote credential check =="
remote_bad=0
while IFS= read -r remote_url; do
  [ -n "$remote_url" ] || continue
  if printf '%s' "$remote_url" | grep -Eq "$PATTERN|https?://[^/@[:space:]]+:[^/@[:space:]]+@"; then
    remote_bad=1
    break
  fi
done <<EOF
$(git config --get-regexp '^remote\\..*\\.url$' 2>/dev/null | sed 's/^[^ ]*[[:space:]]*//')
EOF
if [ "$remote_bad" -ne 0 ]; then
  echo "ERROR: a Git remote appears to contain an embedded credential/token."
  echo "       Use an SSH remote or GitHub CLI/credential manager instead."
  fail=1
else
  echo "PASS"
fi

echo
echo "== Staged diff summary =="
git diff --cached --stat

if [ "$fail" -ne 0 ]; then
  echo
  echo "FAIL: preflight menemukan potensi masalah. Tidak ada secret content yang dicetak."
  exit 1
fi

echo
echo "PASS: preflight selesai; tidak ada pola credential/secret yang terdeteksi."
echo "IMPORTANT: preflight tidak dapat menjamin bahwa secret yang sudah ada di Git history"
echo "           tidak pernah tersimpan di remote. Jika pernah ter-commit, lakukan secret rotation"
echo "           dan history cleanup sebelum push."
