import base64
import hashlib
import importlib.util
import io
import json
import os
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "questbridge/native-driver/stage-sysroot.py"
_spec = importlib.util.spec_from_file_location("stage_sysroot", SCRIPT)
stage = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stage)

REPO = "https://example.invalid/platform/prebuilts/vndk/v34"
REV = "1" * 40
ARCHIVE = f"{REPO}/+archive/{REV}/arm64/include/x.tar.gz"
HEADER = b"#pragma once\n"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def targz(*members):
    """Gzip a PAX tar of (TarInfo, bytes or None) pairs, shaped like a Gitiles +archive."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.PAX_FORMAT) as tar:
        for info, data in members:
            if data is not None:
                info.size = len(data)
            tar.addfile(info, None if data is None else io.BytesIO(data))
    return buf.getvalue()


def regular(name, data):
    return tarfile.TarInfo(name), data


def typed(name, kind, linkname=""):
    info = tarfile.TarInfo(name)
    info.type, info.linkname = kind, linkname
    return info, None


def entry(member, data):
    return {"path": f"include/x/{member}", "url": ARCHIVE, "member": member,
            "sha256": sha(data), "size": len(data)}


def tiny_spec(data=HEADER):
    src = {"repo": REPO, "tag": "t", "tag_object": "2" * 40, "revision": REV, "tree": "3" * 40}
    spec = {"tag": "t", "revision": REV, "sources": {"vndk-v34": src},
            "include_roots": ["include/x"], "libcxx_include_roots": [],
            "module_exports": {"m": ["include/x"]}, "libs": {}, "dependency_closure": {},
            "toolchain_provided_needed": [], "required_headers": ["a.h"],
            "files": [entry("a.h", data)]}
    return spec, targz(regular("a.h", data))


def tree_snapshot(root):
    snap = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root)
            if os.path.islink(full):
                snap[rel] = ("link", os.readlink(full))
            elif os.path.isdir(full):
                snap[rel] = ("dir",)
            else:
                snap[rel] = ("file", Path(full).read_bytes())
    return snap


def stat_snapshot(root):
    """Metadata of every entry, without reading any file contents."""
    snap = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            full = os.path.join(dirpath, name)
            st = os.lstat(full)
            snap[os.path.relpath(full, root)] = (st.st_mode, st.st_size, st.st_mtime_ns, st.st_ino)
    return snap


class ArchiveTest(unittest.TestCase):
    def test_exact_pinned_tree_unpacks(self):
        raw = targz(typed("hidl", tarfile.DIRTYPE), regular("hidl/a.h", HEADER))
        self.assertEqual({"include/x/hidl/a.h": HEADER},
                         stage.unpack_archive(raw, [entry("hidl/a.h", HEADER)], ARCHIVE))

    def test_inflation_bombs_refused_before_tar_parsing(self):
        pax = regular("a.h", b"x")
        pax[0].pax_headers = {"comment": "c" * (4 << 20)}
        for raw in (targz(regular("a.h", bytes(8 << 20))), targz(pax)):
            self.assertLess(len(raw), 64 << 10)  # tiny on the wire, megabytes inflated
            with mock.patch.object(stage.tarfile, "open", side_effect=AssertionError("parsed")):
                with self.assertRaisesRegex(stage.StageError, "inflates past"):
                    stage.unpack_archive(raw, [entry("a.h", b"x")], ARCHIVE)

    def test_truncated_or_trailing_gzip_refused(self):
        raw = targz(regular("a.h", b"x"))
        for bad in (raw[:-8], raw + b"junk", raw + raw):
            with self.assertRaisesRegex(stage.StageError, "gzip"):
                stage.unpack_archive(bad, [entry("a.h", b"x")], ARCHIVE)

    def test_unpinned_members_refused(self):
        cases = {
            "symlink": targz(typed("a.h", tarfile.SYMTYPE, "/etc/passwd")),
            "hardlink": targz(typed("a.h", tarfile.LNKTYPE, "b.h")),
            "extra file": targz(regular("a.h", b"x"), regular("b.h", b"y")),
            "extra dir": targz(typed("zzz", tarfile.DIRTYPE), regular("a.h", b"x")),
            "duplicate": targz(regular("a.h", b"x"), regular("a.h", b"x")),
            "wrong size": targz(regular("a.h", b"xy")),
            "missing": targz(),
        }
        for name, raw in cases.items():
            with self.subTest(name), self.assertRaises(stage.StageError):
                stage.unpack_archive(raw, [entry("a.h", b"x")], ARCHIVE)

    def test_wrong_sha_refused(self):
        with self.assertRaisesRegex(stage.StageError, "checksum mismatch"):
            stage.unpack_archive(targz(regular("a.h", b"y")), [entry("a.h", b"x")], ARCHIVE)
        single = {"path": "lib/x.so", "url": "u", "sha256": sha(b"x"), "size": 1}
        self.assertEqual(b"x", stage.decode_base64(base64.b64encode(b"x"), single))
        with self.assertRaisesRegex(stage.StageError, "checksum mismatch"):
            stage.decode_base64(base64.b64encode(b"y"), single)

    def test_superseded_member_verified_then_dropped(self):
        stale = {"url": ARCHIVE, "member": "s.h", "sha256": sha(b"stale"), "size": 5}
        raw = targz(regular("k.h", b"kept"), regular("s.h", b"stale"))
        self.assertEqual({"include/x/k.h": b"kept"},
                         stage.unpack_archive(raw, [entry("k.h", b"kept")], ARCHIVE, [stale]))
        raw = targz(regular("k.h", b"kept"), regular("s.h", b"STALE"))
        with self.assertRaisesRegex(stage.StageError, "checksum mismatch"):
            stage.unpack_archive(raw, [entry("k.h", b"kept")], ARCHIVE, [stale])

    def test_fetch_caps_response_size(self):
        with mock.patch.object(stage.urllib.request, "urlopen", return_value=io.BytesIO(b"x" * 100)):
            with self.assertRaisesRegex(stage.StageError, "exceeds 10 bytes"):
                stage.fetch("https://example.invalid/a", 10)

    def test_elf_wrong_class_or_arch_refused(self):
        def header(ei_class, machine):
            return b"\x7fELF" + bytes([ei_class, 1, 1]) + bytes(9) + struct.pack("<HH", 3, machine) + bytes(44)
        with self.assertRaisesRegex(stage.StageError, "64-bit"):
            stage.elf_dynamic(header(1, 40), "lib32.so")
        with self.assertRaisesRegex(stage.StageError, "AArch64"):
            stage.elf_dynamic(header(2, 62), "libx86_64.so")


class SpecTest(unittest.TestCase):
    def test_repository_manifest_is_consistent(self):
        stage.check_spec(json.loads(stage.SPEC.read_text()))

    def test_snapshot_file_outside_arm64_refused(self):
        spec, _ = tiny_spec()
        spec["files"][0]["url"] = f"{REPO}/+archive/{REV}/arm/include/x.tar.gz"
        with self.assertRaisesRegex(stage.StageError, "outside arm64"):
            stage.check_spec(spec)


class StageRootTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.parent = Path(tmp.name)
        self.out = self.parent / "sysroot"
        self.fetched = []
        patcher = mock.patch.object(stage, "verify_source")  # no network in tests
        self.verify_source = patcher.start()
        self.addCleanup(patcher.stop)

    def run_stage(self, spec, raw, out=None):
        def fake_fetch(url, limit):
            self.fetched.append(url)
            self.assertLessEqual(len(raw), limit)
            return raw
        with mock.patch.object(stage, "fetch", side_effect=fake_fetch):
            return stage.stage(spec, json.dumps(spec).encode(), out or self.out)

    def test_absent_out_published_and_temp_removed(self):
        spec, raw = tiny_spec()
        self.assertEqual("staged", self.run_stage(spec, raw))
        self.assertEqual(HEADER, (self.out / "include/x/a.h").read_bytes())
        stamp = json.loads((self.out / stage.STAMP).read_text())
        self.assertEqual(sha(json.dumps(spec).encode()), stamp["spec_sha256"])
        self.assertEqual({"sysroot", ".sysroot.lock"}, set(os.listdir(self.parent)))

    def test_verified_root_reused_without_network_or_rename(self):
        spec, raw = tiny_spec()
        self.run_stage(spec, raw)
        inode, before = os.stat(self.out).st_ino, tree_snapshot(self.out)
        self.fetched.clear()
        self.verify_source.reset_mock()
        self.assertEqual("reused", self.run_stage(spec, raw))
        self.assertEqual([], self.fetched)
        self.verify_source.assert_not_called()
        self.assertEqual((inode, before), (os.stat(self.out).st_ino, tree_snapshot(self.out)))

    def test_changed_spec_refused_and_root_untouched(self):
        spec, raw = tiny_spec()
        self.run_stage(spec, raw)
        before = tree_snapshot(self.out)
        changed, raw2 = tiny_spec(b"#pragma once // v2\n")
        with self.assertRaisesRegex(stage.StageError, "new --out"):
            self.run_stage(changed, raw2)
        self.assertEqual(before, tree_snapshot(self.out))
        self.assertEqual(1, len(self.fetched))

    def test_corrupt_root_refused_and_left_as_is(self):
        spec, raw = tiny_spec()
        header = Path("include/x/a.h")

        def relink(root):
            (root / header).unlink()
            (root / header).symlink_to("/dev/null")
        corruptions = {
            "edited": lambda root: (root / header).write_bytes(b"evil\n"),
            "extra": lambda root: (root / "include/x/b.h").write_bytes(b""),
            "symlink": relink,
            "missing": lambda root: (root / header).unlink(),
            "stamp": lambda root: (root / stage.STAMP).write_text("{}"),
        }
        for name, corrupt in corruptions.items():
            with self.subTest(name):
                out = self.parent / name
                self.run_stage(spec, raw, out)
                corrupt(out)
                before = tree_snapshot(out)
                with self.assertRaisesRegex(stage.StageError, "left untouched"):
                    self.run_stage(spec, raw, out)
                self.assertEqual(before, tree_snapshot(out))

    def test_unowned_existing_paths_preserved(self):
        spec, raw = tiny_spec()
        self.out.mkdir()
        (self.out / "mine.txt").write_text("keep")
        plain = self.parent / "plainfile"
        plain.write_text("keep")
        for out in (self.out, plain):
            with self.assertRaisesRegex(stage.StageError, "left untouched"):
                self.run_stage(spec, raw, out)
        self.assertEqual({"mine.txt": ("file", b"keep")}, tree_snapshot(self.out))
        self.assertEqual("keep", plain.read_text())
        self.assertEqual([], self.fetched)

    def test_leftovers_from_other_runs_preserved(self):
        spec, raw = tiny_spec()
        leftovers = (".sysroot.partial-123", ".sysroot.old-123", ".sysroot.stage-abc")
        for name in leftovers:
            (self.parent / name).mkdir()
            (self.parent / name / "keep").write_text(name)
        self.run_stage(spec, raw)
        for name in leftovers:
            self.assertEqual(name, (self.parent / name / "keep").read_text())

    def test_failed_publish_removes_only_its_temp_dir(self):
        spec, raw = tiny_spec()
        (self.parent / ".sysroot.partial-1").mkdir()
        with mock.patch.object(stage.os, "rename", side_effect=OSError("disk said no")):
            with self.assertRaisesRegex(OSError, "disk said no"):
                self.run_stage(spec, raw)
        self.assertFalse(os.path.lexists(self.out))
        self.assertEqual({".sysroot.partial-1", ".sysroot.lock"}, set(os.listdir(self.parent)))

    def test_out_created_during_staging_left_untouched(self):
        spec, raw = tiny_spec()

        def racing_fetch(url, limit):
            self.out.mkdir()
            (self.out / "theirs").write_text("x")
            return raw
        with mock.patch.object(stage, "fetch", side_effect=racing_fetch):
            with self.assertRaisesRegex(stage.StageError, "appeared while staging"):
                stage.stage(spec, json.dumps(spec).encode(), self.out)
        self.assertEqual({"theirs": ("file", b"x")}, tree_snapshot(self.out))
        self.assertEqual({"sysroot", ".sysroot.lock"}, set(os.listdir(self.parent)))

    def test_huge_sparse_pinned_file_refused_without_reading_it(self):
        spec, raw = tiny_spec()
        self.run_stage(spec, raw)
        huge = self.out / "include/x/a.h"
        with open(huge, "r+b") as fh:
            fh.truncate(4 << 30)  # sparse 4 GiB: reading it would exhaust memory
        before, reads, real = stat_snapshot(self.out), [], stage.read_regular

        def recording(path, limit):
            reads.append(Path(path))
            return real(path, limit)
        with mock.patch.object(stage, "read_regular", side_effect=recording):
            with self.assertRaisesRegex(stage.StageError, f"holds {4 << 30} bytes"):
                self.run_stage(spec, raw)
        self.assertEqual([self.out / stage.STAMP], reads)
        self.assertEqual(before, stat_snapshot(self.out))

    def test_oversized_stamp_refused_before_decoding(self):
        spec, raw = tiny_spec()
        self.run_stage(spec, raw)
        stamp = self.out / stage.STAMP
        stamp.write_text(stamp.read_text() + " " * stage.STAMP_MAX)  # same JSON value, too big
        before = stat_snapshot(self.out)
        with mock.patch.object(stage.json, "loads", side_effect=AssertionError("decoded")):
            with self.assertRaisesRegex(stage.StageError, f"at most {stage.STAMP_MAX} bytes"):
                self.run_stage(spec, raw)
        self.assertEqual(before, stat_snapshot(self.out))


if __name__ == "__main__":
    unittest.main()
