# Running a Valkey Release

The short version:

1. Run [Prepare Release][prepare-release].
2. Merge the preparation PR.
3. Approve `release`.
4. Approve `release-publish`.
5. Finish the follow-up work linked from the tracker.

Most of the work between those points is automatic. You still need to review
and merge the downstream PRs. Prepare opens a `Release <tag>` tracking issue;
use it as the home page for the release. It links the runs, approvals, and
follow-up work. Editing the issue does not advance the release. Use the
[active-release filter](https://github.com/valkey-io/valkey/issues?q=is%3Aissue+is%3Aopen+label%3Arelease-tracking)
to find every open tracker. This runbook covers technical publication;
scheduling and announcements remain release-team decisions.

## Before RC1 on a new line

1. Create `M.m` from the agreed `unstable` cutoff.
2. Create the `Valkey M.m` project if it does not exist. Make sure it has
   `To be backported` and `Done` statuses, then change its workflow so PRs
   merged into `unstable` move to `To be backported` instead of `Merged` or
   `Done`.
3. Open and merge a `valkey-ci-agent` PR that adds `M.m` to
   [`release_policy.yml`][release-policy] and adds the branch and project
   number to [`repos.yml`][backport-registry]. You do not need to change the
   build or validation settings in that file. Under the `valkey-io/valkey`
   entry's `branches` list, copy the existing pattern:

   ```yaml
   - branch: "9.2"
     project_number: 51
   ```

   Replace `9.2` with `M.m`. Replace `51` with the number at the end of the
   project's URL; for example, the [Valkey 9.2 project][backport-project-example]
   ends in `/projects/51`.

Prepare checks the policy file, but it does not check that the Valkey branch
exists. If the branch is missing, the notes cut will fail later.

## Before every release

- Make sure your membership in
  [`valkey-io/valkey-committers`][team-committers] or
  [`valkey-io/valkey-release`][team-release] is active. A pending invitation is
  not enough. The same membership covers both approvals.
- Confirm the release scope. For an RC or GA, filter the release project to
  that target and make sure every unfinished item is complete or explicitly
  deferred. For a patch, confirm which fixes are meant to ship.
- Make sure every selected change is on `M.m`. Use the backport steps below
  for anything that merged into `unstable` after the release branch was cut.
- Leave `src/version.h` alone; the preparation PR updates it.
- Do not use this public process for an embargoed security fix.
- Do not run two releases for the same `M.m` branch at once. The automation
  does not enforce this.

## Before RC2, GA, or a patch: update `M.m`

The release owner decides which fixes from `unstable` belong on the release
line:

1. On the merged source PR, set the `Valkey M.m` project status to
   `To be backported`. Add the PR to the project first if its workflow did not.
2. Run [Backport Sweep][backport-sweep] with `repo` set to
   `valkey-io/valkey`, `project_number` set to the number beside the branch in
   [`repos.yml`][backport-registry], and `dry_run` set to `false`. Leaving
   `dry_run` on only reports what it found; it does not create a backport PR.
3. Find the generated `[backport] Backport sweep for M.m` PR in the
   [open-backports filter][open-backports]. Review its `Applied` and
   `Needs attention` sections, wait for the required checks, and merge it.
4. Repeat until every fix intended for the next release is on `M.m`. The
   scheduled [Backport Mark Done Poll][backport-mark-done] moves verified
   project items from `To be backported` to `Done`.

If one fix needs a standalone backport PR, run
[Manual Backport][manual-backport] with the source PR URL and target branch,
then review and merge the result. The automation does the cherry-pick and
validation; the release owner still chooses what ships.

## Release flow

### 1. Run [Prepare Release][prepare-release]

Open [Prepare Release][prepare-release] in `valkey-ci-agent`.

| Input | Value |
| --- | --- |
| `branch` | Release line such as `9.1`, never a full version |
| `intent` | `rc` → next `M.m.0-rcN`; `ga` → `M.m.0`; `patch` → next `M.m.p` |
| `urgency` | `LOW`, `MODERATE`, `HIGH`, `CRITICAL`, or `SECURITY` |
| `dry_run` | `false` to release; `true` only to preview the derived version |

Urgency is a maintainer decision; the workflow flags release-impact signals
but does not assign severity. Use `SECURITY` only for already-public security
fixes. If the preparation PR reports an urgency or security mismatch, either
rerun [Prepare Release][prepare-release] with the corrected urgency or use
inline review comments to add or correct the `Security Fixes` content. Do not
merge until the warning is resolved.

You do not enter a version. The workflow calculates it from the branch and the
existing tags.

A real run opens these at the same time:

- a new `Release <tag>` issue in `valkey-io/valkey`, labeled
  `release-tracking`; and
- the `agent/release-cut/...` preparation PR.

Bookmark the issue. It is where you follow the release.

### 2. Review and merge the preparation PR

Review the change and its PR body:

- Confirm the resolved notes range and read every omission, triage,
  release-impact, or security warning.
- Review the dated `00-RELEASENOTES` section.
- Check the `src/version.h` update.
- Check the contributor footer.

If the PR is a draft, resolve its hold reasons. Do not mark it ready merely to
bypass them.

Want a wording change? Leave an inline review comment on `00-RELEASENOTES`.
The review poller will apply actionable comments and push an update. It ignores
PR-level comments, comments on other files, and resolved or outdated threads.

Do not push to the preparation branch. A rerun may replace it.

The merge commit is the release candidate. Do not merge anything else into
`M.m` until the GitHub Release is published; the candidate must remain branch
HEAD.

### 3. Wait for qualification

The scheduled [Refresh Release Progress][refresh-release] run finds the merged
PR and starts [Publish Release][publish-release] for the exact merge commit. If
you do not want to wait for the schedule, run
[Refresh Release Progress][refresh-release] yourself.

[Publish Release][publish-release] checks the branch HEAD, preparation PR,
version, notes, tag state, and release-tag ruleset. It then runs a no-publish
qualification:

- RC: binary archive matrix;
- GA or patch: binary archives plus the RPM and DEB build-and-test matrix.

Candidate `ci.yml` is shown for context but does not block. The no-publish
qualification is the publication gate.

### 4. Approve `release`

Open the waiting [Publish Release][publish-release] run. Before approving,
check:

- tag and candidate SHA;
- release type and latest-release decision; and
- qualification result and automation revision.

Approve `release` only if the plan is right. The workflow checks the repository
state, your team membership, and the plan receipt again before it writes
anything. It then creates the tag at the candidate commit and publishes the
GitHub Release.

### 5. Approve `release-publish`

Publishing the GitHub Release starts [Build Release][build-release]. Open the
waiting run, check that it names the expected version, and confirm that
**Process Inputs** passed. That job resolves the release tag and stops on a
source-SHA mismatch. Then approve `release-publish`.

Expected outputs:

| Output | RC | GA | Patch |
| --- | ---: | ---: | ---: |
| Hashes and binary archives | Yes | Yes | Yes |
| Container PR and exact images | Yes | Yes | Yes |
| RPM and DEB repositories | No | Yes | Yes |
| Documentation | No | PR; merge creates tag | Tag at prior docs commit |
| Website release PR | No | Yes | Yes |
| Try Valkey | No | Newest stable only | Newest stable only |
| Helm PR | No | When appVersion advances | When appVersion advances |
| Bundle update | `8.1+` | `8.1+` | `8.1+` |

Bundle waits for the exact public container images. Review and merge the
container PR as soon as it is ready. The production run may stay open until the
images appear.

### 6. Finish the release

Work through the links in the tracker:

1. Review and merge the linked downstream PRs.
2. Check that the expected archives, packages, images, documentation, and
   website changes are live. For GA, merging the documentation PR creates its
   version tag automatically.
3. Helm PRs start as drafts. Mark one ready after the exact public container
   image exists.
4. For the first GA on a new line, complete the backport-onboarding issue and
   merge the generated `repos.yml` PR in `valkey-ci-agent` if the line was not
   registered during the RC cycle. If it is already in
   [`repos.yml`][backport-registry], no onboarding issue or PR is expected.
5. Close the tracker after everything that applies has been checked.

## If something goes wrong

- **Manual approval:** normal Publish and Build runs are dispatched by the bot.
  If you directly dispatch or rerun [Publish Release][publish-release] or
  [Build Release][build-release], you cannot approve that same run. Ask another
  active member of the release teams to approve it.
- **Tracker is stale:** run [Refresh Release Progress][refresh-release].
- **Prepare fails:** read the error first. Check team membership,
  [`release_policy.yml`][release-policy], the Valkey `M.m` branch, and whether
  the existing tags allow the requested intent.
- **Qualification fails:** fix the cause. If the Valkey source changes, cut a
  fresh preparation PR. Otherwise rerun or redispatch
  [Publish Release][publish-release] for the same candidate.
- **`M.m` moved after the prep merge:** cut and merge a fresh preparation PR.
- **Validation or approval is invalidated:** fix the release-tag ruleset if
  needed, review the new plan, and approve again.
- **Publish stopped after creating the tag:** rerun
  [Publish Release][publish-release] with the branch and original 40-character
  candidate SHA.
- **No Build Release run appears:** run
  [Trigger Build Release][trigger-build-release] for the published version and
  `prod`.
- **Production or downstream work fails:** check what completed, fix the
  failure, rerun that work, and refresh the tracker. If Bundle is waiting,
  merge the container PR and wait for the exact public image tags.

## Stopping a release

Before the GitHub Release is published, you can still stop cleanly:

1. Close the tracker and any unmerged preparation PR.
2. Cancel active [Prepare Release][prepare-release] or
   [Publish Release][publish-release] runs.
3. Reject any waiting `release` approval.

Closing the tracker stops future refreshes from advancing the release, but it
does not cancel a run that has already started.

Once the GitHub Release is published, do not abandon the version. During an
incident, reject `release-publish` or cancel
[Build Release][build-release] to stop more writes, then fix forward and finish
the release.

Disabling [Refresh Release Progress][refresh-release] also stops only future
refreshes, not active runs.

## A few hard rules

- Do not hand-create or move the release tag or GitHub Release.
- Do not edit `src/version.h` or push to the preparation branch.
- Do not merge into `M.m` between preparation merge and GitHub Release
  publication.
- Do not edit tracker metadata or paste marker-like text into tracker comments.
- Do not use this process for an embargoed security release.

[prepare-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-prepare.yml
[refresh-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-progress.yml
[publish-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-publish.yml
[build-release]: https://github.com/valkey-io/valkey-release-automation/actions/workflows/build-release.yml
[trigger-build-release]: https://github.com/valkey-io/valkey/actions/workflows/trigger-build-release.yml
[team-committers]: https://github.com/orgs/valkey-io/teams/valkey-committers
[team-release]: https://github.com/orgs/valkey-io/teams/valkey-release
[release-policy]: https://github.com/valkey-io/valkey-ci-agent/blob/main/release_policy.yml
[backport-registry]: https://github.com/valkey-io/valkey-ci-agent/blob/main/repos.yml
[backport-project-example]: https://github.com/orgs/valkey-io/projects/51
[backport-sweep]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-sweep.yml
[backport-mark-done]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-mark-done-poll.yml
[manual-backport]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/manual-backport.yml
[open-backports]: https://github.com/valkey-io/valkey/pulls?q=is%3Apr+is%3Aopen+label%3Abackport
