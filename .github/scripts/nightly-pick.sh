#!/usr/bin/env bash
# nightly-pick.sh WORKFLOW...
#
# Finds the newest commit on main that has a completed run of each
# WORKFLOW (a file name in .github/workflows). It writes these lines to
# $GITHUB_OUTPUT:
#
#   sha=<the commit>
#   run_<workflow>=<the run ID>   (for each WORKFLOW, "." and "-" as "_")
#   skip=true|false               (true: the nightly release has this commit)
#
# A run that is not complete is not used. Thus a run that is in progress
# does not stop the nightly release: the release uses the run before it.
# Set FORCE=true to publish again when the commit did not change.
#
# It needs GH_TOKEN and GH_REPO.
set -euo pipefail

[ $# -ge 1 ] || { echo "usage: $0 WORKFLOW..." >&2; exit 2; }
out=${GITHUB_OUTPUT:-/dev/stdout}

# the completed runs of a workflow on main (pushes and manual runs), newest first: "sha id"
runs() {
  gh run list --workflow "$1" --branch main --status completed --limit 50 \
    --json headSha,databaseId,event \
    --jq '.[] | select(.event == "push" or .event == "workflow_dispatch") | "\(.headSha) \(.databaseId)"'
}

declare -A list
for w in "$@"; do
  list[$w]=$(runs "$w")
done

sha=
while read -r s _; do
  [ -n "$s" ] || continue
  ok=1
  for w in "$@"; do
    grep -q "^$s " <<<"${list[$w]}" || { ok=0; break; }
  done
  if [ "$ok" = 1 ]; then sha=$s; break; fi
done <<<"${list[$1]}"

if [ -z "$sha" ]; then
  echo "::error::No commit on main has a completed run of each of: $*"
  exit 1
fi

echo "sha=$sha" >> "$out"
for w in "$@"; do
  id=$(grep -m1 "^$sha " <<<"${list[$w]}" | cut -d' ' -f2)
  echo "run_$(tr '.-' '__' <<<"$w")=$id" >> "$out"
  echo "$w: run $id"
done

# (gh writes the error body of a 404 to stdout: no tag is no commit)
old=$(gh api "repos/$GH_REPO/git/ref/tags/nightly" --jq .object.sha 2>/dev/null) || old=
echo "Commit: $sha (the nightly release has: ${old:-none})"
if [ "$sha" = "$old" ] && [ "${FORCE:-false}" != true ]; then
  echo "skip=true" >> "$out"
  echo "No new commits on main. The nightly release does not change."
else
  echo "skip=false" >> "$out"
fi
