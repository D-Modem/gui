#!/usr/bin/env bash
# nightly-publish.sh SHA NOTES DIR
#
# Updates the "nightly" pre-release: its tag to SHA, its text from the
# file NOTES, and its files to all the files in DIR. If there is no
# release, it makes one.
#
# The script does not delete the release. If a step fails, the old
# release stays (with its old files or some new files), and the next run
# tries again.
#
# It needs GH_TOKEN and GH_REPO.
set -euo pipefail

sha=$1 notes=$2 dir=$3
shopt -s nullglob
files=("$dir"/*)
[ ${#files[@]} -gt 0 ] || { echo "::error::No files to publish in $dir"; exit 1; }
title="Nightly build ($(date -u +%Y-%m-%d), ${sha:0:7})"

# release_id: prints the ID of the "nightly" release, or nothing. (gh
# writes the error body of a 404 to stdout: on an error, print nothing.)
release_id() {
  local id
  id=$(gh api "repos/$GH_REPO/releases/tags/nightly" --jq .id 2>/dev/null) || id=
  echo "$id"
}

# move_tag: puts the "nightly" tag at $sha, and makes the tag if it is
# not there
move_tag() {
  if gh api "repos/$GH_REPO/git/ref/tags/nightly" --jq .object.sha >/dev/null 2>&1; then
    gh api -X PATCH "repos/$GH_REPO/git/refs/tags/nightly" \
      -f sha="$sha" -F force=true >/dev/null
  else
    gh api -X POST "repos/$GH_REPO/git/refs" \
      -f ref=refs/tags/nightly -f sha="$sha" >/dev/null
  fi
}

id=$(release_id)
if [ -z "$id" ]; then
  # (no release: a new one, its tag made at $sha)
  gh release create nightly "${files[@]}" \
    --prerelease --latest=false --target "$sha" \
    --title "$title" --notes-file "$notes"
  exit 0
fi

move_tag
gh release edit nightly --prerelease --latest=false \
  --title "$title" --notes-file "$notes"
gh release upload nightly "${files[@]}" --clobber

# the old files that this build did not make
keep=$(for f in "${files[@]}"; do basename "$f"; done)
gh api "repos/$GH_REPO/releases/$id/assets" --paginate --jq '.[] | "\(.id) \(.name)"' |
  while read -r aid name; do
    if ! grep -qxF "$name" <<<"$keep"; then
      echo "Removing the old file $name"
      gh api -X DELETE "repos/$GH_REPO/releases/assets/$aid"
    fi
  done
