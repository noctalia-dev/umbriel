#!/usr/bin/env bash
# A delegated security-context socket keeps normal application protocols, but
# cannot reach compositor-wide capture, input, clipboard, shell, or management
# globals. The wrapper disconnects the client that created the listener before
# running each assertion, which also covers listener lifetime.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/global-client}"
readonly SECURITY_CONTEXT_CLIENT="${UMBRIEL_SECURITY_CONTEXT_CLIENT:-./build-debug/security-context-client}"

if [[ ! -x $GLOBAL_CLIENT || ! -x $SECURITY_CONTEXT_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

readonly -a NORMAL_GLOBALS=(
  wl_compositor
  wl_subcompositor
  wl_shm
  wl_data_device_manager
  wp_viewporter
  wp_fractional_scale_manager_v1
  wp_presentation
  wp_tearing_control_manager_v1
  wp_content_type_manager_v1
  wl_output
  xdg_wm_base
  xdg_toplevel_tag_manager_v1
  zxdg_decoration_manager_v1
  org_kde_kwin_server_decoration_manager
  zwp_relative_pointer_manager_v1
  zwp_pointer_constraints_v1
  zwp_pointer_gestures_v1
  zwp_tablet_manager_v2
  zwp_idle_inhibit_manager_v1
  xdg_activation_v1
  wl_seat
  wp_cursor_shape_manager_v1
  zwp_text_input_manager_v3
)

readonly -a RESTRICTED_GLOBALS=(
  wp_security_context_manager_v1
  zxdg_output_manager_v1
  ext_idle_notifier_v1
  ext_data_control_manager_v1
  zwlr_layer_shell_v1
  ext_session_lock_manager_v1
  zwp_input_method_manager_v2
  zwp_virtual_keyboard_manager_v1
  zwlr_virtual_pointer_manager_v1
  zwlr_screencopy_manager_v1
  zwlr_export_dmabuf_manager_v1
  ext_output_image_capture_source_manager_v1
  ext_foreign_toplevel_image_capture_source_manager_v1
  ext_image_copy_capture_manager_v1
  zwlr_gamma_control_manager_v1
  zwlr_output_manager_v1
  zwlr_foreign_toplevel_manager_v1
  ext_foreign_toplevel_list_v1
  ext_workspace_manager_v1
)

"$GLOBAL_CLIENT" wp_security_context_manager_v1 present 1

for interface in "${NORMAL_GLOBALS[@]}"; do
  "$GLOBAL_CLIENT" "$interface" present
  "$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" "$interface" present
done

for interface in "${RESTRICTED_GLOBALS[@]}"; do
  "$GLOBAL_CLIENT" "$interface" present
  "$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" "$interface" absent
done

echo "security-context clients receive only reviewed normal app globals"
