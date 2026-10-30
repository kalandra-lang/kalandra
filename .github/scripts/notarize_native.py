"""Import CI credentials into an ephemeral Keychain and notarize hosted builds.

Secrets are supplied only by GitHub Actions environment variables. Never echo
subprocess arguments or output from credential-management commands on failure.
"""
import base64
import argparse
import os
from pathlib import Path
import platform
import re
import secrets
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "native/packaging"))
from SignDistribution import sign


def require_ci():
    if os.environ.get("GITHUB_ACTIONS") != "true" or platform.system() != "Darwin":
        raise RuntimeError("Signing credentials may only be imported on a GitHub macOS runner")


def private_command(*args):
    try:
        result = subprocess.run(list(map(str, args)), capture_output=True, timeout=180)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Credential operation timed out ({Path(args[0]).name}); arguments suppressed") from None
    if result.returncode and "notarytool" in args:
        # Classify known failures; never print Apple's raw response or credentials.
        response = (result.stdout + result.stderr).decode("utf-8", errors="replace").lower()
        if "agreement" in response and any(word in response for word in ("missing", "expired", "signed")):
            raise RuntimeError("Apple notarization access denied: a required developer agreement is missing or expired")
        if "invalid or inaccessible developer team" in response:
            raise RuntimeError("Apple notarization access denied: Apple reports the requested team as invalid or inaccessible")
        status = re.search(r"(?:http status code|status code)[: =]+([1-5][0-9]{2})\b", response)
        if status:
            raise RuntimeError(f"Apple notarization authentication failed (HTTP {status[1]}); response suppressed")
        if "unknown option" in response or "unexpected argument" in response:
            raise RuntimeError("Runner notarytool rejected an unsupported command option; response suppressed")
        if "keychain" in response and ("not found" in response or "could not" in response):
            raise RuntimeError("Notarytool could not access its temporary Keychain; response suppressed")
        if "invalid" in response and ("password" in response or "credentials" in response):
            raise RuntimeError("Apple rejected the notarization login credentials; response suppressed")
        raise RuntimeError(f"Notarytool credential validation failed (exit {result.returncode}); response suppressed")
    if result.returncode and Path(args[0]).name == "codesign":
        response = (result.stdout + result.stderr).decode("utf-8", errors="replace").lower()
        if "unable to build chain" in response:
            raise RuntimeError("Signing failed: certificate trust chain is incomplete on the runner")
        if "could not be found in the keychain" in response or "no identity found" in response:
            raise RuntimeError("Signing failed: configured certificate identity is absent from the imported Keychain")
        if "timestamp" in response:
            raise RuntimeError("Signing failed: trusted timestamp service error")
        if "errsecinternalcomponent" in response or "interaction is not allowed" in response:
            raise RuntimeError("Signing failed: runner Keychain access or certificate trust error")
        raise RuntimeError(f"Signing operation failed (exit {result.returncode}); response suppressed")
    if result.returncode:
        raise RuntimeError(f"Credential setup failed ({Path(args[0]).name}); command output suppressed")
    return result.stdout


def main(check_only=False):
    require_ci()
    names = ("KALANDRA_SIGNING_P12_BASE64", "KALANDRA_SIGNING_P12_PASSWORD",
             "KALANDRA_APPLE_ID", "KALANDRA_APPLE_APP_PASSWORD",
             "KALANDRA_APPLE_TEAM_ID", "KALANDRA_SIGNING_IDENTITY")
    missing = [name for name in names if not os.environ.get(name)]
    if missing:
        raise RuntimeError("Configure GitHub signing secrets/variables first: " + ", ".join(missing))
    root = Path(os.environ["RUNNER_TEMP"]) / "kalandra-signing"
    root.mkdir(mode=0o700)  # Fail if a previous session has not been cleaned up.
    keychain = root / "signing.keychain-db"
    p12 = root / "identity.p12"
    password = secrets.token_urlsafe(48)
    print("::add-mask::" + password, flush=True)
    previous_keychains = shlex.split(private_command("/usr/bin/security", "list-keychains", "-d", "user").decode())
    try:
        p12.write_bytes(base64.b64decode(os.environ["KALANDRA_SIGNING_P12_BASE64"], validate=True))
        p12.chmod(0o600)
        private_command("/usr/bin/security", "create-keychain", "-p", password, keychain)
        private_command("/usr/bin/security", "set-keychain-settings", "-lut", "7200", keychain)
        private_command("/usr/bin/security", "unlock-keychain", "-p", password, keychain)
        private_command("/usr/bin/security", "import", p12, "-k", keychain,
                        "-P", os.environ["KALANDRA_SIGNING_P12_PASSWORD"], "-T", "/usr/bin/codesign")
        p12.unlink()
        print("Encrypted signing identity imported successfully", flush=True)
        private_command("/usr/bin/security", "set-key-partition-list", "-S",
                        "apple-tool:,apple:,codesign:", "-s", "-k", password, keychain)
        private_command("/usr/bin/security", "list-keychains", "-d", "user", "-s",
                        keychain, *previous_keychains)
        # Public certificate metadata only; no key or credential values are logged.
        identities = private_command("/usr/bin/security", "find-identity", "-v", "-p",
                                     "codesigning", keychain).decode()
        available = re.findall(r'\b([A-F0-9]{40}) "Developer ID Application: [^"\n]* \(([A-Z0-9]{10})\)"', identities)
        print("Imported Developer ID certificates (SHA-1, Team ID): " +
              (", ".join(f"{fingerprint} {team}" for fingerprint, team in available) or "none"), flush=True)
        failures = []
        try:
            validate_signing(root, keychain)
        except RuntimeError as error:
            failures.append(str(error))
            print(str(error), flush=True)
        try:
            private_command("/usr/bin/xcrun", "notarytool", "store-credentials", "kalandra-ci",
                            "--keychain", keychain, "--apple-id", os.environ["KALANDRA_APPLE_ID"],
                            "--team-id", os.environ["KALANDRA_APPLE_TEAM_ID"],
                            "--password", os.environ["KALANDRA_APPLE_APP_PASSWORD"], "--validate")
            print("Apple notarization authentication validated", flush=True)
        except RuntimeError as error:
            failures.append(str(error))
            print(str(error), flush=True)
        if failures:
            raise RuntimeError("; ".join(failures))
        if check_only:
            print("Credential check passed: Developer ID signing, trusted timestamp, execution, "
                  "and Apple notarization authentication", flush=True)
            return
        for kind in ("editor-tools", "metal", "cpu"):
            name = f"kalandra-native-{kind}-standalone-macos-arm64.tar.gz"
            sign(Path("release-assets") / name, Path("signed-assets") / kind,
                 os.environ["KALANDRA_SIGNING_IDENTITY"], os.environ["KALANDRA_APPLE_TEAM_ID"],
                 "kalandra-ci", keychain)
    finally:
        subprocess.run(["/usr/bin/security", "list-keychains", "-d", "user", "-s", *previous_keychains],
                       capture_output=True, timeout=30)
        subprocess.run(["/usr/bin/security", "delete-keychain", str(keychain)],
                       capture_output=True, timeout=30)
        shutil.rmtree(root, ignore_errors=True)


def validate_signing(root, keychain):
    # Prove that the imported private key is usable without UI prompts,
    # the certificate matches this publisher, and trusted timestamps work.
    source, executable = root / "check.c", root / "check"
    source.write_text("int main(void) { return 0; }\n")
    private_command("/usr/bin/xcrun", "clang", source, "-o", executable)
    private_command("/usr/bin/codesign", "--force", "--sign",
                    os.environ["KALANDRA_SIGNING_IDENTITY"], "--keychain", keychain,
                    "--timestamp", "--options", "runtime", executable)
    private_command("/usr/bin/codesign", "--verify", "--strict", executable)
    details = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", str(executable)],
                             capture_output=True, text=True, timeout=30)
    if (details.returncode or "Authority=Developer ID Application:" not in details.stderr or
            f"TeamIdentifier={os.environ['KALANDRA_APPLE_TEAM_ID']}" not in details.stderr):
        raise RuntimeError("Signing identity does not match the configured Developer ID team")
    private_command(str(executable))
    print("Developer ID signature, trusted timestamp and execution verified", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    main(parser.parse_args().check_only)
