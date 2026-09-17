# Running a Valkey Release

There are four release-control decisions:

1. Dispatch **Prepare Release**.
2. Review and merge the preparation PR.
3. Approve the `release` environment.
4. Approve the `release-publish` environment.

The automation advances the release between those decisions. Downstream pull
requests still require maintainer review and merge. Use the `Release <tag>`
tracking issue as the starting point: it links the relevant runs, approvals,
and downstream work.

## Before you start

- **Confirm team membership.** You must be an active member of
  `valkey-io/valkey-committers` or `valkey-io/valkey-release`. A pending
  invitation does not authorize anything. Ask a release-team maintainer if you
  need access.
- **For a new major or minor line:** create the `M.m` branch from `unstable`,
  then add `M.m` to `release_policy.yml` in `valkey-ci-agent`. Prepare validates
  the policy entry but does not verify that the Valkey branch exists; a missing
  branch therefore fails later during the notes cut. Check `release_policy.yml`
  for the currently configured lines.
- Make sure every backport intended for the release is already merged into
  `M.m`.
- Do not edit `src/version.h`; the preparation PR updates it.
- Do not use this public workflow for an embargoed security fix. The tracker,
  preparation PR, and release notes are public. The `SECURITY` urgency is for
  already-disclosed fixes.
- Run only one active release on a given `M.m` branch. This is an operating
  rule, not an automation-enforced lock. Different release lines may proceed
  independently.

## 1. Dispatch Prepare Release

In `valkey-ci-agent`, open **Actions → Prepare Release**.

| Input | Value |
|---|---|
| `branch` | Release line such as `9.1`; never enter a full version. |
| `intent` | `rc` for the next `M.m.0-rcN`; `ga` for `M.m.0`; `patch` for the next `M.m.p`. |
| `urgency` | `LOW`, `MODERATE`, `HIGH`, `CRITICAL`, or `SECURITY`. |
| `dry_run` | Select to derive and display the version without creating a PR or tracker. |

The version is derived from the release line and existing tags; you do not
enter it yourself.

A non-dry run creates:

- the `Release <tag>` tracking issue; and
- the `agent/release-cut/...` preparation PR.

Keep the tracking issue open until the release and its required follow-up work
have been verified.

## 2. Review and merge the preparation PR

Review all three generated areas:

- the dated section in `00-RELEASENOTES`;
- the version update in `src/version.h`; and
- the refreshed contributor footer.

A draft PR means the notes cut found something requiring maintainer judgment.
Read the hold reasons in the PR body before marking it ready.

To request release-note changes, leave inline review comments on
`00-RELEASENOTES`. The review poller applies actionable comments and pushes an
updated commit. PR-level comments, comments on other files, and resolved or
outdated threads are ignored. Allow a few minutes for the periodic poller.

Do not push directly to the preparation branch. Re-running the notes cut may
replace that branch.

Merging the preparation PR selects its merge commit as the release candidate.
From that merge until the GitHub release is published, do not merge anything
else into `M.m`: publication requires the candidate to remain the branch HEAD.

## 3. Wait for qualification

After the preparation PR merges, **Refresh Release Progress** discovers it and
dispatches **Publish Release** for the exact merge commit. This is periodic, so
it may not happen immediately. Manually dispatching **Refresh Release Progress**
is safe when you want an immediate refresh.

Publish Release revalidates:

- the release branch and exact branch HEAD;
- the canonical preparation PR and merge commit;
- `src/version.h` and the release-notes section;
- the next version implied by existing tags;
- tag and GitHub Release availability; and
- the active release-tag ruleset.

It then runs a no-publish qualification against the exact candidate:

- RC: the complete binary archive matrix;
- GA or patch: the binary archive matrix plus the RPM and DEB build-and-test
  matrix.

Candidate `ci.yml` status is visible as advisory context. The no-publish
qualification workflow is the technical publication gate.

## 4. Approve the `release` environment

Open the waiting **Publish Release** run. Review the rendered release plan,
including:

- tag and release type;
- candidate commit SHA;
- latest-release decision; and
- qualification result and automation revision.

Approve the `release` environment only when that plan is correct.

After approval, publication revalidates live repository state and your current
team membership. It also verifies that the approved plan receipt still matches.
If anything relevant changed, publication refuses instead of creating a
different release.

On success, the tag is created at the candidate commit and the GitHub Release
is published.

## 5. Approve the `release-publish` environment

Publishing the GitHub Release triggers **Build Release** in
`valkey-release-automation`. Its input-validation job resolves the published
tag back to a commit before the protected production job can run.

Before approving `release-publish`, inspect the completed input-validation job
and confirm that the version and resolved source commit match the GitHub
Release. Then approve the environment.

The production outputs depend on the release type:

| Output | RC | GA | Patch |
|---|---:|---:|---:|
| Source hash entry and binary archives | Yes | Yes | Yes |
| Container update PR and exact-version images | Yes | Yes | Yes |
| RPM and DEB repositories | No | Yes | Yes |
| Versioned documentation | No | Documentation PR | Tag from previous patch documentation |
| Website release PR | No | Yes | Yes |
| Try Valkey | No | If this is the newest stable release | If this is the newest stable release |
| Helm chart PR | No | If the chart version advances | If the chart version advances |
| Valkey Bundle update | For supported lines | For supported lines | For supported lines |

The production run can remain active while waiting for the exact public
container images needed by Bundle. Review and merge the container PR as soon as
it is ready; do not wait for the entire production run to finish before
starting downstream follow-up.

## 6. Complete downstream follow-up

Use the tracker links and review downstream work as it appears.

1. Review and merge each applicable downstream PR.
2. Confirm that the expected archives, packages, images, documentation, and
   website changes are live.
3. Helm PRs are drafts intentionally. Mark one ready only after the exact public
   container image for the release exists.
4. For the first GA on a new line, complete the backport-onboarding issue and
   merge the generated `repos.yml` registry PR in `valkey-ci-agent`. The
   onboarding job is best-effort; if neither item appears, inspect the Publish
   Release run.
5. Close the tracking issue only after all applicable work has been verified.

## Reading the tracker

The tracker is rebuilt from live GitHub state on every refresh. It is a display,
not an authorization mechanism.

- Editing the issue body or ticking a checkbox cannot approve or change the
  release.
- Controller metadata is stored in the bot-owned status comment.
- If the display looks stale, dispatch **Refresh Release Progress**.

## Troubleshooting

| Symptom | Action |
|---|---|
| Tracker looks stale | Dispatch **Refresh Release Progress**. |
| `intent` is refused | Inspect the error. An RC cannot follow a final release, GA requires an existing RC, and patch requires an existing final release. |
| Prepare refuses the request | Confirm active team membership and that the branch is listed in `release_policy.yml`. |
| Notes cut fails for a new line | Confirm that the `M.m` branch exists in Valkey, then rerun Prepare Release. |
| Qualification fails | Inspect the failed matrix job. Resolve the cause, then rerun or redispatch Publish Release for the same candidate. |
| The release branch moved after the preparation merge | Cut and merge a fresh preparation PR. Do not force publication from the old candidate. |
| `cannot verify an active immutable-tag ruleset` | A repository administrator must restore the active ruleset restricting release-tag creation, update, and deletion. |
| Approval is invalidated | Review the new run and plan, then approve again. |
| Publish Release stopped after creating the tag | Redispatch **Publish Release** with the release branch and the original full 40-character candidate SHA. It will verify the existing tag before continuing. |
| The Valkey trigger workflow failed before Build Release appeared | Rerun **Trigger Build Release** for the published version and `prod`. |
| A production or downstream job failed | Inspect the partial results, fix the cause, rerun the failed job or workflow, and refresh the tracker. |
| Bundle is waiting for images | Review and merge the container PR, then wait for the exact public image tags to appear. |

## Stopping a release

Before the GitHub Release is published:

1. Close the tracking issue to prevent future refreshes from dispatching more
   work.
2. Cancel any active Prepare Release or Publish Release run.
3. Reject any waiting `release` approval.

Closing the tracker does **not** cancel a workflow that has already started. If
production is already waiting, also reject `release-publish` or cancel the Build
Release run.

Once the GitHub Release is published, the release cannot be cleanly aborted.
Treat subsequent failures as release recovery work and complete or repair the
required outputs.

Disabling **Refresh Release Progress** stops future reconciliation runs but does
not stop workflows already in progress.

## Do not

- Hand-create or move the release tag or GitHub Release.
- Edit `src/version.h` or push to the preparation branch.
- Merge another change into `M.m` between the preparation-PR merge and GitHub
  Release publication.
- Edit tracker metadata or paste marker-like text into tracker comments.
- Use this process for an embargoed security release.
