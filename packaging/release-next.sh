#!/bin/bash
# Bump the patch version and release the checked-out branch through CI.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A tag must name the commit that contains its matching VERSION. Keep that
# order here: commit and push the bump, run the full CI workflow for that exact
# commit, then create and push an unused annotated tag. A failed CI run leaves
# the version commit in place but never starts the release workflow.

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: ./packaging/release-next.sh [--sign]

From the current branch, bump VERSION to the next unused patch version,
commit and push it, run and wait for CI, then tag and push the tested commit.
Wait for the release workflow and report the published GitHub Release.
Use --sign to sign the annotated tag with your configured Git signing key.
EOF
}

fail()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

wait_for_run()
{
    local workflow="$1" event="$2" ref="$3" sha="$4"
    local runs run_id attempt

    for ((attempt = 0; attempt < 36; attempt++)); do
        runs="$(gh run list --repo "$repo" --workflow "$workflow" \
            --event "$event" --branch "$ref" --commit "$sha" --limit 30 \
            --json databaseId,headSha)" \
            || fail "cannot list $workflow runs"
        run_id="$(jq -r --arg sha "$sha" \
            '[.[] | select(.headSha == $sha)] | first | .databaseId // empty' \
            <<<"$runs")" \
            || fail "cannot read $workflow run metadata"
        if [ -n "$run_id" ]; then
            printf '%s\n' "$run_id"
            return 0
        fi
        sleep 5
    done
    fail "$workflow did not appear for $sha; check GitHub Actions before retrying"
}

wait_for_success()
{
    local run_id="$1" label="$2" details status conclusion last_status=""
    while :; do
        details="$(gh run view "$run_id" --repo "$repo" --json status,conclusion)" \
            || fail "cannot read $label run $run_id"
        status="$(jq -r '.status' <<<"$details")" \
            || fail "cannot read $label status"
        conclusion="$(jq -r '.conclusion // empty' <<<"$details")" \
            || fail "cannot read $label conclusion"
        if [ "$status" != "$last_status" ]; then
            printf '%s: %s\n' "$label" "$status"
            last_status="$status"
        fi
        if [ "$status" = completed ]; then
            [ "$conclusion" = success ] \
                || fail "$label finished with ${conclusion:-no conclusion}; inspect run $run_id"
            return 0
        fi
        sleep 10
    done
}

sign_tag=false
case "${1:-}" in
    "") ;;
    --sign) sign_tag=true ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || { usage >&2; exit 2; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
[ "$(git rev-parse --show-toplevel)" = "$repo_root" ] \
    || fail "the script must be stored under the repository's packaging directory"
branch="$(git branch --show-current)"
[ -n "$branch" ] || fail "check out a branch before releasing"
[ -z "$(git status --porcelain --untracked-files=all)" ] \
    || fail "the worktree is not clean"
git remote get-url origin >/dev/null 2>&1 || fail "origin is not configured"
git fetch --no-tags --quiet origin master || fail "cannot fetch origin/master"
git merge-base --is-ancestor FETCH_HEAD HEAD \
    || fail "the current branch is behind origin/master; update it before releasing"
for command in gh jq; do
    command -v "$command" >/dev/null 2>&1 || fail "$command is required"
done
[ -f .github/workflows/ci.yml ] && [ -f .github/workflows/release.yml ] \
    || fail "the CI and release workflows must exist on this branch"

version="$(<VERSION)"
release="$(<packaging/RELEASE)"
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
    || fail "VERSION must contain exactly MAJOR.MINOR.PATCH"
major="${BASH_REMATCH[1]}"
minor="${BASH_REMATCH[2]}"
patch="${BASH_REMATCH[3]}"
[[ "$release" =~ ^[1-9][0-9]*$ ]] \
    || fail "packaging/RELEASE must contain a positive integer"
repo="$(gh repo view --json nameWithOwner --jq .nameWithOwner)" \
    || fail "GitHub CLI cannot access this repository"
[ -n "$repo" ] || fail "GitHub CLI did not identify the repository"
remote_tags="$(git ls-remote --tags --refs origin 'refs/tags/v*')" \
    || fail "cannot list remote release tags"

# A failed or abandoned tag still consumes its name. Never move or reuse it.
declare -A used_tags=()
remote_versions=()
while read -r _sha ref; do
    [ -n "${ref:-}" ] || continue
    remote_name="${ref#refs/tags/}"
    used_tags["$remote_name"]=1
    if [[ "$remote_name" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
        remote_versions+=("${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}")
    fi
done <<<"$remote_tags"
highest_version="$(printf '%s\n' "$version" "${remote_versions[@]}" | sort -V | tail -n 1)"
[ "$highest_version" = "$version" ] \
    || fail "VERSION $version is behind existing tag v$highest_version"
next_patch=$((10#$patch + 1))
while :; do
    next_version="$major.$minor.$next_patch"
    tag="v$next_version"
    if [ -z "${used_tags[$tag]:-}" ] \
        && ! git show-ref --verify --quiet "refs/tags/$tag"; then
        break
    fi
    next_patch=$((next_patch + 1))
done

printf '%s\n' "$next_version" > VERSION
git add -- VERSION
git commit -m "chore: Release $next_version"
head="$(git rev-parse HEAD)"
[ "$(git show HEAD:VERSION)" = "$next_version" ] \
    || fail "the version commit does not contain $next_version"
[ -z "$(git status --porcelain --untracked-files=all)" ] \
    || fail "a commit hook changed the worktree; nothing was pushed"
printf 'Pushing %s at %s with VERSION=%s\n' "$branch" "$head" "$next_version"
git push --set-upstream origin "HEAD:refs/heads/$branch" \
    || fail "branch push failed; the version commit remains local"

# Master gets an automatic push run. Other branches need workflow_dispatch.
# The new commit, event and branch identify the run for this release.
ci_event=push
if [ "$branch" != master ]; then
    ci_event=workflow_dispatch
    gh workflow run ci.yml --repo "$repo" --ref "$branch" \
        || fail "CI dispatch failed; the version commit was pushed but no tag was created"
fi
ci_run="$(wait_for_run ci.yml "$ci_event" "$branch" "$head")"
printf 'Waiting for CI: https://github.com/%s/actions/runs/%s\n' "$repo" "$ci_run"
wait_for_success "$ci_run" CI

[ "$(git rev-parse HEAD)" = "$head" ] \
    || fail "HEAD changed while CI ran; no release tag was created"
[ -z "$(git status --porcelain --untracked-files=all)" ] \
    || fail "the worktree changed while CI ran; no release tag was created"
remote_branch="$(git ls-remote --exit-code origin "refs/heads/$branch")" \
    || fail "cannot verify the pushed branch"
[ "${remote_branch%%[[:space:]]*}" = "$head" ] \
    || fail "the remote branch moved while CI ran; no release tag was created"
remote_tag="$(git ls-remote --tags --refs origin "refs/tags/$tag")" \
    || fail "cannot recheck $tag on origin"
if git show-ref --verify --quiet "refs/tags/$tag" || [ -n "$remote_tag" ]; then
    fail "$tag was created while CI ran; no tag was moved or reused"
fi

if "$sign_tag"; then
    git tag -s -m "nasmount $next_version (package release $release)" "$tag" "$head"
else
    git tag -a -m "nasmount $next_version (package release $release)" "$tag" "$head"
fi
git push origin "refs/tags/$tag:refs/tags/$tag" \
    || fail "tag push failed; $tag exists locally and was not force-pushed"
release_run="$(wait_for_run release.yml push "$tag" "$head")"
printf 'Waiting for release: https://github.com/%s/actions/runs/%s\n' "$repo" "$release_run"
wait_for_success "$release_run" Release

release_info="$(gh release view "$tag" --repo "$repo" --json isDraft,url)" \
    || fail "release workflow finished, but $tag has no GitHub Release"
release_url="$(jq -r 'select(.isDraft == false) | .url // empty' <<<"$release_info")" \
    || fail "cannot read release metadata"
[ -n "$release_url" ] || fail "$tag was not published as a GitHub Release"
printf 'Published %s\n' "$release_url"
