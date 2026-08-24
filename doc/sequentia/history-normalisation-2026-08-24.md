# History normalisation of 2026-08-24: resyncing an older clone

On 2026-08-23/24 the git history of this repository (and of every other
Sequentia repository) was rewritten once and force-pushed, to normalise commit
metadata: author and committer identities were unified to the project identity
where they were personal or misconfigured variants of it, one misattributed
commit was reassigned to its actual author, and machine-generated trailer
lines were removed from commit messages. **No tree changed**: every branch's
content is byte-for-byte identical before and after, verified per branch
before the push. Release tags keep their names but point at the rewritten
commits.

The rewrite was scoped to Sequentia's own history. Everything up to and
including the Elements 23.3.3 fork point
(`1af7a4d9bea93b4d7f29a77f9751a0e6e03a4390`) keeps its exact upstream SHAs,
signatures included.

## If your clone predates it

A clone from before the rewrite still holds the old commits, and pushing or
pulling from it naively will tangle the two histories. Resync it once:

- **No local work:**

      git fetch origin
      git reset --hard origin/master
      git fetch --tags --force

  The `--force` on the tag fetch matters: a plain `git fetch --tags` does
  *not* update a tag that already exists locally, so your release tags would
  silently keep pointing at commits that no longer exist upstream.

- **Local unpushed commits:** rebase them onto the new history — the trees
  are identical on both sides, so this applies cleanly:

      git fetch origin
      git rebase --onto origin/master @{upstream} your-branch

- Never resolve the situation with a plain `git pull`: it would merge the old
  and new lines and resurrect the old commits.

## Verifying a clean resync

Check for the removed trailer forms — this must come back empty:

    git log --grep='Session:' --grep='noreply@anthropic'

And, if you want content proof, compare trees rather than commit ids: your old
tip and its rewritten counterpart have the same `git rev-parse <commit>^{tree}`.

Do **not** verify by author names. The project's own line still legitimately
contains commits by earlier contributors (for example the 2024 any-asset-fees
work by Mihailo Milenkovic and JBetz, reached through the
`2d298f7e3` merge line): those are real history and were correctly left
untouched. A check that expects only the current committer identities will
flag a perfectly clean clone.
