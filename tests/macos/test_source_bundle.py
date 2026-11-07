# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Check source attribution coverage and safe extraction of license notices."""
import io
import json
from pathlib import Path
import re
import sqlite3
import tarfile
import tempfile
import unittest
import zipfile

from test_build_tools import ROOT, load


class SourceBundleTest(unittest.TestCase):
    def setUp(self):
        self.sources = load("source_bundle")

    def test_catalog_has_verified_sources_and_all_ghostty_inputs(self):
        catalog = json.loads((ROOT / "craft/release-sources.json").read_text())
        components = {entry["package"]: entry for entry in catalog["components"]}
        self.assertEqual(len(components), len(catalog["components"]))
        for entry in components.values():
            self.assertTrue(entry["version"])
            self.assertTrue(entry["sources"])
            for source in entry["sources"]:
                self.assertTrue(source["url"].startswith("https://"))
                self.assertRegex(source["sha256"], re.compile(r"^[0-9a-f]{64}$"))
        ghostty = json.loads((ROOT / "craft/ghostty-sources.json").read_text())
        for source in ghostty["sources"]:
            self.assertIn({key: source[key] for key in ("url", "sha256")}, components["kwi/libghostty-vt"]["sources"])

    def test_unmapped_native_code_blocks_packaging(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "install.db"
            with sqlite3.connect(database) as db:
                db.executescript("CREATE TABLE packageList(packageId,packagePath,version); CREATE TABLE fileList(packageId,filename);")
                db.execute("INSERT INTO packageList VALUES(1,'libs/example','1.2')")
                db.execute("INSERT INTO fileList VALUES(1,'lib/libExample.dylib')")
            self.assertEqual(self.sources.runtime_components(database, [Path("Zirq.app/kwi"), Path("Zirq.app/libExample.dylib")]), {"libs/example": "1.2"})
            with self.assertRaisesRegex(RuntimeError, "No source package owns"):
                self.sources.runtime_components(database, [Path("libUnknown.dylib")])

    def test_tar_notices_preserve_paths_without_following_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "source.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                for name, body in [("project/LICENSE", b"license"), ("project/vendor/LICENSES/MIT.txt", b"vendor license"),
                                   ("project/README", b"Copyright contributor"), ("project/code.cpp", b"code")]:
                    item = tarfile.TarInfo(name)
                    item.size = len(body)
                    output.addfile(item, io.BytesIO(body))
                link = tarfile.TarInfo("project/COPYING")
                link.type = tarfile.SYMTYPE
                link.linkname = "/etc/passwd"
                output.addfile(link)
            destination = root / "notices"
            self.assertEqual(self.sources.write_notices(archive, destination), 3)
            self.assertEqual((destination / "project/vendor/LICENSES/MIT.txt").read_bytes(), b"vendor license")
            self.assertFalse((destination / "project/COPYING").exists())
            self.assertFalse((destination / "project/code.cpp").exists())

    def test_only_translations_of_shipped_components_are_kept(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "install.db"
            app = root / "Zirq.app"
            locale = app / "Contents/Resources/locale/en/LC_MESSAGES"
            locale.mkdir(parents=True)
            with sqlite3.connect(database) as db:
                db.executescript("CREATE TABLE packageList(packageId,packagePath); CREATE TABLE fileList(packageId,filename);")
                for index, package in enumerate(["kwi/kwi", "libs/example", "dev-utils/compiler"]):
                    db.execute("INSERT INTO packageList VALUES(?,?)", (index, package))
                    filename = str(index) + ".mo"
                    db.execute("INSERT INTO fileList VALUES(?,?)", (index, "share/locale/en/LC_MESSAGES/" + filename))
                    (locale / filename).write_bytes(b"translation")
            self.sources.prune_translations(database, app, {"libs/example"})
            self.assertEqual({path.name for path in locale.iterdir()}, {"0.mo", "1.mo"})
            (locale / "unknown.mo").write_bytes(b"unattributed")
            with self.assertRaisesRegex(RuntimeError, "No source package owns translation"):
                self.sources.prune_translations(database, app, {"libs/example"})

    def test_zip_parent_traversal_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "source.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("../LICENSE", b"escape")
            with self.assertRaisesRegex(RuntimeError, "Unsafe archive path"):
                self.sources.write_notices(archive, root / "notices")
            self.assertFalse((root / "LICENSE").exists())


if __name__ == "__main__":
    unittest.main()
