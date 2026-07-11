# Release and build versioning policy

Status: policy, adopted 2026-07-11 after a real debugging session where build
provenance could not be established.

## The problem this fixes

The official testnet binaries on sequentiatestnet.com report a version such as
`/Elements Core:23.3.3/`, set by the version fields in `configure.ac`. But the
commit that bumped the version to "23.3.3 final" (`1d767802c`) and the next
bump to 23.3.4 (`05a9810d6`) are **371 commits apart** on this branch, and the
version string does not change in between. A binary reporting 23.3.3 can
therefore have been built from any of ~370 commits — including ones on either
side of consensus-relevant changes (pinned PoS rules, stake delegation,
vesting locks, payout policies). When two nodes disagree, "we're both on
23.3.3" establishes nothing, and there is no way to reconstruct what code a
peer is actually running from its subversion string.

A second, smaller problem: a local rebuild made for a cosmetic GUI fix bumped
the version to 23.3.4. That number now looks like an official release that was
never published, and future genuinely-official releases have to skip over it.

## Policy

1. **Tag every official release.** The operator builds the published installer
   from an exact commit and tags it in git at that commit
   (`git tag -a v23.3.4 -m "..." <commit> && git push origin v23.3.4`).
   "Official" means: uploaded to sequentiatestnet.com or otherwise handed to
   testers as *the* build. The tag is the only reliable map from a reported
   version back to code.

2. **Private and test rebuilds do not bump the version.** A rebuild for local
   testing, a cosmetic fix, or an experiment keeps the version of the release
   it forked from and identifies itself with a user-agent comment instead:
   run with `-uacomment=<yourname>` (or set it in `elements.conf`), which
   makes the node report e.g. `/Elements Core:23.3.3(alberto)/` to its peers.
   The version number itself is reserved for operator-published releases.

3. **Bump the version only in the commit the release is built from**, i.e. the
   version-bump commit and the tagged release commit should be the same
   commit (or adjacent). This keeps the version string monotonic with
   publication order, not with unrelated branch activity.

## Why not enforce this in the build?

There is no mechanical way for the build to know whether it is "official" —
that is a statement about who publishes it, not about the code. The
enforcement point is the operator's release checklist: build, tag, upload,
in that order, from the same commit.
