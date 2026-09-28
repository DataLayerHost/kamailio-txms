#!/usr/bin/env python3
"""Resolve GitHub's latest stable libtxms release to its exact source commit."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPOSITORY = "DataLayerHost/libtxms"
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"


def resolve(run=subprocess.check_output):
	try:
		release = json.loads(run(["gh", "api", f"repos/{REPOSITORY}/releases/latest"], text=True))
	except subprocess.CalledProcessError as error:
		raise ValueError("Cannot resolve latest libtxms release. Publish a stable library release first and verify repository access.") from error
	tag = release.get("tag_name", "")
	if release.get("draft") or release.get("prerelease") or not re.fullmatch(VERSION, tag):
		raise ValueError("Latest libtxms release must have a stable MAJOR.MINOR.PATCH tag without v")
	ref = f"refs/tags/{tag}"
	lines = run(["git", "ls-remote", f"https://github.com/{REPOSITORY}.git", ref, ref + "^{}"], text=True)
	refs = dict((name, sha) for sha, name in (line.split() for line in lines.splitlines()))
	commit = refs.get(ref + "^{}", refs.get(ref, ""))
	if not re.fullmatch(r"[0-9a-f]{40}", commit):
		raise ValueError(f"Published libtxms release {tag} has no resolvable Git tag")
	return {"version": tag, "commit": commit}


if __name__ == "__main__":
	try:
		result = resolve()
	except (ValueError, subprocess.CalledProcessError) as error:
		sys.exit(str(error))
	print(json.dumps(result))
	if os.environ.get("GITHUB_OUTPUT"):
		with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
			for key, value in result.items():
				output.write(f"{key}={value}\n")
