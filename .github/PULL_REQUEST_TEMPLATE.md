<!-- A bot checks this description and converts the pull request back to a draft if
     required structure is missing. It never closes the pull request.

     Required: the Summary, Motivation, Type of Change, Testing, and Checklist
     headings, and the Checklist wording below. Before marking a pull request ready for
     review, select at least one change type and check every Checklist item.

     Everything else may be deleted, including these guidance comments and any of the
     Related Issue, Manual Coverage, Screenshots / Videos, and Additional Notes sections.
     In Type of Change, keep only the lines that apply.

     An explanation does not replace a required check. If a required statement is not
     true yet, keep the pull request as Draft. -->

## Summary

<!-- What changed and why? -->

## Motivation

<!-- What problem does this solve? -->

## Type of Change

<!-- Mark all that apply. -->

- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change
- [ ] Refactoring
- [ ] Build / packaging
- [ ] Documentation

## Related Issue

<!-- Example: Closes #123 -->

## Testing

<!-- List commands run and any manual testing. If not run, say why. -->

## Manual Coverage

<!-- Mark what applies to this PR. -->

- [ ] Tested in a nested Umbriel session
- [ ] Tested in a native Umbriel session
- [ ] Tested with multiple monitors
- [ ] Tested with a scaled output
- [ ] Tested with native Wayland applications
- [ ] Tested with X11 applications through xwayland-satellite
- [ ] Tested with the scrolling layout
- [ ] Tested with the dwindle layout

## Screenshots / Videos

<!-- Include screenshots or videos for visual, animation, or layout changes. -->

## Checklist

<!-- Before marking the pull request ready for review, check every item below. -->

- [ ] This PR is ready for review, or it is marked as Draft.
- [ ] This change fits `SCOPE.md`, or its scope was agreed in an issue or on Discord first.
- [ ] I read and followed the relevant guidance in `CONTRIBUTING.md`.
- [ ] I ran `just format`, or this PR has no C++ changes.
- [ ] I ran the relevant build, test, lint, or verification commands, or explained why they were not run.
- [ ] I functionally verified compositor behavior where automated checks are insufficient.
- [ ] I self-reviewed the changes.
- [ ] I checked for new warnings or errors.
- [ ] I updated `docs/` and `examples/config.toml`, or this PR does not change user-facing configuration or behavior.
- [ ] I used canonical names for config keys, IPC actions, paths, and identifiers.

## Additional Notes

<!-- Add follow-up notes, reviewer context, or known limitations. -->
