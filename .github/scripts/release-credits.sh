#!/usr/bin/env bash
# Print a "Credits" Markdown section for release notes, built from the
# "Ported-from:" trailers of the commits in a range. Prints nothing when no
# commit in the range carries one.
#
# Usage: release-credits.sh <git-revision-range>   e.g. v1.2.0..HEAD, or HEAD
#
# Trailer convention, one per ported upstream change, next to the
# Co-authored-by line that credits the author on the commit itself:
#   Ported-from: MatixYo/ESP32-Plane-Radar#90 (33c32e7) by @pvanbaren
#   Co-authored-by: Philip Van Baren <pdvb@yahoo.com>
#
# GitHub's generated notes only list PR titles and the PR author, so this is
# what puts the original authors of ported work into the release notes.
set -euo pipefail

range="${1:?usage: release-credits.sh <git-revision-range>}"

mapfile -t credits < <(
  git log --format='%(trailers:key=Ported-from,valueonly,unfold)' "$range" |
    sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d' |
    sort -u
)

if [[ ${#credits[@]} -eq 0 ]]; then
  exit 0
fi

echo "## Credits"
echo
echo "This release includes work ported from other repositories. Thanks to:"
echo
for credit in "${credits[@]}"; do
  echo "* ${credit}"
done
