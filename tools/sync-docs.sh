#!/usr/bin/env bash
# Sync docs/user/*.md into ../noctalia-docs/src/content/docs/umbriel/ as .mdx.
# Existing .mdx files keep their hand-written frontmatter (title, description);
# only the body is refreshed. New files get a title from their first H1, and a
# leading H1 is dropped from the body. Stale .mdx files without a source are
# removed after a successful sync.
# Source docs use relative .md links so they render on GitHub; the site serves pages at /umbriel/<route>/, so sibling
# links are rewritten here, and the packaged configuration links to its repository version. A small explicit map
# handles filenames whose site route differs. docs/design/ is maintainer-only, never synced, and must not be linked
# from user docs; any .md link that survives the rewrite is reported, since it would 404.
# After syncing, rebuild the site (npm run build) before previewing: astro preview serves the prebuilt dist/.
# Usage: tools/sync-docs.sh [docs-site-root]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
site_root="${1:-"$repo_root/../noctalia-docs"}"
dest_dir="$site_root/src/content/docs/umbriel"

mkdir -p "$dest_dir"
rm -f "$site_root/public/umbriel/config.toml"

site_route() {
    case "$1" in
        scratchpad) printf '%s' 'scratchpads' ;;
        *) printf '%s' "$1" ;;
    esac
}

# Build the link rewrite table from the actual doc inventory. User docs are copied to the site at /umbriel/<name>/. Design notes stay maintainer-only in the repo
# and must not be linked from user docs; the leftover check below flags any that slip in.
link_exprs=()
declare -A expected_mdx=()
for md in "$repo_root"/docs/user/*.md; do
    [[ -e "$md" ]] || continue
    base="$(basename "$md" .md)"
    route="$(site_route "$base")"
    expected_mdx["$dest_dir/$route.mdx"]=1
    link_exprs+=(-e "s|]($base.md)|](/umbriel/$route/)|g")
    link_exprs+=(-e "s|]($base.md#|](/umbriel/$route/#|g")
done


for md in "$repo_root"/docs/*.md; do
    [[ -e "$md" ]] || continue
    printf 'sync-docs: warning: %s is not under docs/user/, not synced\n' "$(basename "$md")" >&2
done

for md in "$repo_root"/docs/user/*.md; do
    [[ -e "$md" ]] || continue
    base="$(basename "$md" .md)"
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
            seen == 0 && /^# / { seen = 1; dropped = 1; next }
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

for mdx in "$dest_dir"/*.mdx; do
    [[ -e "$mdx" ]] || continue
    if [[ -n "${expected_mdx["$mdx"]+present}" ]]; then
        continue
    fi
    rm "$mdx"
    printf 'removed stale %s\n' "${mdx#"$site_root"/}"
done
