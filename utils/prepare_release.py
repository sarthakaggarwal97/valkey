#!/usr/bin/env python3
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from collections import OrderedDict


URGENCY_TEXT = {
    "LOW": "No need to upgrade unless there are new features you want to use.",
    "MODERATE": "Program an upgrade of the server, but it's not urgent.",
    "HIGH": "There is a critical bug that may affect a subset of users. Upgrade!",
    "CRITICAL": "There is a critical bug affecting MOST USERS. Upgrade ASAP.",
    "SECURITY": "There are security fixes in the release.",
}

URGENCY_SUMMARY = {
    "LOW": "This release includes stability, bug fixes, and incremental improvements.",
    "MODERATE": "Program an upgrade of the server, but it's not urgent.",
    "HIGH": "There are critical bugs that may affect a subset of users.",
    "CRITICAL": "There are critical bugs affecting most users.",
    "SECURITY": "This release includes security fixes we recommend you apply as soon as possible.",
}

# Map a PR to a release-notes section by label. The first matching row wins, so
# order is the category priority: a PR labeled both `breaking-change` and `bug`
# is filed under Behavior Changes. Only labels that exist in valkey-io/valkey
# today are mapped; sections like Security fixes, Module API, and Build and
# Tooling are produced by hand at release time until matching labels are added
# (see design-docs/release-notes-generation.md).
CATEGORY_LABELS = (
    ("Behavior Changes", {"breaking-change"}),
    ("Cluster and Replication", {"cluster"}),
    ("Performance and Efficiency improvements", {"performance"}),
    ("Bug Fixes", {"bug"}),
    ("New Features and enhanced behavior", {"enhancement"}),
)

SKIP_LABELS = {"no-release-notes"}
INCLUDE_LABEL = "release-notes"
SECTION_RE = re.compile(r"(?m)^Valkey \d+\.\d+\.\d+(?:[^\n]*)\n-+\n")
PR_RE = re.compile(r"\(#(\d+)\)")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")


def run_git(args):
    return subprocess.check_output(["git"] + args, text=True).strip()


def default_date():
    # Avoid strftime("%-d"): the no-pad day flag is glibc-only and raises on
    # macOS/BSD. Format the day as a plain integer instead.
    today = datetime.date.today()
    return f"{today.strftime('%B')} {today.day}, {today.year}"


def parse_version(version):
    match = VERSION_RE.match(version)
    if not match:
        raise ValueError("version must look like X.Y.Z or X.Y.Z-rcN")
    major, minor, patch = (int(match.group(i)) for i in range(1, 4))
    stage = match.group(4) or "ga"
    return major, minor, patch, stage


def version_num(version):
    major, minor, patch, _ = parse_version(version)
    if major > 255 or minor > 255 or patch > 255:
        raise ValueError("version components must fit into one byte")
    return f"0x00{major:02x}{minor:02x}{patch:02x}"


def release_title(version):
    major, minor, patch, stage = parse_version(version)
    if stage != "ga":
        return version
    if patch == 0:
        return f"{major}.{minor}.{patch} GA"
    return version


def release_family(version):
    major, minor, _, _ = parse_version(version)
    return f"{major}.{minor}"


def github_request(repo, path, token):
    url = f"https://api.github.com/repos/{repo}{path}"
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "valkey-prepare-release",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        details = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"GitHub API request failed: {url}: {error.code} {details}") from error


def commit_subjects(previous_tag):
    output = run_git(["log", "--reverse", "--format=%H%x00%s", f"{previous_tag}..HEAD"])
    if not output:
        return []
    commits = []
    for line in output.splitlines():
        sha, subject = line.split("\0", 1)
        commits.append((sha, subject))
    return commits


def pr_numbers_from_subject(subject):
    numbers = PR_RE.findall(subject)
    if not numbers:
        return []
    # Backport squash commits often end with "(#original) (#backport)".
    # The original PR usually has the useful release-note labels and wording.
    return [int(numbers[0])]


def release_note_override(body):
    if not body:
        return None
    for line in body.splitlines():
        match = re.match(r"\s*release-note\s*:\s*(.+?)\s*$", line, flags=re.IGNORECASE)
        if match:
            return match.group(1)
    return None


def labels_for(pr):
    return {label["name"].lower() for label in pr.get("labels", [])}


def should_include(pr):
    # Inclusion is label-driven only. Security fixes are intentionally not
    # auto-detected from CVE strings: they usually land via embargoed PRs the
    # generator never sees, and a CVE mentioned in passing should not pull an
    # unrelated PR in. The release author adds security entries by hand.
    labels = labels_for(pr)
    if labels & SKIP_LABELS:
        return False
    return INCLUDE_LABEL in labels


def category_for(pr):
    labels = labels_for(pr)
    for category, category_labels in CATEGORY_LABELS:
        if labels & category_labels:
            return category
    return "Other Changes"


def strip_trailing_pr_number(title):
    return re.sub(r"\s+\(#\d+\)$", "", title).strip()


def normalize_note(note):
    return note.strip().rstrip(".")


def entry_text(pr):
    # Match the existing 00-RELEASENOTES style: "* <note> (#PR)". Contributor
    # credits live in the separate thank-you section, not inline per entry.
    note = normalize_note(release_note_override(pr.get("body")) or strip_trailing_pr_number(pr["title"]))
    return f"{note} (#{pr['number']})"


def collect_prs(repo, previous_tag, token):
    prs = OrderedDict()
    for sha, subject in commit_subjects(previous_tag):
        numbers = pr_numbers_from_subject(subject)
        if not numbers:
            for pr in github_request(repo, f"/commits/{sha}/pulls", token):
                numbers.append(pr["number"])
        for number in numbers:
            if number not in prs:
                prs[number] = github_request(repo, f"/pulls/{number}", token)
    return list(prs.values())


def grouped_entries(prs):
    groups = OrderedDict()
    for category, _ in CATEGORY_LABELS:
        groups[category] = []
    groups["Other Changes"] = []

    for pr in prs:
        if not should_include(pr):
            continue
        groups.setdefault(category_for(pr), []).append(entry_text(pr))

    return OrderedDict((category, entries) for category, entries in groups.items() if entries)


def urgency_table():
    lines = [
        "Upgrade urgency levels:",
        "",
        "| Level    | Meaning                                                             |",
        "|----------|---------------------------------------------------------------------|",
    ]
    for level, meaning in URGENCY_TEXT.items():
        lines.append(f"| {level:<8} | {meaning:<67} |")
    return "\n".join(lines)


def render_section(version, date, urgency, groups):
    title = f"Valkey {release_title(version)} - {date}"
    lines = [
        title,
        "-" * len(title),
        "",
        f"Upgrade urgency {urgency}: {URGENCY_SUMMARY[urgency]}",
        "",
    ]
    if not groups:
        lines += [
            "### Release notes",
            "",
            "* No release-note labeled pull requests were found in this range.",
            "",
        ]
        return "\n".join(lines)

    for category, entries in groups.items():
        lines.append(f"### {category}")
        lines.append("")
        lines.extend(f"* {entry}" for entry in entries)
        lines.append("")
    return "\n".join(lines)


def render_full_notes(existing, version, date, urgency, groups):
    section = render_section(version, date, urgency, groups)
    if SECTION_RE.search(existing):
        return SECTION_RE.sub(section + "\n\n\\g<0>", existing, count=1)

    # No existing section found, so we are minting the file header. That is only
    # valid when starting a fresh minor (patch == 0, i.e. replacing the unstable
    # placeholder on a newly-cut branch). For a patch release the branch must
    # already carry a release-notes section; not finding one means we are on the
    # wrong branch or the file is unexpectedly empty.
    _, _, patch, _ = parse_version(version)
    if patch != 0:
        raise RuntimeError(
            f"no existing release-notes section found in {version!r} target; "
            "refusing to create a new file header for a patch release"
        )

    family = release_family(version)
    header = f"Valkey {family} release notes"
    return "\n".join([
        header,
        "=" * len(header),
        "",
        urgency_table(),
        "",
        section,
        "",
    ])


def update_version_header(path, version):
    _, _, _, stage = parse_version(version)
    stable_version = version.split("-", 1)[0]
    text = open(path, encoding="utf-8").read()
    replacements = {
        r'#define VALKEY_VERSION "[^"]+"': f'#define VALKEY_VERSION "{stable_version}"',
        r"#define VALKEY_VERSION_NUM 0x[0-9a-fA-F]+": f"#define VALKEY_VERSION_NUM {version_num(version)}",
        r'#define VALKEY_RELEASE_STAGE "[^"]+"': f'#define VALKEY_RELEASE_STAGE "{stage}"',
    }
    for pattern, replacement in replacements.items():
        text, count = re.subn(pattern, replacement, text, count=1)
        if count != 1:
            raise RuntimeError(f"failed to update {pattern} in {path}")
    with open(path, "w", encoding="utf-8") as file:
        file.write(text)


def latest_reachable_tag():
    output = run_git(["tag", "--merged", "HEAD", "--sort=-v:refname"])
    tags = [tag for tag in output.splitlines() if re.match(r"^\d+\.\d+\.\d+(?:-rc\d+)?$", tag)]
    if not tags:
        raise RuntimeError("could not determine previous tag; pass --previous-tag")
    return tags[0]


def parse_args():
    parser = argparse.ArgumentParser(description="Generate Valkey release notes and bump src/version.h.")
    parser.add_argument("--version", required=True, help="Release version, for example 9.2.0-rc1 or 9.0.5.")
    parser.add_argument("--previous-tag", help="Git tag to diff from. Defaults to the latest reachable release tag.")
    parser.add_argument("--urgency", choices=tuple(URGENCY_TEXT.keys()), default="LOW")
    parser.add_argument("--date", default=default_date())
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "valkey-io/valkey"))
    parser.add_argument("--notes-path", default="00-RELEASENOTES")
    parser.add_argument("--version-path", default="src/version.h")
    parser.add_argument("--pr-json", help="Use PR metadata from a JSON file instead of the GitHub API.")
    parser.add_argument("--dry-run", action="store_true", help="Print generated release notes instead of writing files.")
    return parser.parse_args()


def main():
    args = parse_args()
    parse_version(args.version)
    previous_tag = args.previous_tag or latest_reachable_tag()

    if args.pr_json:
        with open(args.pr_json, encoding="utf-8") as file:
            prs = json.load(file)
    else:
        prs = collect_prs(args.repo, previous_tag, os.environ.get("GITHUB_TOKEN"))

    groups = grouped_entries(prs)
    existing_notes = open(args.notes_path, encoding="utf-8").read()
    notes = render_full_notes(existing_notes, args.version, args.date, args.urgency, groups)

    if args.dry_run:
        print(notes)
        return 0

    with open(args.notes_path, "w", encoding="utf-8") as file:
        file.write(notes)
    update_version_header(args.version_path, args.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
