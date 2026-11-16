# Global shortcut routing

The compositor owns shortcut resolution, durable approval, and input delivery. The private `umbriel_global_shortcuts_manager_v1` bridge accepts session, restore, bind, and list operations. It is withheld from security-context clients by the default global filter.

A session can restore a compositor-issued credential scoped to its application and approved shortcut IDs. Restoration rotates the credential before reporting success. A claimed application name alone grants no authority. New IDs require the compositor's paginated approval panel. Each page requires a fresh physical Enter; virtual keyboards and emulated keys cannot advance or approve. Only the final page approves the complete request.

The approval layout measures complete pages with the text renderer before allocating their buffers. Each page includes the application label and review controls within the available panel dimensions. Its contiguous content ranges preserve every shortcut ID, description, candidate, and conflict reason, with breaks only at grapheme boundaries. Control and formatting characters in requester text are replaced before layout; markup is escaped. Only the final page carries the approval action. Reflow creates a new layout starting at the first content range.

The panel uses the preferred output's usable area. Geometry, scale, or output changes restart page review. If there is no usable output or the panel cannot render, the request fails. Another modal, active input capture, locking, inactivity, and shutdown also fail approval without queuing a prompt or remembering a denial. A non-modifier physical key other than Enter, or a pointer click, cancels. Configuration reload and client cancellation invalidate pending review before a later Enter can issue a credential.

Only successful live bindings participate in matching. Configured default-submap keybinds, Ctrl+Alt+Escape, and VT-switch chords are reserved. Live ownership follows successful bind order, with request order breaking ties. Closing an owner promotes approved waiters and publishes complete replacement snapshots, including empty snapshots.

Every keyboard event is observed before capture or confirmation can return. Matching happens after configured press and release bindings. The configured bindings and global shortcuts share keysym matching, including the layout fallback and removal of Caps Lock and Num Lock masks. Input-method-owned keyboard echoes cannot activate a second shortcut.

An activation belongs to a keyboard source and keycode. Releasing the trigger key retires it exactly once with the original event timestamp. Repeated presses cannot create another activation. Retiring an activation preserves consumption until release, so a changed binding or promoted waiter cannot steal an already-held key.

Capture activation, modal opening, lock preparation, VT/session deactivation, and shutdown retire active shortcuts. Source removal retires that source's activations. Changed bindings deactivate before replacement snapshots are published. Inhibition and submaps suppress new activation while releases still retire existing activation. Locking, inactivity, stopping, and both keyboard and pointer capture suppress matching.

The live routing check creates credentials through the production store API in its private state directory. This does not add an approval bypass to the compositor. The private store and bridge are also covered by unit tests and real Wayland connection tests. Run the live checks with `just check input/global_shortcuts` and `just check-stress input/global_shortcuts`. `bash tests/global_shortcuts.sh build-debug` uses the isolated compositor's backend-boundary devices to verify handoff to keyboard and pointer-only capture.
