# Scope

Umbriel is a thin compositor layer over [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and `umbrielfx`, its own hard fork of [SceneFX](https://github.com/wlrfx/scenefx). It is built by the people who use it daily. It is young, and the fastest way to make it worse is to grow it in every direction at once. This file records what the project takes on and what it declines, so a request gets a predictable answer instead of a case-by-case one.

Read the project [ethos](https://noctalia.dev/ethos) for the values behind these lines, and [CONTRIBUTING.md](CONTRIBUTING.md) for the technical principles they follow from.

## In scope

- Bugs, crashes, regressions, and protocol misbehavior in anything Umbriel already does.
- Refinement of the surfaces that already exist: the scrolling, dwindle, master, and floating layouts, per-output workspaces, scratchpads, window and layer rules, the overview, input handling, and the existing effects.
- Configuration keys that expose behavior Umbriel already implements, plus IPC actions and inspection output for state it already tracks.
- Wayland protocol support that real applications need, implemented through wlroots.
- Correct behavior on scaled outputs, multiple outputs, hotplug, session locking, and X11 clients through Xwayland.
- Documentation, packaging, and test coverage for all of the above.

## Out of scope

- **New tiling layouts and new layout paradigms.** Scrolling, dwindle, master, and floating are the set. Improving how those four behave is in scope; adding a fifth model, or reshaping one of them into a different one, is not.
- **Desktop shell components.** Bars, panels, launchers, notification daemons, wallpaper handling, widgets, menus, and on-screen displays belong to a shell speaking Wayland protocols, for example [Noctalia](https://github.com/noctalia-dev/noctalia). Umbriel's job is to expose what a shell needs, not to become one.
- **A plugin system, embedded scripting language, or runtime module loading.** Behavior is configured in TOML and driven through `umbriel msg`.
- **Configuration languages other than TOML**, and compatibility aliases, migration readers, or silent fallbacks for renamed keys.
- **Effects that bypass `umbrielfx`.** New visuals land in `umbrielfx/`, never as ad-hoc scene graph hacks.
- **Reimplementing what wlroots provides**, and diverging from its release cadence.
- **Feature parity with another compositor as the motivation.** "Project X has this" describes Project X, not a problem you have.
- **Per-application special cases** that a window rule or layer rule can already express.
- **Anything already reachable** through configuration, window and layer rules, keybinds, submaps, or `umbriel msg`.
- **Threading the event loop.** `Server::spawn` depends on it staying single-threaded.

"Out of scope" is not a verdict on the idea. It means the work does not belong in this project, at this stage, with this maintenance budget.

## How a request is judged

1. **Does it need the compositor?** If a client, a shell, or a script over `umbriel msg` can do it, that is where it belongs.
2. **Does it extend something that exists, or open a new front?** Sharpening current behavior beats adding surface.
3. **Does it fall out of wlroots and `umbrielfx`?** A feature that needs machinery of its own costs far more than its diff.
4. **Who maintains it in a year?** Every option, key, and code path is carried forward, and re-tested on every wlroots bump.

A request that survives all four is worth an issue. One that does not is still worth a conversation, just not a tracking issue.

## Where to raise things

- **Bug or regression:** open a bug report. These are always welcome, including small ones.
- **Idea, question, or "would you consider":** ask on [Discord](https://discord.noctalia.dev) first. It is faster than an issue and it survives being wrong.
- **Large or invasive pull request:** agree on the change before writing it. An unsolicited rewrite can be declined on scope alone, and nobody enjoys that outcome.

This file is not a promise that the answer stays no forever. Scope widens deliberately, when the foundation is stable enough to carry the addition, and it is recorded here when it does.
