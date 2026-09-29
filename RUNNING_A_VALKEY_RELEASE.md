# Running a Valkey Release

The short version:

1. Run [Prepare Release][prepare-release].
2. Merge the preparation PR.
3. Approve `release` in `valkey-io/valkey-ci-agent`.
4. Approve `release-publish` in `valkey-io/valkey-release-automation`.
5. Finish the tracker and Build Release follow-up work.

Most of the work between those points is automatic. You still need to review
and merge the downstream PRs. Prepare creates or reuses a `Release <tag>`
tracking issue. Use it as the home page for the release: it links the runs,
approvals, and follow-up work. Editing the issue does not advance the release.
Use the [active-release filter][active-release-filter] to find every open
tracker. This runbook covers technical publication;
scheduling and announcements remain release-team decisions.

## Prerequisites

- You need access to dispatch workflows in `valkey-ci-agent` and to review and
  merge the Valkey preparation and downstream PRs.
- Active membership in [`valkey-io/valkey-committers`][team-committers] or
  [`valkey-io/valkey-release`][team-release] is required to run Prepare and to
  publish through the `release` environment in `valkey-ci-agent`.
- The `release-publish` environment in `valkey-release-automation` is configured
  separately. Confirm that it has an eligible approver before starting.
- For a new release line, arrange normal `valkey-ci-agent` maintainer review for
  the `release_policy.yml` and `repos.yml` change.
- Repository administrators must have configured the Valkeyrie App and AWS
  credentials used by the workflows. The release owner does not need the secret
  values.

## Before RC1 on a new line

1. Create `M.m` from the agreed `unstable` cutoff.
2. Create the `Valkey M.m` project if it does not exist. Make sure its `Status`
   field contains `RC1 blocker`, `To be backported`, and `Done`.
3. Before registering the line, change the project workflow so PRs merged into
   `unstable` move to `To be backported`, then move every existing merged PR in
   the project to that status. Do not treat an empty RC1 filter as clean until
   you have verified that the `RC1 blocker` option exists.
4. Open and merge a `valkey-ci-agent` PR that adds `M.m` to
   [`release_policy.yml`][release-policy] and adds the branch and project
   number to [`repos.yml`][backport-registry]. You do not need to change the
   build or validation settings in that file. Keep version-like branch names
   quoted in both files; unquoted `9.3` is parsed as a number and rejected.
   Under the `valkey-io/valkey` entry's `branches` list, copy the existing
   pattern:

   ```yaml
   - branch: "9.2"
     project_number: 51
   ```

   Replace `9.2` with `M.m`. Replace `51` with the number at the end of the
   project's URL; for example, the [Valkey 9.2 project][backport-project-example]
   ends in `/projects/51`.
5. After the registry PR merges, run or wait for
   [Backport Mark Done Poll][backport-mark-done]. It moves project PRs that it
   can verify on `M.m` from `To be backported` to `Done`. Manually check any
   RC1-baseline items it leaves behind.

Prepare checks the policy file, but it does not check that the Valkey branch
exists. If the branch is missing, the notes cut will fail later.

## Before every release

- Confirm the release scope in the `Valkey M.m` project. The field is `Status`.
  For RC1, enter `status:"RC1 blocker"` in the project filter and resolve or
  explicitly defer every item it returns. For every later RC, GA, or patch,
  review `status:"To be backported"` and decide which fixes must ship.
- Make sure every selected change is on `M.m`. Use the backport steps below
  for anything that merged into `unstable` after the release branch was cut.
- Leave `src/version.h` alone; the preparation PR updates it.
- Do not use this public process for an embargoed security fix.
- Do not prepare two release intents for the same branch concurrently. The
  workflows serialize execution and Publish pins the branch HEAD, but
  sequential runs can still leave separate RC and GA preparation PRs open.

## After the branch cut: keep `M.m` current

Once the release branch is cut, the release owner is responsible for making
sure fixes selected for the release line actually reach `M.m`. Do not wait
until the next release to inspect the backport queue.

1. On the merged source PR, set the `Valkey M.m` project status to
   `To be backported`. Add the PR to the project first if its workflow did not.
2. Check the [open-backports filter][open-backports] for the
   `[backport] Backport sweep for M.m` PR. Backport Poll checks hourly and
   starts a sweep whenever no sweep PR is open, so another may appear soon
   after the previous one merges. The scheduled [Backport Sweep][backport-sweep]
   is a daily fallback.
3. Review every entry under `Applied`, `Skipped`, and the collapsed
   `Needs attention` section. Confirm that each skipped change genuinely
   contributes nothing to the branch. Backport CI Follow-up also checks hourly
   and may push a fix; review its comment and resulting diff before merging.
   Resolve remaining failures and obtain the required review.
4. If you need an immediate refresh, run [Backport Sweep][backport-sweep] with
   `repo` set to `valkey-io/valkey`, `project_number` set to the number beside
   the branch in [`repos.yml`][backport-registry], `max_candidates` set to `0`
   to include every eligible candidate or to a deliberate batch limit, and
   `dry_run` set to `false`. The default limit of `2` counts only successfully
   applied and validated changes; repeat the sweep if you use a limit and more
   intended candidates remain. Leaving `dry_run` on only reports what it found.
5. Confirm the selected commits are now on `M.m`. The
   scheduled [Backport Mark Done Poll][backport-mark-done] moves verified
   project items from `To be backported` to `Done`.
6. Before every later RC, GA, or patch release, repeat until every intended fix
   is on `M.m` and every remaining project item is explicitly deferred.

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

The form initially selects `rc` and `LOW`; verify both before dispatching.
Prepare defaults `dry_run` to `false`. Its dry run derives only the version and
tag; it does not preview release notes. Cut Release Notes workflows instead
default `dry_run` to `true`.

Urgency is a maintainer decision; the workflow flags release-impact signals but
does not assign severity. Correct a wrong non-security urgency by rerunning
Prepare before review edits are applied.

`Prepare Release` cannot supply security-fix entries. For a `SECURITY` release
covering already-public fixes:

1. Run Prepare with `SECURITY` to derive the version and open the tracker.
2. Run [Cut Release Notes (Advanced)][cut-release-notes-advanced] with
   `repo` set to `valkey`, `urgency` set to `SECURITY`, the same version and
   stage, and `dry_run` set to `false`. Supply `security_fixes` or enable
   `security_from_advisories`.
3. Review the regenerated preparation PR and confirm that its security hold is
   cleared. Inline edits alone do not recalculate the hold banner or draft
   state.

You do not enter a version. The workflow calculates it from the branch and the
existing tags. The intent must match them:

- `rc` is allowed only before any final release exists on the line.
- `ga` requires at least one RC and is refused after a final release exists.
- `patch` requires an existing final release and derives the next patch number.

After derivation, Prepare starts the notes cut and tracker creation
independently. The first run creates a `Release <tag>` issue in
`valkey-io/valkey`, labeled `release-tracking`; later runs for the same tag
reuse or reopen it. The tracker may appear before the `agent/release-cut/...`
preparation PR and may remain if notes generation fails.

Bookmark the issue. It is where you follow the release.

### 2. Review and merge the preparation PR

Review the change and its PR body:

- Confirm the resolved notes range and read every omission, triage,
  release-impact, or security warning.
- Review the dated `00-RELEASENOTES` section.
- Check the `src/version.h` update.
- Check the contributor footer.

If the PR is a draft, resolve its hold reasons. After reviewing and accepting a
non-security review hold, you may use **Ready for review**. A security hold must
be corrected and recut; do not mark it ready to bypass the hold.

Want a wording change? Leave an inline review comment on `00-RELEASENOTES`.
The latest human comment in each unresolved, current thread is used, and its
author must be an active [`valkey-io/contributors`][team-contributors] member.
Edits must stay inside the current dated release section. The preparation PR
must contain only `00-RELEASENOTES` and `src/version.h`; any additional file
makes the poller refuse the PR.

Do not push to the preparation branch. Do not rerun Prepare or either Cut
Release Notes workflow after review-poller edits unless you intend to discard
them: a recut regenerates the notes, force-pushes the preparation branch, and
may return the PR to draft. Reapply and rereview any wording changes afterward.

The commit created by merging the preparation PR is the release candidate. Do
not merge anything else into `M.m` until the GitHub Release is published; the
candidate must remain branch HEAD.

### 3. Wait for qualification

The scheduled [Refresh Release Progress][refresh-release] run finds the merged
PR and starts [Publish Release][publish-release] for the exact candidate
commit. If you do not want to wait for the schedule, run
[Refresh Release Progress][refresh-release] yourself.

[Publish Release][publish-release] checks the branch HEAD, preparation PR,
version, notes, tag state, and release-tag ruleset. It then runs a no-publish
qualification:

- RC: binary archive matrix;
- GA or patch: binary archives plus the RPM and DEB build-and-test matrix.

Candidate `ci.yml` is shown for context but does not block. The no-publish
qualification is the publication gate.

### 4. Approve `release` in `valkey-io/valkey-ci-agent`

Open the waiting [Publish Release][publish-release] run. Before approving,
check:

- tag and candidate SHA;
- release type and latest-release decision; and
- qualification result and automation revision.

Approve `release` only if the plan is right. The workflow checks the repository
state, your team membership, the approval receipt, and its bound plan digest
again before it writes anything. It then creates the tag at the candidate
commit and publishes the GitHub Release.

For the first GA on a release line, also inspect the
`Onboard first-GA backport automation` job in this Publish Release run. It is
best-effort and is not linked from the tracker. If the line is already in
[`repos.yml`][backport-registry], no action is expected. Otherwise complete the
generated onboarding issue and review the generated `repos.yml` PR. If the
project cannot be matched uniquely, only the issue is created; resolve that
blocker before registering the line.

### 5. Approve `release-publish` in `valkey-io/valkey-release-automation`

Publishing the GitHub Release starts [Build Release][build-release]. Open the
waiting run, check that it names the expected version, and confirm that
the `process-inputs` job passed. It resolves the release tag and stops on a
source-SHA mismatch. Then approve `release-publish`.

The account that approves `release-publish` becomes the release owner for the
production run. The generated container, documentation, website, and Helm PRs
mention that account in a comment when they are opened.

Expected outputs:

| Output | RC | GA | Patch |
| --- | ---: | ---: | ---: |
| Hashes and binary archives | Yes | Yes | Yes |
| Container update PR | Yes | Yes | Yes |
| RPM and DEB repositories | No | Yes | Yes |
| Documentation | No | PR; verify tag after merge | Tag at prior docs commit |
| Website release PR | No | Yes | Yes |
| Try Valkey | No | Newest stable only | Newest stable only |
| Helm PR | No | When appVersion advances | When appVersion advances |
| Bundle update | `8.1+` | `8.1+` | `8.1+` |

Build Release opens the `valkey-container` update PR; it does not publish the
images itself. Merge that PR promptly, then wait for external publication and
verify the exact public tags.

The `trigger-valkey-bundle` job waits for both `${VERSION}-alpine` and
`${VERSION}-trixie` on Docker Hub. It fails after about 5 hours 50 minutes. If
it times out, wait until both images are public, then use **Re-run failed jobs**
on the Build Release run.

### 6. Finish the release

Use the tracker for hashes and linked downstream PRs. Use the Build Release run
to verify binary archives, RPM/DEB packages, public container images, and Try
Valkey; those outputs are not all linked from the tracker.

1. Review and merge the linked downstream PRs.
2. Check that the expected archives, packages, images, documentation, and
   website changes are live. For GA, follow the documentation PR's instructions
   and verify that the expected `valkey-doc` version tag exists after merge.
3. Helm PRs start as drafts. Mark one ready after the exact public container
   image exists.
4. Close the tracker after everything that applies has been checked.

## If something goes wrong

- **Manual approval:** normal Publish and Build runs are dispatched by the bot.
  If you directly dispatch or rerun [Publish Release][publish-release] or
  [Build Release][build-release], you cannot approve that same run. Ask another
  eligible reviewer for that repository's environment to approve it.
- **Tracker is stale:** run [Refresh Release Progress][refresh-release].
- **Prepare fails:** read the error first. Check team membership,
  [`release_policy.yml`][release-policy], the Valkey `M.m` branch, and whether
  the existing tags allow the requested intent. The tracker may still have
  opened. Keep it if you will retry the same release; the rerun refreshes it.
  If you are abandoning the attempt, close the tracker and any preparation PR.
- **Qualification fails without a candidate change:** fix the external or
  transient cause, then rerun or redispatch
  [Publish Release][publish-release] for the same candidate.
- **The candidate must change after the preparation PR merged:** before the
  release tag exists, cancel active publication, revert the preparation PR's
  merge result on `M.m` while leaving any later source fixes in place, merge any
  remaining required changes, then run [Prepare Release][prepare-release] again
  and merge the replacement preparation PR. Running Prepare again before the
  revert will fail because `src/version.h` already records that release.
  Do not rerun Prepare directly even if an older tracker prompt tells you to.
- **Validation or approval is invalidated:** this can happen when the
  release-tag ruleset, candidate, plan digest, `valkey-ci-agent/main`, or
  `valkey-release-automation/main` changes. Refresh may cancel a stale Publish
  run, causing its pending approval to disappear. Open the replacement run from
  current `main`, review the new evidence, and approve again.
- **Publish stopped after creating the tag:** the version is committed and must
  be completed. Rerun [Publish Release][publish-release] with the branch and
  original lowercase 40-character candidate SHA.
- **No Build Release run appears:** first inspect the corresponding
  [Trigger Build Release][trigger-build-release] run in `valkey`. A production
  dispatch requires the release tag to exist and resolve to a commit. If the
  trigger failed, fix it and rerun it for the published version and `prod`. If
  it succeeded, look for a run titled exactly `Build Release <tag> (prod)`.
- **Production or downstream work fails:** inspect the Build Release dependency
  chain. A hashes failure prevents container, documentation, Try Valkey,
  website, Helm, and Bundle work from starting, although archives and RPM/DEB
  packages may already have completed. Fix the earliest failed job, use
  **Re-run failed jobs**, and confirm that every previously skipped dependent
  job runs. If Bundle timed out, rerun after both exact public image tags exist.

## Stopping a release

Before the release tag exists, you can still stop cleanly:

1. Cancel active [Prepare Release][prepare-release],
   [Cut Release Notes][cut-release-notes],
   [Cut Release Notes (Advanced)][cut-release-notes-advanced], or
   [Publish Release][publish-release] runs.
2. Close the tracker and any unmerged preparation PR.
3. Reject any waiting `release` approval.

Closing the tracker stops future refreshes from advancing the release, but it
does not cancel a run that has already started. If the preparation PR already
merged, also revert the commit created by merging it; otherwise `src/version.h`
continues to record the release and another Prepare for that version or stage
will fail.

A later Prepare for the same tag reopens the tracker. Do not rerun Prepare if
the release is intentionally stopped.

Once the release tag exists, do not abandon the version. If the GitHub Release
is still missing, rerun [Publish Release][publish-release] with the original
candidate. After the GitHub Release is published, reject `release-publish` or
cancel [Build Release][build-release] to stop more writes during an incident,
then fix forward and finish the release.

Disabling [Refresh Release Progress][refresh-release] also stops only future
refreshes, not active runs.

## A few hard rules

- Do not hand-create or move the release tag or GitHub Release.
- Do not edit `src/version.h` or push to the preparation branch.
- Do not merge into `M.m` between preparation merge and GitHub Release
  publication.
- Do not delete or alter the bot-owned tracker status comment. Ordinary
  discussion comments are not release authority.

[prepare-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-prepare.yml
[cut-release-notes]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-notes-cut.yml
[cut-release-notes-advanced]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-notes-cut-advanced.yml
[refresh-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-progress.yml
[publish-release]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/release-publish.yml
[build-release]: https://github.com/valkey-io/valkey-release-automation/actions/workflows/build-release.yml
[trigger-build-release]: https://github.com/valkey-io/valkey/actions/workflows/trigger-build-release.yml
[team-committers]: https://github.com/orgs/valkey-io/teams/valkey-committers
[team-release]: https://github.com/orgs/valkey-io/teams/valkey-release
[team-contributors]: https://github.com/orgs/valkey-io/teams/contributors
[active-release-filter]: https://github.com/valkey-io/valkey/issues?q=is%3Aissue+is%3Aopen+label%3Arelease-tracking
[release-policy]: https://github.com/valkey-io/valkey-ci-agent/blob/main/release_policy.yml
[backport-registry]: https://github.com/valkey-io/valkey-ci-agent/blob/main/repos.yml
[backport-project-example]: https://github.com/orgs/valkey-io/projects/51
[backport-sweep]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-sweep.yml
[backport-mark-done]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-mark-done-poll.yml
[manual-backport]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/manual-backport.yml
[open-backports]: https://github.com/valkey-io/valkey/pulls?q=is%3Apr+is%3Aopen+label%3Abackport
