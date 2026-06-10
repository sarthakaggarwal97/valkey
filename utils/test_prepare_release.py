#!/usr/bin/env python3
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

import prepare_release


class PrepareReleaseTest(unittest.TestCase):
    def test_version_header_values_for_rc(self):
        self.assertEqual(prepare_release.version_num("9.2.0-rc1"), "0x00090200")
        self.assertEqual(prepare_release.release_title("9.2.0-rc1"), "9.2.0-rc1")

    def test_version_header_values_for_ga_minor(self):
        self.assertEqual(prepare_release.version_num("9.1.0"), "0x00090100")
        self.assertEqual(prepare_release.release_title("9.1.0"), "9.1.0 GA")

    def test_grouped_entries_include_labeled_prs(self):
        prs = [
            {
                "number": 10,
                "title": "Add cluster bus metric",
                "body": "",
                "labels": [{"name": "release-notes"}, {"name": "cluster"}],
                "user": {"login": "alice"},
            },
            {
                "number": 11,
                "title": "Internal refactor",
                "body": "",
                "labels": [{"name": "bug"}],
                "user": {"login": "bob"},
            },
            {
                "number": 12,
                "title": "Fix a visible bug",
                "body": "",
                "labels": [{"name": "release-notes"}, {"name": "bug"}],
                "user": {"login": "carol"},
            },
        ]

        groups = prepare_release.grouped_entries(prs)

        # #10 is labeled and categorized; #11 is unlabeled (no release-notes) so
        # it is excluded; #12 is labeled and lands under Bug Fixes.
        self.assertEqual(groups["Cluster and Replication"], ["Add cluster bus metric (#10)"])
        self.assertEqual(groups["Bug Fixes"], ["Fix a visible bug (#12)"])

    def test_cve_pr_not_auto_included(self):
        # A CVE mentioned in the body must NOT pull an unlabeled PR in. Security
        # entries are label-driven and otherwise added by hand at release time.
        prs = [
            {
                "number": 13,
                "title": "Fix memory issue",
                "body": "Related to CVE-2026-12345 reported upstream.",
                "labels": [],
                "user": {"login": "carol"},
            },
        ]

        self.assertEqual(prepare_release.grouped_entries(prs), {})

    def test_unlabeled_pr_skipped(self):
        prs = [
            {
                "number": 30,
                "title": "Fix visible bug",
                "body": "",
                "labels": [{"name": "bug"}],
                "user": {"login": "alice"},
            },
        ]

        self.assertEqual(prepare_release.grouped_entries(prs), {})

    def test_category_priority_first_match_wins(self):
        # A PR with both breaking-change and bug files under Behavior Changes,
        # since that row precedes Bug Fixes in CATEGORY_LABELS.
        pr = {
            "number": 40,
            "title": "Change default eviction policy",
            "body": "",
            "labels": [{"name": "release-notes"}, {"name": "bug"}, {"name": "breaking-change"}],
            "user": {"login": "alice"},
        }
        self.assertEqual(prepare_release.category_for(pr), "Behavior Changes")

    def test_release_note_override(self):
        pr = {
            "number": 20,
            "title": "Make internal thing nicer",
            "body": "Release-note: Improve client-visible latency during rehashing.",
            "labels": [{"name": "release-notes"}, {"name": "performance"}],
            "user": {"login": "alice"},
        }

        self.assertEqual(
            prepare_release.entry_text(pr),
            "Improve client-visible latency during rehashing (#20)",
        )

    def test_backport_subject_uses_original_pr_number(self):
        self.assertEqual(
            prepare_release.pr_numbers_from_subject("Backport 9.0: Fix visible bug (#1234) (#5678)"),
            [1234],
        )

    def test_render_replaces_unstable_placeholder(self):
        notes = prepare_release.render_full_notes(
            "Hello! This file is just a placeholder.\n",
            "9.2.0-rc1",
            "June 10, 2026",
            "LOW",
            {"Bug Fixes": ["Fix a bug (#1)"]},
        )

        self.assertIn("Valkey 9.2 release notes\n========================", notes)
        self.assertIn("Valkey 9.2.0-rc1 - June 10, 2026", notes)
        self.assertIn("* Fix a bug (#1)", notes)

    def test_patch_release_requires_existing_section(self):
        # On a patch release the file must already contain a section to splice
        # into. Minting a fresh header would be wrong, so it must raise.
        with self.assertRaises(RuntimeError):
            prepare_release.render_full_notes(
                "Hello! This file is just a placeholder.\n",
                "9.0.5",
                "June 10, 2026",
                "LOW",
                {"Bug Fixes": ["Fix a bug (#1)"]},
            )


if __name__ == "__main__":
    unittest.main()
