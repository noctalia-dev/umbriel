# Prints every fixed sleep of 0.2 s or more that sits outside a polling loop and lacks a trailing
# "# real time: <reason>". Animation timing uses `umbriel clock-freeze`/`clock-advance`, end states use
# `umbriel settle`, and client state is polled; only compositor timers, helper-client pauses, and proofs that nothing
# reacts within a window of real time keep a sleep.
# Usage: awk -f sleep-lint.awk checks/*.sh
FNR == 1 { depth = 0; heredoc = "" }
{
  if (heredoc != "") {
    line = $0
    sub(/^\t+/, "", line)
    if (line == heredoc) heredoc = ""
    next
  }
  line = $0
  gsub(/"([^"\\]|\\.)*"/, "\"\"", line)
  gsub(/'[^']*'/, "''", line)
  sub(/(^|[[:space:]])#.*/, "", line)
  if (match(line, /<<-?[[:space:]]*[A-Za-z_]+/)) {
    heredoc = substr(line, RSTART, RLENGTH)
    sub(/^<<-?[[:space:]]*/, "", heredoc)
  } else if (match(line, /<<-?[[:space:]]*''/) || match(line, /<<-?[[:space:]]*""/)) {
    # A quoted delimiter was blanked with the strings; recover it from the original line.
    if (match($0, /<<-?[[:space:]]*['"][A-Za-z_]+['"]/)) {
      heredoc = substr($0, RSTART, RLENGTH)
      sub(/^<<-?[[:space:]]*['"]/, "", heredoc)
      sub(/['"]$/, "", heredoc)
    }
  }
  if (line ~ /(^|[;[:space:]])do([[:space:]]|;|$)/) depth++
  if (line ~ /^[[:space:]]*sleep[[:space:]]+[0-9.]+[[:space:]]*$/ && depth == 0) {
    seconds = line
    sub(/^[[:space:]]*sleep[[:space:]]+/, "", seconds)
    if (seconds + 0 >= 0.2 && $0 !~ /# real time: [^[:space:]]/) printf "%s:%d: %s\n", FILENAME, FNR, $0
  }
  if (line ~ /(^|[;[:space:]])done([[:space:]]|;|$|[<>|&)])/) depth--
}
