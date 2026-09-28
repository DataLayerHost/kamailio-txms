#!/usr/bin/env python3
import hashlib
import json
from unittest.mock import Mock
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from release import build_release
from latest_libtxms import resolve


class ReleaseTests(unittest.TestCase):
	def setUp(self):
		self.temp = tempfile.TemporaryDirectory(prefix="txms-release-test-")
		self.root = Path(self.temp.name)
		self.git("init", "-q")
		self.git("config", "user.email", "test@example.invalid")
		self.git("config", "user.name", "Release test")
		(self.root / "CMakeLists.txt").write_text("project(txms VERSION 0.1.0 LANGUAGES C)\n")
		(self.root / "LICENSE").write_text("Test fixture license\n")
		self.git("add", ".")
		self.git("commit", "-qm", "fixture")
		self.git("tag", "0.1.0")

	def tearDown(self):
		self.temp.cleanup()

	def git(self, *args):
		return subprocess.check_output(["git", "-C", str(self.root), *args], text=True).strip()

	def test_archive_is_tagged_tree_with_checksums(self):
		(self.root / "conan.lock").write_text("must not enter release\n")
		out = self.root / "dist"
		metadata = build_release(self.root, "libtxms", "0.1.0", out)
		archive = out / "libtxms-0.1.0.tar.gz"
		with tarfile.open(archive) as source:
			self.assertIn("libtxms-0.1.0/LICENSE", source.getnames())
			self.assertNotIn("libtxms-0.1.0/conan.lock", source.getnames())
		self.assertEqual(metadata["commit"], self.git("rev-parse", "HEAD"))
		self.assertEqual(metadata["source_sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())

	def test_gateway_records_tested_dependency(self):
		metadata = build_release(self.root, "kamailio-txms", "0.1.0", self.root / "dist", "0.2.0", "a" * 40)
		self.assertEqual(metadata["libtxms_version"], "0.2.0")
		self.assertEqual(metadata["libtxms_commit"], "a" * 40)

	def test_mismatched_or_unsafe_version_rejected(self):
		self.git("tag", "0.2.0")
		for tag in ("0.2.0", "v0.1.0", "0.1.0-rc1", "0.1.0;echo bad", "00.1.0"):
			with self.subTest(tag=tag), self.assertRaises(ValueError):
				build_release(self.root, "libtxms", tag, self.root / "dist")

	def test_dirty_tracked_file_rejected(self):
		(self.root / "LICENSE").write_text("changed\n")
		with self.assertRaises(ValueError):
			build_release(self.root, "libtxms", "0.1.0", self.root / "dist")

	def test_checkout_must_match_tag(self):
		(self.root / "LICENSE").write_text("changed\n")
		self.git("commit", "-qam", "changed")
		with self.assertRaises(ValueError):
			build_release(self.root, "libtxms", "0.1.0", self.root / "dist")


	def test_invalid_dependency_revision_rejected(self):
		for version, commit in ((None,None), ("0.2.0","main"), ("v0.2.0","a"*40)):
			with self.subTest(version=version,commit=commit), self.assertRaises(ValueError):
				build_release(self.root, "kamailio-txms", "0.1.0", self.root / "dist", version, commit)


class LatestReleaseTests(unittest.TestCase):
	def test_lightweight_and_annotated_tags(self):
		for annotated in (False,True):
			refs = "a"*40 + "\trefs/tags/0.2.0\n"
			if annotated: refs += "b"*40 + "\trefs/tags/0.2.0^{}\n"
			run = Mock(side_effect=[json.dumps({"tag_name":"0.2.0","target_commitish":"main"}),refs])
			self.assertEqual(resolve(run), {"version":"0.2.0","commit":("b" if annotated else "a")*40})
			self.assertEqual(run.call_count,2)

	def test_missing_release_has_actionable_error(self):
		run=Mock(side_effect=subprocess.CalledProcessError(1,["gh"]))
		with self.assertRaisesRegex(ValueError,"Publish a stable library release first"):
			resolve(run)
		self.assertEqual(run.call_count,1)

	def test_invalid_release_or_missing_tag_rejected(self):
		for release in ({"tag_name":"v0.2.0"},{"tag_name":"0.2.0","draft":True},{"tag_name":"0.2.0","prerelease":True},{"tag_name":"0.2.0"}):
			with self.subTest(release=release), self.assertRaises(ValueError):
				resolve(Mock(side_effect=[json.dumps(release),""]))


class WorkflowTests(unittest.TestCase):
	def test_published_release_uploads_only_after_tests(self):
		workflow = yaml.safe_load((ROOT / ".github/workflows/release.yml").read_text())
		# YAML 1.1 parses the Actions `on` key as True.
		self.assertEqual(workflow[True], {"release": {"types": ["published"]}})
		jobs = workflow["jobs"]
		self.assertEqual(jobs["tests"]["uses"], "./.github/workflows/ci.yml")
		self.assertEqual(jobs["assets"]["needs"], "tests")
		self.assertEqual(set(jobs["publish"]["needs"]), {"tests", "assets"})
		self.assertEqual(jobs["publish"]["env"]["TAG"], "${{ github.event.release.tag_name }}")
		commands = "\n".join(step.get("run", "") for step in jobs["publish"]["steps"])
		self.assertIn('gh release upload "$TAG"', commands)
		self.assertNotIn("gh release create", commands)


	def test_gateway_uses_one_resolved_release_for_all_builds_and_metadata(self):
		workflow = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())
		for name in ("core", "kamailio", "fuzz"):
			job = workflow["jobs"][name]
			self.assertEqual(job["needs"], "dependency")
			checkout = next(step for step in job["steps"] if step.get("with", {}).get("repository") == "DataLayerHost/libtxms")
			self.assertEqual(checkout["with"]["ref"], "${{ needs.dependency.outputs.commit }}")
			self.assertEqual(job["env"]["LIBTXMS_EXPECTED_VERSION"], "${{ needs.dependency.outputs.version }}")
		outputs=workflow[True]["workflow_call"]["outputs"]
		self.assertEqual(outputs["libtxms_commit"]["value"], "${{ jobs.dependency.outputs.commit }}")
		release=yaml.safe_load((ROOT / ".github/workflows/release.yml").read_text())
		self.assertEqual(release["jobs"]["assets"]["env"]["LIBTXMS_COMMIT"], "${{ needs.tests.outputs.libtxms_commit }}")

if __name__ == "__main__":
	unittest.main()
