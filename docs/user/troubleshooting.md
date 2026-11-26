# Troubleshooting

## Configuration errors

Configuration warnings and errors appear in a panel on the primary output, and `umbriel config validate` checks a file without a running session. See [Diagnostics](configuration.md#diagnostics).

## Screen sharing picker missing after login

Start the session through `start-umbriel` and install the matching `umbriel.service` alongside the compositor binary. The packaged service uses `Type=notify` to hold the graphical session targets until Umbriel is ready; portal services with `After=graphical-session.target` then inherit the published session environment. No portal service override is required for this startup ordering.

If the picker is still missing, inspect the service and portal logs:

```sh
systemctl --user cat umbriel.service
journalctl --user -b \
  -u xdg-desktop-portal.service \
  -u xdg-desktop-portal-umbriel.service \
  -u xdg-desktop-portal-gtk.service \
  --no-pager
```

A portal that was already running before the session startup transaction can retain an earlier environment. Umbriel does not automatically restart portal services. Nested and unmanaged launches do not provide the packaged service's readiness ordering.

## Logs

Umbriel writes its main log to `$XDG_CACHE_HOME/umbriel/umbriel.log`. If `XDG_CACHE_HOME` is unset, the fallback path is `~/.cache/umbriel/umbriel.log`. The previous file is retained as `umbriel.log.1` when the current log reaches 1 MiB.

Messages from wlroots appear with the `[wlr]` tag at info level and above. To include wlroots debug output when reporting a rendering or display problem, start Umbriel with `UMBRIEL_WLR_DEBUG=1` set in its environment.

When standard output or standard error is connected to a TTY, raw writes from Umbriel and its child processes are redirected to `$XDG_CACHE_HOME/umbriel/umbriel-stderr.log`, or `~/.cache/umbriel/umbriel-stderr.log` when `XDG_CACHE_HOME` is unset.

## A launched program does not appear

A `spawn` action, an autostart entry, and the `-s` startup command hand their command to `/bin/sh`, so what they report is the handoff and nothing about the program. Each launch is recorded in Umbriel's own log, naming the command and the process it forked, and the scope unit that owns that process when the session runs applications in their own scopes:

```text
launch requested for 'kitty' (pid 4210, unit app-umbriel-1180-3.scope) on WAYLAND_DISPLAY=wayland-0
launch requested for 'events.lid_close' (pid 4211, unit app-umbriel-1180-4.scope, command 'umbriel msg output-disable eDP-1') on WAYLAND_DISPLAY=wayland-0
```

The command appears in the record when Umbriel launched it on behalf of something else with a name of its own, such as a lid-close event or a scratchpad's `spawn_when_empty`. A session that does not run applications in their own scopes has no unit.

`journalctl --user -u umbriel.service | grep 'launch requested'` lists every launch the compositor performed in a session started through `start-umbriel`: it writes that record from its own process, so it appears whether or not the program does anything. A session started any other way has no journal entry, and its record is in `umbriel.log`.

What the program itself wrote is reachable with the identifier query, including a shell diagnostic for a name that resolves to nothing:

```text
journalctl --user -t umbriel
```

journald resolves the unit from the process that writes, so a program that is still running when it writes is attributed to the scope unit the launch record named: `journalctl --user -u app-umbriel-1180-3.scope`. A program that writes and exits at once, which is what a missing command does, may be recorded with the `umbriel` identifier and no unit, because there is no process left to look up. Such a record is missed by a unit query and still found by `-t umbriel`. A program that writes nothing, or redirects its own diagnostics away, leaves only the launch record. Under a TTY the child's own output also reaches `umbriel-stderr.log`.

## Game menus open as separate windows

Some Wine and Proton builds that run natively on Wayland open context menus as untitled, unparented windows. Window rules apply to them, but they carry no position, so they can open centered or maximized, and hovering on the way to the menu can refocus the game. Run the game through Xwayland instead. For Proton, set this Steam launch option:

```sh
PROTON_ENABLE_WAYLAND=0 PROTON_USE_WAYLAND=0 %command%
```

Restart the game afterward. Its window should then report `xwayland: true` in `umbriel windows --json`.

## Reporting a problem

Report problems on the [issue tracker](https://github.com/noctalia-dev/umbriel/issues) or ask on [Discord](https://discord.noctalia.dev). Include the Umbriel version (`umbriel --version`), your distribution and GPU, the relevant part of `umbriel.log`, and a small reproduction.
