# Generate release notes from pull request metadata

## Summary

Valkey should generate release notes and the release version bump from pull
request metadata at release-preparation time instead of maintaining an
in-development changelog file.

The release author should run a manual workflow against the target release
branch, provide the release version, previous tag, urgency, and release date, and
receive a draft pull request that:

- prepends the generated section to `00-RELEASENOTES`
- updates `VALKEY_VERSION`, `VALKEY_VERSION_NUM`, and
  `VALKEY_RELEASE_STAGE` in `src/version.h`
- leaves the generated wording open for human review before merge

This automates the mechanical work while preserving the release author's final
editorial pass.

## Motivation

Release preparation still requires a maintainer to walk every merged pull
request since the previous tag, decide whether it is user-facing, place each
entry in the right section, rewrite titles into release-note language, update
`src/version.h`, and open the release-preparation pull request.

A running changelog would move some of this work earlier, but it has two
practical drawbacks:

- every user-facing pull request edits a shared file, which creates avoidable
  conflict churn
- the changelog entry duplicates information already present in the pull request
  title, body, labels, and author metadata

OpenSearch recently moved away from per-pull-request `CHANGELOG.md` updates for
similar reasons: the file became a frequent conflict point and still had missing
entries, bad links, and original-versus-backport pull request mismatches. Their
current release process generates release notes from pull request and commit
metadata at release time, opens a bot-authored release-notes pull request, and
keeps human review as the quality gate. Their repository `CHANGELOG.md` now
points readers to an unreleased-change pull request search instead of accepting
per-change entries.

Valkey should adopt that operating model rather than the old file-maintenance
model. Valkey does not need to copy every OpenSearch implementation detail:
OpenSearch uses a Jenkins job in `opensearch-build` and an LLM prompt to rewrite,
filter, and categorize entries. Valkey can start with a smaller deterministic
generator because the release-note volume is lower, `00-RELEASENOTES` has a
different format, and the existing `release-notes` label already identifies many
user-facing changes.

Related discussion:

- [Valkey issue #3952](https://github.com/valkey-io/valkey/issues/3952)
- [OpenSearch issue #21071](https://github.com/opensearch-project/OpenSearch/issues/21071)

## Source of truth

The source of truth for release-note inclusion should be the pull request, not a
tree file.

Each pull request should make the user-facing decision during review:

- `release-notes`: include this pull request in generated release notes
- `skip-changelog`, `skip-release-notes`, or `no-release-notes`: explicitly skip
  this pull request

The release-note text should come from:

1. a `Release-note: ...` line in the pull request description, when present
2. otherwise, the pull request title

Reviewers should treat vague release-note wording as review feedback. If the
pull request title and description are not good enough to explain the user-facing
change, they are probably not good enough for review either.

Categories should come from labels where possible. A small label taxonomy can be
added over time, but the first generator can use the labels Valkey already has:

- `cluster` -> `Cluster and Replication`
- `performance` -> `Performance and Efficiency improvements`
- `bug` -> `Bug Fixes`
- `enhancement` -> `New Features and enhanced behavior`
- `breaking-change` -> `Behavior Changes`

Pull requests that have release-note inclusion but no recognized category should
fall into `Other Changes` so the release author can move them during review.

## Branch and range model

Release-note generation must be branch-aware.

For patch releases, the workflow runs on the release branch, such as `9.0`, and
collects commits in:

```text
<previous-tag>..HEAD
```

For each commit, the generator resolves the associated pull request. If a
backport squash commit includes both the original pull request number and the
backport pull request number, the generator should prefer the original pull
request because it usually carries the user-facing labels and wording.

For new minor releases, the same workflow can run after the release branch is
cut. The release author should provide the previous tag or branch-point range
explicitly until the branch-point detection rule is finalized.

## Workflow

The first implementation should add a `workflow_dispatch` workflow:

```text
.github/workflows/prepare-release.yml
```

Inputs:

- `version`: for example `9.2.0-rc1`, `9.2.0`, or `9.0.5`
- `previous_tag`: previous release tag to diff from
- `urgency`: `LOW`, `MODERATE`, `HIGH`, `CRITICAL`, or `SECURITY`
- `release_date`: date shown in `00-RELEASENOTES`

The workflow should:

1. check out the selected branch with full history
2. collect pull requests in the requested range
3. generate the new `00-RELEASENOTES` section
4. update `src/version.h`
5. open a draft pull request for review

The workflow should not cut a release tag. Tagging remains a human release
decision and continues to trigger the existing post-release automation.

This intentionally mirrors OpenSearch at the process level:

- release manager triggers generation as part of release preparation
- generator reads pull request and commit metadata rather than a maintained
  changelog section
- bot opens a release-notes pull request
- reviewers perform the final editorial pass

The first Valkey implementation should differ only where Valkey's release
process is simpler:

- use GitHub Actions instead of Jenkins
- use deterministic label/title rendering instead of an LLM dependency
- update `src/version.h` in the same pull request because Valkey currently does
  that manually as part of release preparation
- prepend to `00-RELEASENOTES` rather than generating OpenSearch's
  `release-notes/opensearch.release-notes-<version>.md` artifact

## Security notes

Security fixes may come from private or embargoed work that is not fully visible
through public pull request metadata. The generator should not pretend to solve
that.

The generated `Security fixes` section should be treated as a draft. The release
author must still review and add CVE wording manually when needed.

## In-development view

The main tradeoff is losing a committed, human-readable "unreleased" changelog
during development. This is acceptable for the first version because:

- GitHub pull request search can show merged `release-notes` pull requests for a
  branch and date range
- the generator can be run in dry-run mode before release day
- avoiding a shared changelog file removes the conflict and drift risk entirely

If maintainers later want a better in-development view, add a read-only workflow
that generates an artifact or job summary on demand. Do not commit a periodically
updated changelog back to the repository.

## CI label gate

A label gate is useful, but it should be a separate rollout step.

The first version should generate release notes from available metadata without
blocking pull requests. After the workflow has been used successfully, add an
advisory check that asks each pull request to carry either `release-notes` or an
explicit skip label. Once maintainers are comfortable with the signal quality,
the check can become required.

This keeps the initial automation valuable without making label taxonomy a
release blocker.

## Publishing target

Valkey's contribution process expects contributors to push branches to forks and
open pull requests from there. Release automation that creates branches in
`valkey-io/valkey` should be treated as a maintainer-approved exception, not as
the default contributor path.

If the workflow runs in the upstream repository, maintainers should explicitly
decide whether the bot may push a temporary `prepare-release/<version>` branch to
the upstream repository or whether it should push to a dedicated automation fork.

## Rollout

1. Land the generator and manual workflow.
2. Test the generator against a recent patch release and compare the output with
   the manually written release notes.
3. Use the workflow for one release candidate with human review.
4. Add missing category labels or mappings based on the review experience.
5. Add an advisory `release-notes` versus skip-label CI check.
6. Decide whether the label check should become required.

This sequencing gives maintainers useful automation immediately while keeping
the higher-friction policy changes separate.
