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

    def test_grouped_entries_include_labeled_and_cve_prs(self):
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
                "title": "Fix memory issue (CVE-2026-12345)",
                "body": "",
                "labels": [],
                "user": {"login": "carol"},
            },
        ]

        groups = prepare_release.grouped_entries(prs)

        self.assertEqual(groups["Security fixes"], ["Fix memory issue (CVE-2026-12345) by @carol (#12)"])
        self.assertEqual(groups["Cluster and Replication"], ["Add cluster bus metric by @alice (#10)"])
        self.assertNotIn("Bug Fixes", groups)

    def test_no_release_notes_label_skips_entry(self):
        prs = [
            {
                "number": 30,
                "title": "Fix visible bug",
                "body": "",
                "labels": [{"name": "release-notes"}, {"name": "no-release-notes"}, {"name": "bug"}],
                "user": {"login": "alice"},
            },
        ]

        self.assertEqual(prepare_release.grouped_entries(prs), {})

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
            "Improve client-visible latency during rehashing by @alice (#20)",
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
            {"Bug Fixes": ["Fix a bug by @alice (#1)"]},
        )

        self.assertIn("Valkey 9.2 release notes\n========================", notes)
        self.assertIn("Valkey 9.2.0-rc1 - June 10, 2026", notes)
        self.assertIn("* Fix a bug by @alice (#1)", notes)


if __name__ == "__main__":
    unittest.main()
