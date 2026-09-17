# Running a Valkey Release

Four release-control decisions:

1. Dispatch **Prepare Release**.
2. Merge the preparation PR.
3. Approve `release`.
4. Approve `release-publish`.

The automation advances the release between those decisions. Downstream PRs
still require maintainer review and merge. Start from the `Release <tag>`
tracking issue; it links the runs, approvals, and follow-up work. The tracker
displays live state but authorizes nothing. Use the
[active-release filter](https://github.com/valkey-io/valkey/issues?q=is%3Aissue+is%3Aopen+label%3Arelease-tracking)
to find every open tracker.

## Before you start

- You must be an active member of `valkey-io/valkey-committers` or
  `valkey-io/valkey-release`. A pending invitation grants no access. The same
  membership is used for both approvals.
- For a new major or minor line, create `M.m` from `unstable` and add it to
  `release_policy.yml` in `valkey-ci-agent`. Prepare checks the policy entry,
  but a missing Valkey branch fails later during the notes cut.
- Merge every intended backport into `M.m`.
- Do not edit `src/version.h`; the preparation PR updates it.
- Do not use this public process for an embargoed security fix. `SECURITY`
  urgency is for already-disclosed fixes.
- Run only one active release per `M.m` branch. This is an operating rule, not
  an automation-enforced lock.

## 1. Dispatch Prepare Release

Open
[Prepare Release](https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-prepare.yml)
in `valkey-ci-agent`.

| Input | Value |
| --- | --- |
| `branch` | Release line such as `9.1`, never a full version |
| `intent` | `rc` → next `M.m.0-rcN`; `ga` → `M.m.0`; `patch` → next `M.m.p` |
| `urgency` | `LOW`, `MODERATE`, `HIGH`, `CRITICAL`, or `SECURITY` |
| `dry_run` | Derive the version without creating a PR or tracker |

The version is derived from the branch and existing tags; you never enter it.
A real run opens two items in parallel:

- a new `Release <tag>` issue in `valkey-io/valkey`, labeled
  `release-tracking`; and
- the `agent/release-cut/...` preparation PR.

The issue is the release dashboard. Bookmark it and use it to follow the
release.

## 2. Review and merge the preparation PR

Check:

- the dated `00-RELEASENOTES` section;
- the `src/version.h` update; and
- the contributor footer.

A draft PR has unresolved hold reasons in its body. Resolve them before marking
it ready.

Request wording changes with inline review comments on `00-RELEASENOTES`. The
periodic review poller applies actionable comments and pushes an update.
PR-level comments, comments on other files, and resolved or outdated threads
are ignored.

Do not push to the preparation branch; rerunning the cut may replace it.

The merge commit becomes the release candidate. Do not merge anything else into
`M.m` until the GitHub Release is published: publication requires that commit
to remain branch HEAD.

## 3. Wait for qualification

[Refresh Release Progress](https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-progress.yml)
detects the merged PR and dispatches
[Publish Release](https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-publish.yml)
for its exact merge commit. Dispatch Refresh manually if you do not want to wait
for the periodic run.

Publish Release rechecks the branch HEAD, preparation PR, version, notes, tag
state, and release-tag ruleset, then runs a no-publish qualification:

- RC: binary archive matrix;
- GA or patch: binary archives plus the RPM and DEB build-and-test matrix.

Candidate `ci.yml` status is advisory. The no-publish qualification is the
technical publication gate.

## 4. Approve `release`

Open the waiting **Publish Release** run and review its release plan:

- tag and candidate SHA;
- release type and latest-release decision; and
- qualification result and automation revision.

Approve `release` only if the plan is correct. Publication then rechecks live
repository state, your team membership, and the approved plan receipt. On
success it creates the tag at the candidate commit and publishes the GitHub
Release.

## 5. Approve `release-publish`

The GitHub Release triggers
[Build Release](https://github.com/valkey-io/valkey-release-automation/actions/workflows/build-release.yml).
Before approving, confirm that the run names the expected version and that
**Process Inputs** succeeded. That job resolves the release tag and rejects a
source-SHA mismatch before approval is offered. Then approve
`release-publish`.

Expected outputs:

| Output | RC | GA | Patch |
| --- | ---: | ---: | ---: |
| Hashes and binary archives | Yes | Yes | Yes |
| Container PR and exact images | Yes | Yes | Yes |
| RPM and DEB repositories | No | Yes | Yes |
| Versioned documentation | No | PR | Tag from the preceding patch |
| Website release PR | No | Yes | Yes |
| Try Valkey | No | Newest stable only | Newest stable only |
| Helm PR | No | When appVersion advances | When appVersion advances |
| Bundle update | `8.1+` | `8.1+` | `8.1+` |

Bundle waits for exact public container images. Review and merge the container
PR as soon as it is ready; production may remain running until those images
appear.

## 6. Finish the release

The tracker links each applicable downstream target.

1. Review and merge the linked downstream PRs.
2. Verify the expected archives, packages, images, documentation, and website
   changes are live.
3. Helm PRs start as drafts. Mark one ready only after the exact public
   container image exists.
4. For the first GA on a new line, complete the backport-onboarding issue and
   merge the generated `repos.yml` PR in `valkey-ci-agent`. If neither appears,
   inspect the Publish Release run.
5. Close the tracker only after all applicable outputs are verified.

## If something goes wrong

- **Tracker is stale:** dispatch **Refresh Release Progress**.
- **Prepare fails:** follow the error. Check team membership,
  `release_policy.yml`, the Valkey `M.m` branch, and whether existing tags allow
  the requested intent.
- **Qualification fails:** fix the cause. If Valkey source changes, cut a fresh
  preparation PR; otherwise rerun or redispatch Publish Release for the same
  candidate.
- **`M.m` moved after the prep merge:** cut and merge a fresh preparation PR.
- **Validation or approval is invalidated:** restore the release-tag ruleset if
  necessary, review the new plan, and approve again.
- **Publish stopped after creating the tag:** redispatch Publish Release with
  the branch and original 40-character candidate SHA.
- **No Build Release run appears:** rerun Valkey's **Trigger Build Release** for
  the published version and `prod`.
- **Production or downstream work fails:** inspect partial results, fix and
  rerun the failed work, then refresh the tracker. If Bundle is waiting, merge
  the container PR and wait for the exact public image tags.

## Stopping a release

Before the GitHub Release is published, close the tracker and any unmerged
preparation PR, cancel active Prepare/Publish runs, and reject any waiting
`release` approval. Closing the tracker prevents future reconciliation but does
not cancel work already running.

After the GitHub Release is published, the version cannot be abandoned. During
an incident, reject `release-publish` or cancel Build Release to contain further
writes, then recover forward and complete or repair the remaining outputs.

Disabling **Refresh Release Progress** stops future refreshes, not active runs.

## Do not

- Hand-create or move the release tag or GitHub Release.
- Edit `src/version.h` or push to the preparation branch.
- Merge into `M.m` between preparation merge and GitHub Release publication.
- Edit tracker metadata or paste marker-like text into tracker comments.
- Use this process for an embargoed security release.
