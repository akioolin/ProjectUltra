#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

fail=0
scanned=0
findings=()

add_finding() {
  local path="$1"
  local type="$2"

  findings+=("$path: $type")
  fail=1
}

# Local AI-tooling config must never be committed (the agents/ autonomous system
# was retired 2026-05-30, so its queue/report artifact checks went with it).
is_local_ai_artifact_path() {
  local path="$1"

  case "$path" in
    .claude/*|.codex|.codex/*)
      return 0
      ;;
  esac

  return 1
}

has_match() {
  local pattern="$1"
  local path="$2"

  LC_ALL=C grep -I -E -q -- "$pattern" "$path"
}

has_fixed_match() {
  local pattern="$1"
  local path="$2"

  LC_ALL=C grep -I -F -q -- "$pattern" "$path"
}

private_key_re='-----BEGIN ((OPENSSH|RSA|DSA|EC|ED25519|ENCRYPTED) )?PRIVATE KEY-----'
putty_private_key_re='^PuTTY-User-Key-File-[0-9]+: ssh-'
github_classic_token_re='gh[pousr]_[[:alnum:]_]{30,}'
github_fine_grained_token_re='github_pat_[[:alnum:]_]{50,}'

while IFS= read -r -d '' path; do
  [[ -f "$path" ]] || continue
  scanned=$((scanned + 1))

  if is_local_ai_artifact_path "$path"; then
    add_finding "$path" "local AI-tooling artifact or metadata"
  fi

  if has_match "$private_key_re" "$path" || has_match "$putty_private_key_re" "$path"; then
    add_finding "$path" "private key"
  fi

  if has_match "$github_classic_token_re" "$path" || has_match "$github_fine_grained_token_re" "$path"; then
    add_finding "$path" "GitHub token"
  fi
done < <(git ls-files --cached --others --exclude-standard -z)

if [[ "$fail" -ne 0 ]]; then
  echo "Artifact/secret check failed. No secret values are printed below." >&2
  printf '%s\n' "${findings[@]}" | sort -u | sed 's/^/  - /' >&2
  exit 1
fi

echo "Artifact/secret check passed ($scanned files scanned)."
