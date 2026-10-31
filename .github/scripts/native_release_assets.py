"""Validate the complete release asset set before creating any GitHub release."""
import hashlib
import json
from pathlib import Path


ARCHIVES = tuple(f"kalandra-native-{kind}-macos-arm64.tar.gz" for kind in
                 ("cpu", "editor-tools", "metal", "cpu-standalone", "editor-tools-standalone", "metal-standalone")) + tuple(
    f"kalandra-native-cpu-standalone-{platform}-x86_64.tar.gz" for platform in ("linux", "windows")) + ("kalandra-native-cpu-standalone-linux-arm64.tar.gz", "kalandra-native-cpu-standalone-windows-arm64.tar.gz")
IMAGES = tuple(f"kalandra-native-{kind}-standalone.dmg" for kind in ("cpu", "editor-tools", "metal"))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_assets(unsigned=Path("release-assets"), signed=Path("signed-assets")):
    names = {*ARCHIVES, "kalandra-native.vsix"}
    expected = names | {name + ".sha256" for name in names}
    if {path.name for path in unsigned.iterdir()} != expected:
        raise ValueError("Unsigned release asset set is incomplete or contains unexpected files")
    assets = sorted(unsigned.iterdir())
    signed_files = [path for path in signed.rglob("*") if path.is_file()]
    expected_signed = {name + suffix for name in IMAGES
                       for suffix in ("", ".sha256", ".notarization.json")}
    # Path.with_suffix replaces .dmg in receipt names.
    expected_signed = {name.replace(".dmg.notarization", ".notarization") for name in expected_signed}
    if len(signed_files) != len(expected_signed) or {path.name for path in signed_files} != expected_signed:
        raise ValueError("All three notarized distributions and their receipts are required")
    for path in [*assets, *signed_files]:
        if path.is_symlink() or not path.is_file():
            raise ValueError("Release assets must be regular files")
        if path.name.endswith(".sha256"):
            target = path.with_suffix("")
            if path.read_text().split() != [digest(target), target.name]:
                raise ValueError("Release asset checksum mismatch")
    for image in (path for path in signed_files if path.suffix == ".dmg"):
        record = json.loads(image.with_suffix(".notarization.json").read_text())
        archive = unsigned / (image.stem + "-macos-arm64.tar.gz")
        if (record.get("notarization", {}).get("status") != "Accepted" or
                not record.get("stapleValidated") or not record.get("gatekeeperAssessmentPassed") or
                not record.get("signedRuntimeQualificationPassed") or
                record.get("artifactSHA256") != digest(image) or
                record.get("inputArchiveSHA256") != digest(archive)):
            raise ValueError("Notarized artifact receipt does not match qualified assets")
    return [*assets, *sorted(signed_files)]
