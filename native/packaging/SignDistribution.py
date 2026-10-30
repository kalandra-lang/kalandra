"""Sign a staging copy of a standalone artifact using a runner Keychain identity.

Optionally notarize/staple a DMG through a named notarytool Keychain profile.
No private key or password is read by this script, exported, or put in arguments.
The input archive is immutable and a delivery is published only after validation.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

from BundleMachO import digest, macho
from CreateArchive import create_archive
from StandaloneTests import qualify


def run(*args, timeout=120):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    return result.stdout, result.stderr


def extract_verified(archive, stage):
    expected = archive.with_suffix(archive.suffix + ".sha256").read_text().split()[0]
    if digest(archive) != expected:
        raise ValueError("Input archive checksum mismatch")
    with tarfile.open(archive) as package:
        members = package.getmembers()
        names = [member.name for member in members]
        if len(names) != len(set(names)) or any(
                Path(name).is_absolute() or ".." in Path(name).parts for name in names):
            raise ValueError("Unsafe or duplicate archive member")
        roots = {Path(member.name).parts[0] for member in members}
        allowed = {"kalandra-native-editor-tools-standalone", "kalandra-native-cpu-standalone",
                   "kalandra-native-metal-standalone"}
        if len(roots) != 1 or not roots <= allowed or any(
                not (member.isdir() or member.isfile()) for member in members):
            raise ValueError("Only qualified standalone native packages can be signed")
        package.extractall(stage, filter="data")
    prefix = (stage / roots.pop()).resolve()
    listed = set()
    for line in (prefix / "SHA256SUMS").read_text().splitlines():
        checksum, name = line.split("  ", 1)
        target = (prefix / name).resolve()
        if name in listed or not target.is_relative_to(prefix) or digest(target) != checksum:
            raise ValueError("Extracted package inventory mismatch")
        listed.add(name)
    files = {str(path.relative_to(prefix)) for path in prefix.rglob("*") if path.is_file()}
    if files != listed | {"SHA256SUMS"}:
        raise ValueError("Extracted package contains unlisted files")
    return prefix


def sign(archive, output, identity, team, profile=None, keychain=None):
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", identity) or not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise ValueError("Supply the certificate SHA-1 identity and ten-character Team ID")
    if output.exists():
        raise ValueError("Output directory already exists; existing deliveries are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix="sign-stage-") as temporary:
        work = Path(temporary).resolve()
        prefix = extract_verified(archive, work / "contents")
        signature = ["/usr/bin/codesign", "--force", "--sign", identity, "--timestamp"]
        if keychain:
            signature += ["--keychain", str(keychain)]
        binaries = [path for path in sorted(prefix.rglob("*")) if path.is_file() and macho(path)]
        if not binaries:
            raise ValueError("Standalone package contains no Mach-O files")
        print(f"Signing {len(binaries)} Mach-O files using the runner Keychain identity", flush=True)
        for path in binaries:
            run(*signature, "--options", "runtime", path)
            run("/usr/bin/codesign", "--verify", "--strict", path)
            _, details = run("/usr/bin/codesign", "--display", "--verbose=4", path)
            if (f"TeamIdentifier={team}" not in details or
                    "Authority=Developer ID Application:" not in details or
                    "(runtime)" not in details):
                raise RuntimeError("Signature is not the expected hardened Developer ID identity")
        metadata = prefix / "lib/kalandra/toolchain/standalone.json"
        record = json.loads(metadata.read_text())
        record["codeSignature"] = "Developer ID Application"
        record["teamIdentifier"] = team
        record.pop("notarized", None)
        record["notarization"] = "Verify the distribution container ticket and external receipt"
        for dependency in record["dependencies"]:
            dependency["bundledSHA256"] = digest(prefix / dependency["path"])
        metadata.write_text(json.dumps(record, indent=2) + "\n")
        (prefix / "SHA256SUMS").write_text("".join(
            f"{digest(path)}  {path.relative_to(prefix)}\n" for path in sorted(prefix.rglob("*"))
            if path.is_file() and path != prefix / "SHA256SUMS"))
        # Re-run relocation, protocols and CPU execution with the hardened
        # signatures before paying for an Apple submission.
        qualified_archive = work / "signed.tar.gz"
        create_archive(prefix, qualified_archive)
        qualified_archive.with_suffix(".gz.sha256").write_text(digest(qualified_archive) + "\n")
        # Delivery staging may be inside the checkout. Acceptance deliberately
        # denies native children access to the checkout, so extract/run its
        # fixtures in the system temporary directory, just like unsigned tests.
        with tempfile.TemporaryDirectory(prefix="kalandra-signed-acceptance-") as acceptance:
            kind = "metal" if "metal" in prefix.name else "cpu" if "cpu" in prefix.name else "editor"
            qualify(qualified_archive, kind,
                    Path(acceptance).resolve())
        delivery = work / "delivery"
        delivery.mkdir()
        image = delivery / (prefix.name + ".dmg")
        run("/usr/bin/hdiutil", "create", "-format", "UDZO", "-volname", "Kalandra Native",
            "-srcfolder", prefix.parent, image, timeout=300)
        run(*signature, image)
        run("/usr/bin/codesign", "--verify", "--strict", image)
        receipt = {"inputArchiveSHA256": digest(archive), "teamIdentifier": team,
                   "certificateSHA1": identity.upper(), "signedMachOFiles": len(binaries),
                   "signedRuntimeQualificationPassed": True,
                   "notarization": "not requested"}
        if profile:
            print("Submitting signed DMG using the named Keychain profile", flush=True)
            credentials = ["--keychain-profile", profile]
            if keychain:
                credentials += ["--keychain", str(keychain)]
            response, _ = run("/usr/bin/xcrun", "notarytool", "submit", image,
                              *credentials, "--output-format", "json",
                              "--wait", "--timeout", "20m", timeout=1500)
            submission = json.loads(response)
            if submission.get("status") != "Accepted":
                raise RuntimeError("Notarization did not succeed: " + json.dumps(submission))
            run("/usr/bin/xcrun", "stapler", "staple", image)
            run("/usr/bin/xcrun", "stapler", "validate", image)
            run("/usr/sbin/spctl", "--assess", "--type", "open", "--context",
                "context:primary-signature", "--verbose=2", image)
            receipt["notarization"] = submission
            receipt["stapleValidated"] = receipt["gatekeeperAssessmentPassed"] = True
        receipt["artifactSHA256"] = digest(image)
        image.with_suffix(".dmg.sha256").write_text(f"{receipt['artifactSHA256']}  {image.name}\n")
        image.with_suffix(".notarization.json").write_text(json.dumps(receipt, indent=2) + "\n")
        delivery.rename(output)
        print(f"Validated delivery: {output}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--team-id", required=True)
    parser.add_argument("--notary-profile")
    parser.add_argument("--keychain", type=Path)
    args = parser.parse_args()
    sign(args.archive.resolve(), args.output.resolve(), args.identity, args.team_id,
         args.notary_profile, args.keychain)
