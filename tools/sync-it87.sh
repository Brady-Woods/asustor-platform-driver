#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sync-it87.sh - (re-)import Frank Crawford's out-of-tree it87 driver
# (https://github.com/frankcrawford/it87) into this repository.
#
# Usage:
#   tools/sync-it87.sh [--commit] [--pristine] <it87-checkout> <ref> [<commit>[:<label>]...]
#   tools/sync-it87.sh --diff <it87-checkout>
#
#   <it87-checkout>  a clone of frankcrawford/it87 (only read, never modified;
#                    fetch it yourself first)
#   <ref>            the upstream commit to import, e.g. origin/master
#   <commit>         extra upstream commits to apply on top, e.g. an unmerged
#                    pull request: 61c0770:'PR #110'. Commits that <ref> already
#                    contains are skipped, so once the PR is merged, a plain
#                    `tools/sync-it87.sh ../it87 origin/master` drops it.
#
#   --commit         commit the result ("it87: import frankcrawford/it87 ...")
#   --pristine       overwrite the vendored files with the upstream ones,
#                    dropping our local changes (default: keep them, see below)
#   --diff           show our local changes: the diff between the upstream
#                    version recorded in it87.UPSTREAM and the vendored files
#
# Vendored files: it87.c and compat.h, plus it87.UPSTREAM, which records what
# was imported (and is read by the Makefile for the module version).
#
# Our own changes to it87.c are separate commits on top of the import. On a
# re-import, the upstream change since the last import (old upstream -> new
# upstream, both rebuilt from it87.UPSTREAM and the checkout) is merged into
# the current files with `git merge-file`, so the local changes stay. If they
# conflict, the files are left with conflict markers and the script fails:
# resolve, then commit. The commits recorded in it87.UPSTREAM have to be in
# the checkout for this (and for --diff).

set -euo pipefail

FILES="it87.c compat.h"
URL="https://github.com/frankcrawford/it87"

die() { echo "sync-it87: $*" >&2; exit 1; }

commit=0
pristine=0
diff=0
while [ $# -gt 0 ]; do
	case "$1" in
	--commit) commit=1; shift ;;
	--pristine) pristine=1; shift ;;
	--diff) diff=1; shift ;;
	-h|--help) sed -n '3,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	--) shift; break ;;
	-*) die "unknown option $1" ;;
	*) break ;;
	esac
done
if [ $diff = 1 ]; then
	[ $# -eq 1 ] || die "usage: $0 --diff <it87-checkout>"
else
	[ $# -ge 2 ] || die "usage: $0 [--commit] [--pristine] <it87-checkout> <ref> [<commit>[:<label>]...]"
fi

src=$1
ref=${2:-}
shift
[ $# -eq 0 ] || shift

top=$(cd "$(dirname "$0")/.." && pwd)
record="$top/it87.UPSTREAM"
git -C "$src" rev-parse --git-dir >/dev/null 2>&1 || die "$src is not a git checkout"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# build_tree <dir> <base> [<commit> ...]: write the upstream files at <base>
# with <commit>s applied on top (3-way, so they may be based on another
# upstream commit) into <dir>. Applied commits are appended to $tmp/applied.
build_tree() {
	local dir=$1 base=$2 c f
	shift 2
	for c in "$base" "$@"; do
		git -C "$src" cat-file -e "$c^{commit}" 2>/dev/null ||
			die "commit $c is not in $src, fetch it first (a pull request: git fetch origin pull/<N>/head)"
	done
	mkdir -p "$dir"
	: >"$tmp/applied"
	for f in $FILES; do
		git -C "$src" show "$base:$f" >"$dir/$f"
	done
	for c in "$@"; do
		if git -C "$src" merge-base --is-ancestor "$c" "$base"; then
			echo "sync-it87: $c is already in $base, skipping it" >&2
			continue
		fi
		for f in $FILES; do
			git -C "$src" show "$c^:$f" >"$tmp/old" 2>/dev/null || : >"$tmp/old"
			git -C "$src" show "$c:$f" >"$tmp/new" 2>/dev/null || continue
			git merge-file -q "$dir/$f" "$tmp/old" "$tmp/new" ||
				die "$c does not apply to $base ($f)"
		done
		echo "$c" >>"$tmp/applied"
	done
}

# build_recorded_tree: the upstream version recorded in it87.UPSTREAM, in
# $tmp/old-tree
build_recorded_tree() {
	local old_base old_extras
	[ -f "$record" ] || die "no it87.UPSTREAM, nothing was imported yet"
	old_base=$(sed -n 's/^base=//p' "$record")
	old_extras=$(sed -n 's/^extra=\([0-9a-f]*\).*/\1/p' "$record")
	# shellcheck disable=SC2086
	build_tree "$tmp/old-tree" "$old_base" $old_extras
}

if [ $diff = 1 ]; then
	build_recorded_tree
	for f in $FILES; do
		diff -u --label "a/$f" --label "b/$f" "$tmp/old-tree/$f" "$top/$f" || :
	done
	exit 0
fi

# resolve the new upstream
base=$(git -C "$src" rev-parse --verify "$ref^{commit}") || die "unknown ref $ref"
extras=""
: >"$tmp/labels"
for arg in "$@"; do
	c=${arg%%:*}
	label=""
	if [ "$c" != "$arg" ]; then
		label=${arg#*:}
	fi
	c=$(git -C "$src" rev-parse --verify "$c^{commit}") || die "unknown commit $c"
	extras="$extras $c"
	echo "$c $label" >>"$tmp/labels"
done
label_of() { sed -n "s/^$1 //p" "$tmp/labels"; }

# shellcheck disable=SC2086
build_tree "$tmp/new-tree" "$base" $extras
applied=$(cat "$tmp/applied")

version=$(git -C "$src" describe --long --always --tags "$base")
for c in $applied; do
	version="$version+$(git -C "$src" rev-parse --short "$c")"
done

# merge (or copy) into our files
if [ $pristine = 0 ] && [ -f "$record" ]; then
	build_recorded_tree
	conflicts=0
	for f in $FILES; do
		[ -f "$top/$f" ] || cp "$tmp/old-tree/$f" "$top/$f"
		if ! git merge-file -L "$f (ours)" -L "$f (previous import)" \
			-L "$f (upstream)" "$top/$f" "$tmp/old-tree/$f" \
			"$tmp/new-tree/$f"; then
			echo "sync-it87: conflicts in $f, resolve them" >&2
			conflicts=1
		fi
	done
else
	for f in $FILES; do
		cp "$tmp/new-tree/$f" "$top/$f"
	done
	conflicts=0
fi

{
	echo "# Upstream of it87.c and compat.h, written by tools/sync-it87.sh."
	echo "# Local changes are separate commits on top of the import commit."
	echo "url=$URL"
	echo "base=$base"
	for c in $applied; do
		label=$(label_of "$c")
		echo "extra=$c${label:+ $label}"
	done
	echo "version=$version"
} >"$record"

# commit message
short=$(git -C "$src" rev-parse --short "$base")
subject="it87: import frankcrawford/it87 $short"
body="Imported with tools/sync-it87.sh from $URL:

  $(git -C "$src" log -1 --format='%H %s' "$base")"
for c in $applied; do
	label=$(label_of "$c")
	subject="$subject + ${label:-$(git -C "$src" rev-parse --short "$c")}"
	[ -n "$label" ] && subject="$subject ($(git -C "$src" rev-parse --short "$c"))"
	body="$body
  + $(git -C "$src" log -1 --format='%H %s' "$c")${label:+ ($label)}"
done
body="$body

Module version: $version"

if [ $conflicts = 1 ]; then
	die "merge conflicts, nothing committed"
fi

echo "$subject"
echo
echo "$body"

if [ $commit = 1 ]; then
	cd "$top"
	# shellcheck disable=SC2086
	git add -- $FILES it87.UPSTREAM
	git commit -q -m "$subject" -m "$body" -- $FILES it87.UPSTREAM
	echo
	git log -1 --oneline
fi
