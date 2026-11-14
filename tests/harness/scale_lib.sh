# Helpers shared by the render-scale checks, which read the scale hints a test client logs.

# The Nth line of `log` starting with `prefix`, 1-based, once it has arrived.
nth_hint() { # log, prefix, count
  await_lines "$1" "$2" "$3" 0.05 > /dev/null || {
    echo "no hint #$3 starting with '$2' in $1: $(tr '\n' '|' < "$1")" >&2
    return 1
  }
  grep "^$2" "$1" | sed -n "${3}p"
}

# Fails unless `hint` holds `fragment`.
expect_hint() { # hint, required fragment, failure message
  if [[ $1 != *"$2"* ]]; then
    echo "$3: $1"
    exit 1
  fi
}
