#!/bin/bash
# Both master builds and version tags publish to João's single stable release.
set -euo pipefail
package_dir=${1:?Usage: publish_linux_release.sh PACKAGE_DIR VERSION}
version=${2:?VERSION is required}
: "${GITHUB_REPOSITORY:?}"
: "${GITHUB_SHA:?}"
: "${GH_TOKEN:?}"
release_tag=Realease
shopt -s nullglob
files=("$package_dir"/*)
if [ "${#files[@]}" -eq 0 ]; then
    echo 'ERROR: no validated packages to publish' >&2
    exit 1
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
release_id=$(gh api "repos/${GITHUB_REPOSITORY}/releases/tags/${release_tag}" --jq .id)
gh release upload "$release_tag" "${files[@]}" --repo "$GITHUB_REPOSITORY" --clobber

# Remove previous downloads only after every new asset is uploaded and its
# size agrees with the local package that passed validation.
gh api --paginate "repos/${GITHUB_REPOSITORY}/releases/${release_id}/assets" \
    --jq '.[] | [.id, .name, .size] | @tsv' > "$tmp/assets.tsv"
for file in "${files[@]}"; do
    name=$(basename "$file")
    size=$(stat -c %s "$file")
    awk -F '\t' -v name="$name" -v size="$size" \
        '$2 == name && $3 == size {found=1} END {exit !found}' "$tmp/assets.tsv"
done
while IFS=$'\t' read -r asset_id name size; do
    if [[ "$name" == SDRPP-FULL-v* ]] && [ ! -f "$package_dir/$name" ]; then
        gh api -X DELETE "repos/${GITHUB_REPOSITORY}/releases/assets/${asset_id}"
    fi
done < "$tmp/assets.tsv"

# Align the source download with the exact commit used to build the packages.
gh api -X PATCH "repos/${GITHUB_REPOSITORY}/git/refs/tags/${release_tag}" \
    -f sha="$GITHUB_SHA" -F force=true >/dev/null

# Preserve João's notes below the current version summary.
gh release view "$release_tag" --repo "$GITHUB_REPOSITORY" \
    --json body --jq .body > "$tmp/old-notes.md"
printf 'SDRPP-FULL v%s Linux — continuous audio through batched device callbacks with bounded headroom; preserved LogMMSE, responsive RTL gain/AGC, filters and private RTL-SDR 2.0.3/Blog V4L; commit %s\n' \
    "$version" "$GITHUB_SHA" > "$tmp/notes.md"
if head -n 1 "$tmp/old-notes.md" | grep -q '^SDRPP-FULL v[0-9].*Linux'; then
    tail -n +2 "$tmp/old-notes.md" >> "$tmp/notes.md"
else
    printf '\n' >> "$tmp/notes.md"
    cat "$tmp/old-notes.md" >> "$tmp/notes.md"
fi
gh release edit "$release_tag" --repo "$GITHUB_REPOSITORY" \
    --title "SDRPP-FULL v${version}" --latest --notes-file "$tmp/notes.md"
if gh release view nightly --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
    gh release delete nightly --repo "$GITHUB_REPOSITORY" --cleanup-tag --yes
fi
