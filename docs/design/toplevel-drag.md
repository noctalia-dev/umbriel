# Toplevel data drags

`ToplevelDragManager` implements `xdg-toplevel-drag-v1` using wlroots' existing data-device drag and xdg-shell objects. The protocol XML comes from the required wayland-protocols package. The manager belongs to `Seat`, and its global is available to ordinary security-context clients.

A protocol object associates a data source with at most one live toplevel. Attachment can precede drag start, mapping, or both. New attached windows receive a floating opening configure and stay outside the layout while carried. An already mapped tile is detached from its layout through the existing `Cursor::MoveGrab` machinery. The cursor uses the protocol's offsets in logical coordinates and the normal drag presentation, which spans outputs without clipping to the window's home output.

The data-device grab remains owned by wlroots. Unlike an ordinary compositor window move, toplevel movement continues into data-device hit testing and motion delivery. `wlr_scene_node_at_excluding` ignores the carried window's whole subtree without changing scene visibility or damage. Insert hints also reject input, so a dwindle or master drop preview cannot intercept the data target. The application receives the ordinary drag events and owns tab merging or panel docking; Umbriel only carries and places the window.

Unmapping detaches the toplevel without ending its data drag. A later attach can carry another window or the remapped role. Unmapping the origin, such as when a browser loses its last tab, must not cancel a different carried window. Destroying the factory leaves its protocol objects alive. Source destruction, role destruction, disconnect, cancellation, and compositor shutdown all remove their listeners and release the move presentation.

Drop placement runs through the normal window-drop path. The protocol object can be destroyed after `dnd_drop_performed`, even if another button keeps wlroots' pointer grab alive. Once dropped, pointer and scene refreshes must not select a new data target: doing so resets the source's accepted state and can reject the recipient's `wl_data_offer.finish` request. Window geometry events report the carried position until normal placement resumes.

## wlroots boundary

wlroots 0.20 has no public conversion from a `wl_data_source` resource to its `wlr_data_source`. Its resource user data points to `wlr_client_data_source`, whose first member is the public data source. The manager uses that address only as an identity when matching seat events; it never reads private tail fields. Recheck this prefix assumption when changing the pinned wlroots series. Other protocol objects use wlroots' public conversion functions.

## Verification

`drag/toplevel_drag` covers attachment order, unmap and reattach, origin removal, all three layouts, underlying drop delivery, rendered carried content, and a drop while another button remains held. `drag/toplevel_drag_outputs` checks rendering on both sides of an output boundary, logical offsets at scale 2, and destination-workspace placement. `drag/toplevel_drag_lifecycle` covers protocol errors and source, role, and client teardown.
