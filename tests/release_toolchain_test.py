"""Gate 4's build tools check (tools/release/release.py, toolchain_mismatches): the package setup/toolchain.json
names has to be built from the recompiler patch and commit the release recompiles with, or the phone's build makes
other game code than the app takes. Packages are made here; no GitHub is reached."""
import importlib.util
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/release"))
spec = importlib.util.spec_from_file_location("release", ROOT / "tools/release/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

PATCH = (ROOT / "patches/xenonrecomp-mw2.patch").read_bytes()
SETUP = (ROOT / "setup/setup.ps1").read_bytes()
NAMED = {"version": "3"}


def package(folder, patch=PATCH, commit=None, version="3"):
    """A toolchain APK with the manifest tools/toolchain/package.py writes."""
    commit = commit or release.re.search(rb"XenonCommit\s*=\s*'([0-9a-f]+)'", SETUP).group(1).decode()
    manifest = {"schema": 1, "version": version, "recompiler": {
        "commit": commit, "patchSha256": release.hashlib.sha256(patch.replace(b"\r", b"")).hexdigest()}}
    path = Path(folder) / f"toolchain-{len(list(Path(folder).iterdir()))}.apk"
    with zipfile.ZipFile(path, "w") as apk:
        apk.writestr("assets/toolchain/toolchain.json", json.dumps(manifest))
    return path


class ToolchainGateTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = folder.name

    def mismatches(self, apk, patch=PATCH, named=NAMED):
        return release.toolchain_mismatches(release.toolchain_manifest(apk), named, patch, SETUP)

    def test_built_from_this_patch_passes(self):
        self.assertEqual(self.mismatches(package(self.folder)), [])

    def test_carriage_returns_do_not_count(self):
        self.assertEqual(self.mismatches(package(self.folder), patch=PATCH.replace(b"\n", b"\r\n")), [])

    def test_older_patch_is_refused(self):
        # The state before the 2_10_10_10 fix's toolchain: toolchain-v2 was built from the patch before it.
        problems = self.mismatches(package(self.folder, patch=PATCH + b"\n# an older patch\n"))
        self.assertEqual(len(problems), 1)
        self.assertIn("xenonrecomp-mw2.patch", problems[0])

    def test_other_recompiler_commit_is_refused(self):
        problems = self.mismatches(package(self.folder, commit="0" * 40))
        self.assertEqual(len(problems), 1)
        self.assertIn("XenonRecomp", problems[0])

    def test_version_other_than_named_is_refused(self):
        problems = self.mismatches(package(self.folder, version="2"))
        self.assertEqual(len(problems), 1)
        self.assertIn("version", problems[0])


if __name__ == "__main__":
    unittest.main()
