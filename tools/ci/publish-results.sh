#!/bin/bash
# publish-results.sh <dir> - commit a CI run's logs to the ci-results branch: <branch>/<run>/ and <branch>/latest/.
# Env: TOKEN (GITHUB_TOKEN), GITHUB_REPOSITORY, BRANCH, RUN, SHA. Small files only (each log capped at 2 MB).
set -u
src=$(cd "$1" && pwd)
b=$(echo "$BRANCH" | tr '/' '_')
w=$(mktemp -d)
cd "$w" || exit 1
git init -q
git config user.name "ci"
git config user.email "ci@users.noreply.github.com"
git remote add origin "https://x-access-token:${TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
for try in 1 2 3 4 5; do
    rm -rf ./* 2>/dev/null
    if git fetch -q --depth 1 origin ci-results 2>/dev/null; then git checkout -q -B ci-results FETCH_HEAD
    else git checkout -q --orphan ci-results; fi
    for d in "$b/$RUN" "$b/latest"; do
        rm -rf "$d"; mkdir -p "$d"
        for f in "$src"/*; do [ -f "$f" ] && head -c 2097152 "$f" > "$d/$(basename "$f")"; done
        printf 'branch %s\ncommit %s\nrun %s\n' "$BRANCH" "$SHA" "$RUN" > "$d/RUN.txt"
    done
    git add -A
    git commit -q -m "ci: $BRANCH run $RUN ($SHA)" || exit 0
    git push -q origin ci-results && { echo "published to ci-results:$b/$RUN"; exit 0; }
    sleep $((try * 3))
done
echo "publish failed"; exit 1
