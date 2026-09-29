# Running a Valkey Release

The short version:

1. Run [Prepare Release][prepare-release].
2. Merge the preparation PR.
3. Approve `release` in `valkey-io/valkey-ci-agent`.
4. Approve `release-publish` in `valkey-io/valkey-release-automation`.
5. Finish the tracker and Build Release follow-up work.

Most steps are automatic; you review and merge PRs. Prepare creates or reuses a
`Release <tag>` tracker ([open trackers][active-release-filter]) that links the
main release runs, approvals, and follow-up work; editing it does not advance
the release. Scope: technical publication only.

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
  credentials used by the workflows.

## Hard rules

- Do not hand-create or move the release tag or GitHub Release.
- Do not use this public process for an embargoed security fix.
- Do not edit `src/version.h` or push to the preparation branch.
- Treat [Cut Release Notes][cut-release-notes] and
  [Cut Release Notes (Advanced)][cut-release-notes-advanced] as
  release-owner-only controls. They do not check release-team membership; any
  `valkey-ci-agent` writer can recut and overwrite the preparation PR.
- Keep `M.m` frozen between preparation merge and GitHub Release publication,
  as described in [step 2](#2-review-and-merge-the-preparation-pr).
- Do not delete or alter the bot-owned tracker status comment. Ordinary
  discussion comments are not release authority.

## Before RC1 on a new line

1. Create `M.m` from the agreed `unstable` cutoff.
2. Create the `Valkey M.m` project if it does not exist. Make sure its `Status`
   field contains `RC1 blocker`, `To be backported`, and `Done`.
3. Before registering the line, change the project workflow so PRs merged into
   `unstable` move to `To be backported`, then move every existing merged PR in
   the project to that status.
4. Open and merge a `valkey-ci-agent` PR that adds `M.m` to
   [`release_policy.yml`][release-policy] and adds the branch and project
   number to [`repos.yml`][backport-registry]. You do not need to change the
   build or validation settings. Keep version-like branch names quoted in both
   files; unquoted `9.3` is parsed as a number and rejected. Under the
   `valkey-io/valkey` entry's `branches` list, add:

   ```yaml
   - branch: "M.m"       # keep the quotes
     project_number: <N> # from the project URL: .../projects/<N>
   ```

5. After the registry PR merges, run or wait for
   [Backport Mark Done Poll][backport-mark-done]. It cannot identify a
   rebase-merged baseline PR whose commits carry no PR number; move those items
   to `Done` manually. If Backport Poll runs first, the initial sweep may list
   already-present baseline items under `Skipped`; verify them rather than
   treating them as missing backports.

Prepare checks the policy file, but it does not check that the Valkey branch
exists. If the branch is missing, the notes cut will fail later.

## Before every release

- Confirm the release scope in the `Valkey M.m` project. The field is `Status`.
  For RC1, enter `status:"RC1 blocker"` in the project filter and resolve or
  explicitly defer every item it returns. For every later RC, GA, or patch,
  review `status:"To be backported"` and decide which fixes must ship.
- Complete the [backport steps](#after-the-branch-cut-keep-mm-current) until
  every selected change is on `M.m`.
- Do not prepare two release intents for the same branch concurrently. The
  workflows serialize execution and Publish pins the branch HEAD, but
  sequential runs can still leave separate RC and GA preparation PRs open.

## After the branch cut: keep `M.m` current

Once the release branch is cut, keep its backport queue current between
releases.

1. On the merged source PR, set the `Valkey M.m` project status to
   `To be backported`. Add the PR to the project first if its workflow did not.
2. Check the [open-backports filter][open-backports] for the
   `[backport] Backport sweep for M.m` PR. Backport Poll checks hourly and
   starts one if none is open; the scheduled [Backport Sweep][backport-sweep]
   is a daily fallback.
3. Review every entry under `Applied`, `Skipped`, and the collapsed
   `Needs attention` section. Confirm that each skipped change genuinely
   contributes nothing. Backport CI Follow-up may push an hourly fix; inspect
   its comment and diff, resolve remaining failures, and obtain review.
4. For an immediate refresh, run [Backport Sweep][backport-sweep] with
   `repo=valkey-io/valkey`, `project_number=<N>`, `max_candidates=0`, and
   `dry_run=false`. A limit of `0` includes every eligible candidate. The
   default limit of `2` counts successful validated changes; repeat a limited
   sweep while intended candidates remain. The default `dry_run=true` only
   reports.
5. Confirm the selected commits are now on `M.m`. The
   scheduled [Backport Mark Done Poll][backport-mark-done] moves verified
   project items from `To be backported` to `Done`. Explicitly defer every
   remaining project item that is not intended for the release.

If one fix needs a standalone backport PR, run
[Manual Backport][manual-backport] with the source PR URL and target branch,
then review and merge the result.

## Release flow

### 1. Run [Prepare Release][prepare-release]

Open [Prepare Release][prepare-release] in `valkey-ci-agent`.

| Input | Value |
| --- | --- |
| `branch` | Release line such as `9.1`, never a full version |
| `intent` | Default `rc`. `rc` → next `M.m.0-rcN`, only before a final; `ga` → `M.m.0`, requiring an RC and no final; `patch` → next `M.m.p`, requiring a final |
| `urgency` | Default `LOW`, so verify it. `LOW`, `MODERATE`, `HIGH`, `CRITICAL`, or `SECURITY`; see below for `SECURITY` |
| `dry_run` | Default `false`. `true` shows the derived tag only, with no notes preview; Cut Release Notes defaults to `true` |

Urgency is a maintainer decision; the workflow flags release-impact signals but
does not assign severity. For a wrong non-security urgency, rerun Prepare before
any review edits.

`Prepare Release` cannot supply security-fix entries. For a `SECURITY` release
covering already-public fixes:

1. Run Prepare with `SECURITY` to derive the version and open the tracker.
2. Wait for Prepare's **Open release preparation PR** job to finish. Prepare
   always starts the basic notes cut. Both cuts share a per-version lock and do
   not cancel in-progress runs: Advanced may otherwise wait for hours, while an
   Advanced run that gets there first can be overwritten by Prepare's later
   cut.
3. Run [Cut Release Notes (Advanced)][cut-release-notes-advanced] with
   `repo` set to `valkey`, `urgency` set to `SECURITY`, the same version, and
   `dry_run` set to `false`. Use the same stage for RC or GA; for a patch,
   leave `stage` empty so it infers `ga`. Supply `security_fixes` or enable
   `security_from_advisories`. Never run Advanced before Prepare.
4. Review the regenerated preparation PR and confirm that its security hold is
   cleared. Inline edits alone do not recalculate the hold banner or draft
   state.

The tracker and `agent/release-cut/...` PR are created independently. The
tracker may appear first, or remain alone if the notes cut fails.

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
The review poller requires:

- the latest comment in an unresolved, current thread;
- an author who is an active
  [`valkey-io/contributors`][team-contributors] member;
- an edit inside the current dated release section; and
- no changed file other than `00-RELEASENOTES` or `src/version.h`.

Any Prepare, basic, or Advanced recut after poller edits regenerates the notes,
force-pushes the bot-owned preparation branch, and may return the PR to draft.
Recut only if you intend to discard, reapply, and rereview those edits.

The commit created by merging the preparation PR is the release candidate. Do
not merge anything else into `M.m` until the GitHub Release is published; the
candidate must remain branch HEAD.

### 3. Wait for qualification

The scheduled [Refresh Release Progress][refresh-release] run finds the merged
PR and starts [Publish Release][publish-release] for the exact candidate
commit.

[Publish Release][publish-release] checks the branch HEAD, preparation PR,
version, notes, tag state, and release-tag ruleset, then runs a no-publish
qualification:

- RC: binary archive matrix;
- GA or patch: binary archives plus the RPM and DEB build-and-test matrix.

Candidate `ci.yml` is informational; the no-publish qualification is the gate.

### 4. Approve `release` in `valkey-io/valkey-ci-agent`

Open the waiting [Publish Release][publish-release] run. Before approving,
check the tag and candidate SHA, release type and latest-release decision,
qualification result, and automation revision.

Approve `release` only if the plan is right. The workflow checks the repository
state, your team membership, the approval receipt, and its bound plan digest
again before it writes anything. It then creates the tag at the candidate
commit and publishes the GitHub Release.

For the first GA on a release line, also inspect the
`Onboard first-GA backport automation` job in this Publish Release run. It is
best-effort and not tracker-linked. If the line is absent from
[`repos.yml`][backport-registry], complete its issue and review its generated
PR. An ambiguous project match creates only the issue; resolve it before
registration.

### 5. Approve `release-publish` in `valkey-io/valkey-release-automation`

Publishing the GitHub Release starts [Build Release][build-release]. Open the
waiting run. Before approving, check the expected version and confirm that the
`process-inputs` job passed; it stops on a source-SHA mismatch. Then approve
`release-publish`.

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

- **Container:** Build Release only opens the update PR; images publish after
  it merges.
- **Bundle:** waits for `${VERSION}-alpine` and `${VERSION}-trixie` on Docker
  Hub and fails after about 5h50m. Rerun failed jobs once both exist.

### 6. Finish the release

1. Review and merge the linked downstream PRs.
2. Verify archives, packages, public images, Try Valkey, documentation, and
   website changes are live. Use the Build Release run for outputs the tracker
   does not link. For GA, verify the expected `valkey-doc` tag after merging the
   documentation PR.
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
  existing tags allow the intent. Keep an opened tracker if retrying the same
  release; otherwise close it and any preparation PR.
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
  `valkey-release-automation/main` changes. Refresh may cancel the stale
  Publish run and its approval. Review and approve the replacement from current
  `main`.
- **No Build Release run appears:** first inspect the corresponding
  [Trigger Build Release][trigger-build-release] run in `valkey`. A production
  dispatch requires a tag resolving to a commit. If the trigger failed, fix and
  rerun it for the published version and `prod`; otherwise find the run titled
  exactly `Build Release <tag> (prod)`.
- **Production or downstream work fails:** inspect the Build Release dependency
  chain. A hashes failure prevents container, documentation, Try Valkey,
  website, Helm, and Bundle work from starting, although archives and RPM/DEB
  packages may already have completed. Fix the earliest failed job, use
  **Re-run failed jobs**, and if Bundle timed out, rerun after both exact public
  image tags exist.

## Stopping a release

- **No release tag:** cancel active [Prepare Release][prepare-release],
  [Cut Release Notes][cut-release-notes],
  [Cut Release Notes (Advanced)][cut-release-notes-advanced], and
  [Publish Release][publish-release] runs. Close the tracker and any unmerged
  preparation PR, and reject a waiting `release` approval. If preparation
  already merged, revert that merge result while keeping later source fixes.
  Do not rerun Prepare; it reopens the tracker. Neither closing the tracker nor
  disabling [Refresh Release Progress][refresh-release] cancels active runs.
- **Tag exists, but no GitHub Release:** rerun
  [Publish Release][publish-release] with the branch and original lowercase
  40-character candidate SHA, then complete the version.
- **GitHub Release published:** reject a waiting `release-publish` approval or
  cancel [Build Release][build-release] to stop further writes during an
  incident, then fix forward and finish the release.

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
[backport-sweep]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-sweep.yml
[backport-mark-done]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/backport-mark-done-poll.yml
[manual-backport]: https://github.com/valkey-io/valkey-ci-agent/actions/workflows/manual-backport.yml
[open-backports]: https://github.com/valkey-io/valkey/pulls?q=is%3Apr+is%3Aopen+label%3Abackport
