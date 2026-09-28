#!/usr/bin/env python3
"""Create checksummed source assets from the exact tagged commit; never publish."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"

def build_release(root, project, tag, output):
	root, output = Path(root).resolve(), Path(output).resolve()
	if project not in ("libtxms", "kamailio-txms") or not re.fullmatch(VERSION, tag):
		raise ValueError("Use a stable MAJOR.MINOR.PATCH tag and a known project")
	def git(*args):
		return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()
	commit = git("rev-parse", "HEAD")
	if git("rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
		raise ValueError("The release tag must point to the checked-out commit")
	cmake = git("show", f"{commit}:CMakeLists.txt")
	match = re.search(r"project\(\w+ VERSION (" + VERSION + r")\b", cmake)
	if not match or match.group(1) != tag:
		raise ValueError("Tag and CMake project version differ")
	if git("status", "--porcelain", "--untracked-files=no"):
		raise ValueError("Tracked files are dirty; commit changes before releasing")
	output.mkdir(parents=True, exist_ok=True)
	archive = output / f"{project}-{tag}.tar.gz"
	subprocess.run(["git", "-C", str(root), "archive", "--format=tar.gz", f"--prefix={project}-{tag}/", f"--output={archive}", commit], check=True)
	digest = hashlib.sha256(archive.read_bytes()).hexdigest()
	metadata = {"project": project, "version": tag, "tag": tag, "commit": commit, "source_sha256": digest}
	if project == "kamailio-txms":
		dependency = git("show", f"{commit}:LIBTXMS_VERSION")
		if not re.fullmatch(VERSION, dependency):
			raise ValueError("Invalid LIBTXMS_VERSION")
		metadata["libtxms_tag"] = dependency
	(output / "release.json").write_text(json.dumps(metadata, indent=2) + "\n")
	(output / "SHA256SUMS").write_text(f"{digest}  {archive.name}\n")
	return metadata

if __name__ == "__main__":
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--project", required=True)
	parser.add_argument("--tag", required=True)
	parser.add_argument("--root", default=".")
	parser.add_argument("--output", default="dist")
	args = parser.parse_args()
	print(json.dumps(build_release(args.root, args.project, args.tag, args.output), indent=2))
