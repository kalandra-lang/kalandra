"""Validate version tags and emit GitHub Actions outputs without shell evaluation."""
import os
import re
import sys
from pathlib import Path

CORE = r"(?:0|[1-9][0-9]*)"
IDENT = r"[0-9A-Za-z-]+"
VERSION = re.compile(rf"v({CORE}\.{CORE}\.{CORE})(?:-({IDENT}(?:\.{IDENT})*))?(?:\+({IDENT}(?:\.{IDENT})*))?", re.ASCII)

def classify(tag):
    match = VERSION.fullmatch(tag)
    if not match:
        raise ValueError(f"Not a SemVer version tag: {tag!r}")
    prerelease = match.group(2)
    if prerelease and any(part.isdigit() and len(part) > 1 and part.startswith("0") for part in prerelease.split(".")):
        raise ValueError("Numeric prerelease identifiers must not have leading zeros")
    return {"tag": tag, "version": tag[1:], "prerelease": str(prerelease is not None).lower()}

def project_version(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    classify("v" + version)
    return version


def validate_project_tag(tag, version=None):
    values = classify(tag)
    expected = project_version() if version is None else version
    if values["version"] != expected:
        raise ValueError(f"Release tag {tag!r} does not match VERSION {expected!r}")
    return values


def release_notes(version, root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    notes = (root / "docs/releases" / f"{version}.md").read_text(encoding="utf-8")
    if not notes.strip() or not re.match(rf"^# Kalandra {re.escape(version)}(?:\s|$)", notes):
        raise ValueError("Release notes must begin with a heading for the exact release version")
    return notes


if __name__ == "__main__":
    try:
        values = validate_project_tag(os.environ["RELEASE_TAG"])
        release_notes(values["version"])
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write("".join(f"{key}={value}\n" for key, value in values.items()))
