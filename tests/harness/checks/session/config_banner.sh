#!/usr/bin/env bash
# The diagnostics panel is drawn like the other internal panels, with a border that reports severity: the warning colour while the configuration still applied, the error colour once it did not. Colour and geometry are the whole signal, so this samples the two border rows alone rather than the panel interior, where the severity-coloured heading would satisfy the same assertion without any border being drawn. Each state is asserted against the one before it, so a panel stuck on its first severity fails even though every individual colour is present somewhere.
set -euo pipefail

readonly SHOT=$UMBRIEL_RUNTIME_DIR/banner.png
readonly WARNING='#F5C96B'
readonly ERROR='#FF6B6B'
# The banner sits 24 logical pixels below the top edge with a 2px border, so
# these rows are border or nothing.
readonly BORDER_ROWS=1280x6+0+22
readonly MIN_BORDER_PIXELS=200

border_pixels() {
  magick "$SHOT" -crop "$BORDER_ROWS" +repage txt: | grep -c "$1" || true
}

capture() {
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$SHOT"
}

capture
clean_warning=$(border_pixels "$WARNING")
clean_error=$(border_pixels "$ERROR")
if (( clean_warning > 0 || clean_error > 0 )); then
  echo "a banner was drawn for a configuration with no diagnostics: warning=$clean_warning error=$clean_error"
  exit 1
fi

printf '\n[appearance]\nnot_a_real_key = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
capture
warned_warning=$(border_pixels "$WARNING")
warned_error=$(border_pixels "$ERROR")
if (( warned_warning < MIN_BORDER_PIXELS )); then
  echo "an unknown key did not draw a warning-bordered banner: warning=$warned_warning"
  exit 1
fi
if (( warned_error > 0 )); then
  echo "a warning drew the error border: error=$warned_error"
  exit 1
fi

# Both selectors on one workspace rule is an error, so the configuration is
# rejected and the banner has to change severity with it.
printf '\n[[workspace]]\nname = "chat"\nindex = 3\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
capture
failed_error=$(border_pixels "$ERROR")
if (( failed_error < MIN_BORDER_PIXELS )); then
  echo "a rejected configuration did not draw an error-bordered banner: error=$failed_error"
  exit 1
fi
if ! grep -q "config reload failed; keeping previous configuration" "$UMBRIEL_LOG"; then
  echo "the error banner appeared for a configuration that was still applied"
  exit 1
fi

echo "the diagnostics banner draws a bordered panel and its border follows severity"
