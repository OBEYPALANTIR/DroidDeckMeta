#!/usr/bin/env python3
"""Stage the pinned Android 14 VNDK v34 arm64 subset described by android-sysroot.json.

Every listed file is fetched from Gitiles at its pinned revision (one +archive tarball per
include root, ?format=TEXT for single files) and checked for exact size and SHA-256.
Libraries must be AArch64 ET_DYN ELFs whose SONAME and DT_NEEDED match the pinned closure;
required headers must resolve under the declared include roots.

A staged root is immutable. An existing --out is reused only when its stamp names this exact
spec and every file, ELF and header check passes again; anything else is refused and left
untouched, so changing any pin needs a new --out. An absent --out is built in a private temp
dir and published with a single rename under a lock. Any failure exits 1.

shortcut: "files" was pinned from Merkle-verified Gitiles listings by hand-run research;
add a resolve mode here before bumping the tag or the module selection.
"""
import argparse
import base64
import contextlib
import fcntl
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import stat
import struct
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path, PurePosixPath

SPEC = Path(__file__).with_name("android-sysroot.json")
STAMP = ".stage-sysroot-stamp.json"
STAMP_MAX = 4096  # stamps are ~200 bytes; a bigger one is refused before decoding
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
EM_AARCH64, ET_DYN, SHT_DYNAMIC, DT_NULL, DT_NEEDED, DT_SONAME = 183, 3, 6, 0, 1, 14
THROTTLED = (429, 503)  # googlesource answered both to bursts while this subset was staged
# Inflated tar bound: per member a 512-byte header plus at most one pax/GNU long-name block
# (<= 1.5 KiB for names under 1000 bytes; pinned names are under 100), per archive a global
# pax header, the end-of-archive blocks and record padding.
TAR_MEMBER_OVERHEAD, TAR_ARCHIVE_OVERHEAD = 2048, 20480


class StageError(Exception):
    pass


def fetch(url, limit):
    req = urllib.request.Request(url, headers={"User-Agent": "droiddeck-stage-sysroot/1"})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read(limit + 1)
            break
        except urllib.error.HTTPError as e:
            if e.code not in THROTTLED or attempt == 3:
                raise StageError(f"fetch failed: {url}: {e!r}") from e
            delay = e.headers.get("Retry-After", "")
            time.sleep(min(int(delay), 60) if delay.isdigit() else 5 * 2 ** attempt)
        except (urllib.error.URLError, http.client.HTTPException, OSError) as e:
            raise StageError(f"fetch failed: {url}: {e!r}") from e
    if len(data) > limit:
        raise StageError(f"response exceeds {limit} bytes: {url}")
    return data


def gitiles_json(url):
    text = fetch(url, 1 << 20).decode("utf-8", "replace")
    if not text.startswith(")]}'"):
        raise StageError(f"not a Gitiles JSON response: {url}")
    try:
        return json.loads(text[4:])
    except ValueError as e:
        raise StageError(f"bad Gitiles JSON from {url}: {e}") from e


def verify_source(name, src):
    """The tag must still peel to the pinned commit, which must carry the pinned tree."""
    repo, tag, rev = src["repo"], src["tag"], src["revision"]
    ref = gitiles_json(f"{repo}/+refs/tags/{tag}?format=JSON").get(f"refs/tags/{tag}") or {}
    if ref.get("value") != src["tag_object"] or ref.get("peeled", ref.get("value")) != rev:
        raise StageError(f"{name}: tag {tag} is {ref}, pinned {src['tag_object']} -> {rev}")
    commit = gitiles_json(f"{repo}/+/{rev}?format=JSON")
    if commit.get("commit") != rev or commit.get("tree") != src["tree"]:
        raise StageError(f"{name}: commit {rev} has tree {commit.get('tree')}, pinned {src['tree']}")


def safe_rel(path):
    pure = PurePosixPath(path)
    return bool(path) and not pure.is_absolute() and ".." not in pure.parts and str(pure) == path


def pin_ok(entry):
    return HEX64.fullmatch(entry["sha256"]) and isinstance(entry["size"], int) and entry["size"] >= 0


def check_spec(spec):
    sources = spec["sources"]
    vndk = sources["vndk-v34"]
    if (spec["revision"], spec["tag"]) != (vndk["revision"], vndk["tag"]):
        raise StageError("top-level revision/tag disagree with sources.vndk-v34")
    kinds = []  # (url prefix, url suffix, is snapshot source)
    for name, src in sources.items():
        if not all(HEX40.fullmatch(src[k]) for k in ("revision", "tree", "tag_object")):
            raise StageError(f"{name}: revision/tree/tag_object must be 40-hex ids")
        kinds.append((f"{src['repo']}/+/{src['revision']}/", "?format=TEXT", src is vndk))
        kinds.append((f"{src['repo']}/+archive/{src['revision']}/", ".tar.gz", src is vndk))
    files = {}
    for f in spec["files"]:
        path, url, member = f["path"], f["url"], f.get("member")
        if not safe_rel(path) or path in files:
            raise StageError(f"unsafe or duplicate path: {path!r}")
        files[path] = f
        if not pin_ok(f):
            raise StageError(f"{path}: bad sha256/size pin")
        kind = [k for k in kinds if url.startswith(k[0]) and url.endswith(k[1])]
        if not kind or (kind[0][1] == ".tar.gz") != (member is not None):
            raise StageError(f"{path}: url is not a pinned-revision Gitiles file/archive: {url}")
        if kind[0][2] and not url[len(kind[0][0]):].startswith("arm64/"):
            raise StageError(f"{path}: snapshot file outside arm64/: {url}")
        if member is not None and not (safe_rel(member) and path.endswith("/" + member)):
            raise StageError(f"{path}: archive member {member!r} does not match its path")
    archives = {f["url"] for f in spec["files"] if "member" in f}
    for s in spec.get("superseded_members", []):
        target = files.get(s["replaced_by"])
        if (s["url"] not in archives or not safe_rel(s["member"]) or not pin_ok(s)
                or target is None or target["url"] == s["url"]
                or not s["replaced_by"].endswith("/" + s["member"])):
            raise StageError(f"bad superseded member pin: {s}")
    roots = set(spec["include_roots"]) | set(spec["libcxx_include_roots"])
    stray = {r for dirs in spec["module_exports"].values() for r in dirs} - roots
    if stray:
        raise StageError(f"module_exports roots not declared as include roots: {sorted(stray)}")
    if set(spec["libs"]) != set(spec["dependency_closure"]):
        raise StageError("libs and dependency_closure name different libraries")


def check_bytes(entry, data):
    if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise StageError(f"checksum mismatch: {entry.get('path', entry.get('member'))} "
                         f"from {entry['url']}")
    return data


def decode_base64(raw, entry):
    try:
        data = base64.b64decode(raw.strip(), validate=True)
    except ValueError as e:
        raise StageError(f"{entry['path']}: response is not base64: {entry['url']}") from e
    return check_bytes(entry, data)


def member_dirs(entries):
    return {str(p) for e in entries for p in PurePosixPath(e["member"]).parents if str(p) != "."}


def tar_bound(entries, dirs):
    data = sum((e["size"] + 511) // 512 * 512 for e in entries)
    return data + TAR_MEMBER_OVERHEAD * (len(entries) + len(dirs)) + TAR_ARCHIVE_OVERHEAD


def gunzip_bounded(raw, limit, url):
    """Inflate one gzip stream, refusing more than limit bytes before anything parses it."""
    inflater = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        data = inflater.decompress(raw, limit + 1)
    except zlib.error as e:
        raise StageError(f"bad gzip stream {url}: {e}") from e
    if len(data) > limit:
        raise StageError(f"archive inflates past {limit} bytes: {url}")
    if not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
        raise StageError(f"truncated or trailing gzip data: {url}")
    return data


def unpack_archive(raw, entries, url, superseded=()):
    """Return {path: bytes} from a Gitiles tarball holding exactly the pinned members.

    Superseded members (replaced by another source) must still match their pins, then are
    discarded. Directories must be parents of pinned members; anything else is refused.
    """
    want = {e["member"]: e for e in entries}
    drop = {s["member"]: s for s in superseded}
    if want.keys() & drop.keys():
        raise StageError(f"archive member both kept and superseded: {url}")
    pinned = {**want, **drop}
    dirs = member_dirs(pinned.values())
    data = gunzip_bounded(raw, tar_bound(list(pinned.values()), dirs), url)
    got, seen_dirs, count = {}, set(), 0
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r|") as tar:
            for m in tar:
                count += 1
                if count > len(pinned) + len(dirs):
                    raise StageError(f"archive has more members than pinned: {url}")
                if m.type == tarfile.DIRTYPE:
                    name = m.name.rstrip("/")
                    if name not in dirs or name in seen_dirs:
                        raise StageError(f"unpinned archive directory {m.name!r}: {url}")
                    seen_dirs.add(name)
                    continue
                e = pinned.get(m.name)
                if (m.type not in (tarfile.REGTYPE, tarfile.AREGTYPE) or e is None
                        or m.name in got or m.size != e["size"]):
                    raise StageError(f"unpinned archive member {m.name!r} "
                                     f"(type {m.type!r}, {m.size} bytes): {url}")
                got[m.name] = check_bytes(e, tar.extractfile(m).read())
    except (tarfile.TarError, EOFError, ValueError, OSError) as ex:
        raise StageError(f"unreadable archive {url}: {ex!r}") from ex
    missing = sorted(pinned.keys() - got.keys())
    if missing:
        raise StageError(f"archive lacks pinned members {missing[:5]}: {url}")
    return {e["path"]: got[name] for name, e in want.items()}


def fetch_url(url, entries, superseded=()):
    """Fetch one pinned URL; return {path: bytes} for every spec entry it carries."""
    if "member" not in entries[0]:
        if len(entries) != 1:
            raise StageError(f"single-file url listed {len(entries)} times: {url}")
        raw = fetch(url, 4 * ((entries[0]["size"] + 2) // 3) + 2)
        return {entries[0]["path"]: decode_base64(raw, entries[0])}
    pinned = list(entries) + list(superseded)
    limit = tar_bound(pinned, member_dirs(pinned))
    raw = fetch(url, limit + (limit >> 10) + 1024)  # deflate never grows data by more
    return unpack_archive(raw, entries, url, superseded)


def elf_dynamic(data, name):
    """Return (SONAME, DT_NEEDED list) of an AArch64 shared object, else raise."""
    if data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise StageError(f"{name}: not a 64-bit little-endian ELF")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if (e_machine, e_type) != (EM_AARCH64, ET_DYN):
        raise StageError(f"{name}: e_machine={e_machine} e_type={e_type}, need AArch64 ET_DYN")
    try:
        (shoff,) = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        if shentsize != 64:
            raise StageError(f"{name}: unexpected section header size {shentsize}")
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * 64) for i in range(shnum)]
        for _, sh_type, _, _, off, size, link, _, _, _ in sections:
            if sh_type != SHT_DYNAMIC:
                continue
            strtab = sections[link][4]
            cstr = lambda o: data[strtab + o:data.index(b"\0", strtab + o)].decode()
            soname, needed = None, []
            for i in range(size // 16):
                tag, val = struct.unpack_from("<qQ", data, off + 16 * i)
                if tag == DT_NULL:
                    break
                if tag == DT_NEEDED:
                    needed.append(cstr(val))
                elif tag == DT_SONAME:
                    soname = cstr(val)
            return soname, needed
    except (struct.error, ValueError, IndexError, UnicodeDecodeError) as e:
        raise StageError(f"{name}: malformed ELF dynamic section: {e}") from e
    raise StageError(f"{name}: no SHT_DYNAMIC section")


def check_staged(spec, staged):
    roots = spec["include_roots"] + spec["libcxx_include_roots"]
    for root in roots:
        if not any(p.startswith(root + "/") for p in staged):
            raise StageError(f"include root has no staged files: {root}")
    missing = [h for h in spec["required_headers"] if not any(f"{r}/{h}" in staged for r in roots)]
    if missing:
        raise StageError(f"required headers missing from include roots: {missing}")
    provided = set(spec["libs"]) | set(spec["toolchain_provided_needed"])
    for soname, path in sorted(spec["libs"].items()):
        if path not in staged:
            raise StageError(f"{soname}: {path} is not a staged file")
        got, needed = elf_dynamic(staged[path], path)
        if got != soname:
            raise StageError(f"{path}: SONAME {got!r}, expected {soname!r}")
        if needed != spec["dependency_closure"][soname]:
            raise StageError(f"{path}: DT_NEEDED {needed} differs from pinned closure")
        unresolved = [n for n in needed if n not in provided]
        if unresolved:
            raise StageError(f"{path}: DT_NEEDED not staged or toolchain-provided: {unresolved}")


def read_regular(path, limit):
    """Bytes of the regular file at path, or None if it is not one or holds over limit bytes."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)  # no symlink, no FIFO stall
    with os.fdopen(fd, "rb") as fh:
        st = os.fstat(fh.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_size > limit:
            return None
        data = fh.read(limit + 1)  # bounded even if a writer grows the file meanwhile
    return None if len(data) > limit else data


def read_existing(spec, stamp, out):
    """Re-verify an existing root without modifying it; refuse anything but this exact pin."""
    def refuse(why):
        return StageError(f"refusing to reuse {out}: {why}. It was left untouched; staged roots "
                          "are immutable, so stage this spec into a new --out")
    expected = {f["path"]: f for f in spec["files"]}
    staged = {}
    try:
        if not stat.S_ISDIR(os.lstat(out).st_mode):
            raise refuse("not a directory (or a symlink)")
        raw = read_regular(out / STAMP, STAMP_MAX)
        if raw is None:
            raise refuse(f"{STAMP} is not a regular file of at most {STAMP_MAX} bytes")
        found = json.loads(raw)
        if found != stamp:
            raise refuse(f"its stamp {found} is not this spec's {stamp}")
        for dirpath, dirnames, filenames in os.walk(out):
            for name in dirnames + filenames:
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, out)
                st = os.lstat(full)
                if stat.S_ISDIR(st.st_mode) or rel == STAMP:
                    continue
                entry = expected.get(rel)
                if not stat.S_ISREG(st.st_mode) or entry is None:
                    raise refuse(f"unexpected entry {rel}")
                if st.st_size != entry["size"]:  # judged from metadata, before reading anything
                    raise refuse(f"{rel} holds {st.st_size} bytes, pinned {entry['size']}")
                data = read_regular(full, entry["size"])
                if data is None:
                    raise refuse(f"{rel} changed while it was being verified")
                staged[rel] = data
    except (OSError, ValueError) as e:
        raise refuse(f"unreadable ({e})") from e
    missing = sorted(expected.keys() - staged.keys())
    if missing:
        raise refuse(f"pinned files missing: {missing[:5]}")
    try:
        for path, entry in expected.items():
            check_bytes(entry, staged[path])
        check_staged(spec, staged)
    except StageError as e:
        raise refuse(str(e)) from e


@contextlib.contextmanager
def publish_lock(out):
    """Serialize publishers of one out; the lock file is never removed."""
    fd = os.open(out.with_name(f".{out.name}.lock"), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


def publish(staged, stamp, out):
    """Build the tree in a private temp dir, then rename it onto the still-absent out."""
    out.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f".{out.name}.stage-", dir=out.parent))  # ours alone
    try:
        tree = work / "root"
        tree.mkdir()
        for path, data in staged.items():
            dest = tree / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        (tree / STAMP).write_text(json.dumps(stamp, indent=2) + "\n")
        with publish_lock(out):
            if os.path.lexists(out):
                raise StageError(f"{out} appeared while staging; left untouched "
                                 "(rerun to verify and reuse it, or choose a new --out)")
            # rename(2) can at most displace an empty directory, so nothing is lost here
            os.rename(tree, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def stage(spec, spec_bytes, out):
    """Reuse or build out and return "reused" or "staged"; never modifies an existing out."""
    check_spec(spec)
    stamp = {"spec_sha256": hashlib.sha256(spec_bytes).hexdigest(), "tag": spec["tag"],
             "revision": spec["revision"], "files": len(spec["files"])}
    if os.path.lexists(out):
        read_existing(spec, stamp, out)
        return "reused"
    for name, src in spec["sources"].items():
        verify_source(name, src)
    groups = {}
    for f in spec["files"]:
        groups.setdefault(f["url"], []).append(f)
    superseded = spec.get("superseded_members", [])
    staged = {}
    for url, entries in groups.items():  # sequential: parallel bursts drew 429/503
        staged.update(fetch_url(url, entries, [s for s in superseded if s["url"] == url]))
    check_staged(spec, staged)
    publish(staged, stamp, out)
    return "staged"


def main():
    spec_bytes = SPEC.read_bytes()
    spec = json.loads(spec_bytes)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=Path(os.path.expanduser(spec["default_root"])),
                        help="staged root, never replaced once it exists (default: %(default)s)")
    out = Path(os.path.abspath(parser.parse_args().out))
    try:
        result = stage(spec, spec_bytes, out)
    except (StageError, KeyError, TypeError, OSError) as e:
        print(f"stage-sysroot: error: {e if isinstance(e, StageError) else repr(e)}", file=sys.stderr)
        return 1
    total = sum(f["size"] for f in spec["files"])
    print(f"{result} {len(spec['files'])} files ({total} bytes) from {spec['tag']} @ {spec['revision']}")
    print(f"root: {out}")
    print(f"include roots: {len(spec['include_roots'])} (+{len(spec['libcxx_include_roots'])} libc++)")
    print(f"libs: {len(spec['libs'])}, DT_NEEDED closure complete "
          f"(toolchain-provided: {', '.join(spec['toolchain_provided_needed'])})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
