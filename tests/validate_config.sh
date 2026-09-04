#!/bin/sh
set -eu

umbriel=$1
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/components/nested"

expect_invalid() {
  config=$1
  expected=$2
  if "$umbriel" validate -c "$config" >"$fixture/stdout" 2>"$fixture/stderr"; then
    printf 'expected validation failure for %s\n' "$config" >&2
    cat "$fixture/stderr" >&2
    exit 1
  fi
  if ! grep -F "$expected" "$fixture/stderr" >/dev/null; then
    printf 'expected diagnostic containing %s\n' "$expected" >&2
    cat "$fixture/stderr" >&2
    exit 1
  fi
  grep -F "configuration invalid" "$fixture/stderr" >/dev/null
}

cat >"$fixture/config.toml" <<'EOF'
[include]
files = ["components/layout.toml"]
EOF
cat >"$fixture/components/layout.toml" <<'EOF'
[overview]
zoom = 900
EOF
expect_invalid "$fixture/config.toml" "$fixture/components/layout.toml:2:8"

cat >"$fixture/components/layout.toml" <<'EOF'
[include]
files = ["nested/broken.toml"]
EOF
cat >"$fixture/components/nested/broken.toml" <<'EOF'
[overview
zoom = 0.5
EOF
expect_invalid "$fixture/config.toml" "$fixture/components/nested/broken.toml:1:10"

cat >"$fixture/config.toml" <<'EOF'
[include]
files = ["components/missing.toml"]
EOF
expect_invalid "$fixture/config.toml" "include not found: $fixture/components/missing.toml"

cat >"$fixture/config.toml" <<'EOF'
[include]
files = ["components/layout.toml"]
EOF
cat >"$fixture/components/layout.toml" <<'EOF'
[include]
files = ["../config.toml"]
EOF
expect_invalid "$fixture/config.toml" "include cycle or duplicate skipped: $fixture/config.toml"

cat >"$fixture/config.toml" <<'EOF'
[include]
files = ["components/layout.toml"]

[layout]
gap = 19
EOF
cat >"$fixture/components/layout.toml" <<'EOF'
[layout]
gap = 7
EOF
"$umbriel" validate -c "$fixture/config.toml" >"$fixture/stdout" 2>"$fixture/stderr"
grep -F "config: ok ($fixture/config.toml)" "$fixture/stdout" >/dev/null
