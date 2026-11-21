#!/usr/bin/env bash
# nightly-publish.sh SHA NOTES DIR
#
# Replaces the "nightly" pre-release with a new one: its tag at SHA, its
# text from the file NOTES, its files all the files in DIR.
#
# It needs GH_TOKEN and GH_REPO.
set -euo pipefail

sha=$1 notes=$2 dir=$3
shopt -s nullglob
files=("$dir"/*)
[ ${#files[@]} -gt 0 ] || { echo "::error::No files to publish in $dir"; exit 1; }

gh release delete nightly --yes --cleanup-tag 2>/dev/null || true
# (a tag that has no release)
gh api -X DELETE "repos/$GH_REPO/git/refs/tags/nightly" 2>/dev/null || true

gh release create nightly "${files[@]}" \
  --prerelease --latest=false --target "$sha" \
  --title "Nightly build ($(date -u +%Y-%m-%d), ${sha:0:7})" \
  --notes-file "$notes"
