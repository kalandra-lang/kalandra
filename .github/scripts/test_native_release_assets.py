import json
from pathlib import Path
import tempfile
import unittest

from native_release_assets import ARCHIVES, IMAGES, digest, validate_assets


def write_fixture(root):
    unsigned, signed = root / "release-assets", root / "signed-assets"
    unsigned.mkdir()
    signed.mkdir()
    for name in (*ARCHIVES, "kalandra-native.vsix"):
        path = unsigned / name
        path.write_bytes(b"unsigned fixture")
        path.with_suffix(path.suffix + ".sha256").write_text(f"{digest(path)}  {name}\n")
    for name in IMAGES:
        path = signed / name
        path.write_bytes(b"signed fixture")
        path.with_suffix(".dmg.sha256").write_text(f"{digest(path)}  {name}\n")
        path.with_suffix(".notarization.json").write_text(json.dumps({
            "notarization": {"status": "Accepted"}, "stapleValidated": True,
            "gatekeeperAssessmentPassed": True, "signedRuntimeQualificationPassed": True,
            "artifactSHA256": digest(path),
            "inputArchiveSHA256": digest(unsigned / (path.stem + "-macos-arm64.tar.gz"))}))
    return unsigned, signed


class AssetTests(unittest.TestCase):
    def test_complete_release(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(len(validate_assets(*write_fixture(Path(folder)))), 29)

    def test_missing_receipt_blocks_release(self):
        with tempfile.TemporaryDirectory() as folder:
            unsigned, signed = write_fixture(Path(folder))
            next(signed.glob("*.json")).unlink()
            with self.assertRaisesRegex(ValueError, "All three notarized"):
                validate_assets(unsigned, signed)

    def test_changed_unsigned_or_signed_bytes_block_release(self):
        for signed_asset in (False, True):
            with self.subTest(signed=signed_asset), tempfile.TemporaryDirectory() as folder:
                unsigned, signed = write_fixture(Path(folder))
                target = next(signed.glob("*.dmg")) if signed_asset else unsigned / ARCHIVES[0]
                target.write_bytes(b"tampered")
                with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                    validate_assets(unsigned, signed)

    def test_rejected_notarization_blocks_release(self):
        with tempfile.TemporaryDirectory() as folder:
            unsigned, signed = write_fixture(Path(folder))
            receipt = next(signed.glob("*.json"))
            record = json.loads(receipt.read_text())
            record["notarization"]["status"] = "Invalid"
            receipt.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "receipt"):
                validate_assets(unsigned, signed)

    def test_unexpected_asset_blocks_release(self):
        with tempfile.TemporaryDirectory() as folder:
            unsigned, signed = write_fixture(Path(folder))
            (unsigned / "unexpected").write_text("not qualified")
            with self.assertRaisesRegex(ValueError, "unexpected"):
                validate_assets(unsigned, signed)
