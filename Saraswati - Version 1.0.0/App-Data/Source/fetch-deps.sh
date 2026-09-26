#!/usr/bin/env bash
# Shallow-fetches every library listed in deps.txt into third_party/ at its pinned commit.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p third_party
grep -v '^#' deps.txt | while read -r dir url commit; do
  [ -z "$dir" ] && continue
  dest="third_party/$dir"
  if [ -d "$dest/.git" ] && [ "$(git -C "$dest" rev-parse HEAD)" = "$commit" ]; then
    echo "$dir: already at $commit"; continue
  fi
  rm -rf "$dest"; mkdir -p "$dest"
  git -C "$dest" init -q
  git -C "$dest" remote add origin "$url"
  git -C "$dest" fetch -q --depth 1 origin "$commit"
  git -C "$dest" checkout -q FETCH_HEAD
  echo "$dir: fetched $commit"
done
