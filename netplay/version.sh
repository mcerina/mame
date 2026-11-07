#!/bin/sh
# The build version, which every player's build must share: the commit, plus
# "-dirty" when the tracked files have local changes.
cd "$(dirname "$0")/.." || exit 1
v=$(git rev-parse --short=10 HEAD) || exit 1
[ -n "$(git status --porcelain --untracked-files=no)" ] && v="$v-dirty"
echo "$v"
