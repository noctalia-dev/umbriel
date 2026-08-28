#!/usr/bin/env bash
# Sync docs/user/*.md into ../noctalia-docs/src/content/docs/umbriel/ as .mdx.
# docs/design/ holds maintainer notes and is never synced. Existing .mdx files
# keep their hand-written frontmatter (title, description); only the body is
# refreshed from the source .md. New files get a title derived from their first
# H1. A leading H1 matching the frontmatter title is dropped from the body,
# mirroring the convention in the docs site. index.mdx is hand-authored in the
# docs site (it imports Astro components), so a future docs/user/index.md is
# ignored rather than synced over it. Source docs use relative .md links so
# they render correctly on GitHub. The docs site serves pages at
# /umbriel/<route>/, so links are rewritten here: sibling docs become site
# URLs, and the packaged config example is copied as a static asset. A small
# explicit map handles source filenames whose established site route differs.
# Design notes (docs/design) are maintainer-only and are never synced, so links
# to them must not appear in user docs. Any .md link that survives the rewrite
# is reported, since it would 404 on the site. After syncing, rebuild the site
# (npm run build) before previewing: astro preview serves the prebuilt dist/
# and does not pick up new .mdx. Usage: tools/sync-docs.sh [docs-site-root]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
site_root="${1:-"$repo_root/../noctalia-docs"}"
dest_dir="$site_root/src/content/docs/umbriel"
asset_dir="$site_root/public/umbriel"

mkdir -p "$dest_dir"
mkdir -p "$asset_dir"

site_route() {
    case "$1" in
        scratchpad) printf '%s' 'scratchpads' ;;
        *) printf '%s' "$1" ;;
    esac
}

# Build the link rewrite table from the actual doc inventory. User docs are copied to the site at /umbriel/<name>/. Design notes stay maintainer-only in the repo
# and must not be linked from user docs; the leftover check below flags any that slip in.
link_exprs=()
for md in "$repo_root"/docs/user/*.md; do
    [[ -e "$md" ]] || continue
    base="$(basename "$md" .md)"
    [[ "$base" == "index" ]] && continue
    route="$(site_route "$base")"
    link_exprs+=(-e "s|]($base.md)|](/umbriel/$route/)|g")
    link_exprs+=(-e "s|]($base.md#|](/umbriel/$route/#|g")
done

cp "$repo_root/examples/config.toml" "$asset_dir/config.toml"
link_exprs+=(-e 's|](../../examples/config.toml)|](/umbriel/config.toml)|g')

for md in "$repo_root"/docs/*.md; do
    [[ -e "$md" ]] || continue
    printf 'sync-docs: warning: %s is not under docs/user/, not synced\n' "$(basename "$md")" >&2
done

for md in "$repo_root"/docs/user/*.md; do
    [[ -e "$md" ]] || continue
    base="$(basename "$md" .md)"
    [[ "$base" == "index" ]] && continue
    route="$(site_route "$base")"
    mdx="$dest_dir/$route.mdx"

    h1="$(awk '/^# / { sub(/^# /, ""); print; exit }' "$md")"

    frontmatter=""
    if [[ -f "$mdx" ]]; then
        frontmatter="$(awk 'NR == 1 && $0 == "---" { fm = 1; print; next } fm && $0 == "---" { print; exit } fm { print }' "$mdx")"
    fi

    title=""
    if [[ -n "$frontmatter" ]]; then
        title="$(printf '%s\n' "$frontmatter" | awk '/^title: / { sub(/^title: /, ""); print; exit }')"
    fi
    if [[ -z "$title" ]]; then
        title="$h1"
        frontmatter="---
title: $title
---"
    fi
    if [[ -z "$title" ]]; then
        printf 'sync-docs: no title in %s, skipping\n' "$md" >&2
        continue
    fi

    {
        printf '%s\n' "$frontmatter"
        printf '\n'
        awk -v title="$title" '
            NR == 1 && $0 == "---" { fm = 1; next }
            fm == 1 && $0 == "---" { fm = 0; next }
            fm == 1 { next }
            seen == 0 && $0 == ("# " title) { seen = 1; dropped = 1; next }
            dropped == 1 && /^$/ { dropped = 0; seen = 1; next }
            { seen = 1; print }
        ' "$md" | sed "${link_exprs[@]}"
    } > "$mdx.tmp"

    leftover="$(grep -Eo '\]\([^)]*\.md[^)]*\)' "$mdx.tmp" | grep -Ev '\]\(https?://' || true)"
    if [[ -n "$leftover" ]]; then
        printf 'sync-docs: warning: %s has unsynced .md links:\n' "$base.md" >&2
        sed 's/^/  /' <<<"$leftover" >&2
    fi

    mv "$mdx.tmp" "$mdx"
    printf 'synced %s -> %s\n' "$base.md" "${mdx#"$site_root"/}"
done
