"""Run package operations against a temporary compile-time root, never the host."""
import io
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="bbpkg-test-") as directory:
    base = Path(directory)
    root = base / "root"
    root.mkdir()
    binary = base / "bbpkg"
    subprocess.run(shlex.split(os.environ.get("HOSTCC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        f'-DBBPKG_ROOT="{root}"', str(SOURCE / "bbpkg.c"), "-o", str(binary)
    ], check=True)

    def run(*args, success=True, env=None):
        result = subprocess.run([str(binary), *args], text=True, capture_output=True, env=env)
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        return result.stdout

    def archive(path, entries):
        with tarfile.open(path, "w:gz") as output:
            for name, data in entries:
                item = tarfile.TarInfo(name)
                item.size = len(data)
                if name.endswith("/"):
                    item.type = tarfile.DIRTYPE
                    item.mode = 0o755
                output.addfile(item, io.BytesIO(data))

    assert "no packages" in run("list")
    run("list", "extra", success=False)
    run("install", "../escape", "missing", success=False)
    package = base / "pkg'; touch INJECTED; '.tar.gz"
    # Includes more than the old hard limit and deliberately unordered directories.
    archive(package, [(f"opt/test/f{i}", b"hello") for i in range(4100)] + [("opt/", b""), ("opt/test/", b"")])
    run("install", "sample", str(package))
    assert not (SOURCE / "INJECTED").exists()
    assert (root / "opt/test/f4099").read_text() == "hello"
    assert "sample" in run("list")
    assert "f4099" in run("info", "sample")
    run("install", "sample", str(package), success=False)
    run("remove", "sample")
    assert not (root / "opt/test").exists()
    assert "no packages" in run("list")
    run("install", "missing", str(base / "absent"), success=False)
    assert not list((root / "var/lib/bbpkg/installed").glob(".pending-*"))
    archive(package, [("../escape", b"bad")])
    run("install", "traversal", str(package), success=False)
    assert not (base / "escape").exists()
    outside = base / "outside"
    outside.mkdir()
    (root / "alias").symlink_to(outside, target_is_directory=True)
    archive(package, [("alias/file", b"bad")])
    run("install", "symlink-parent", str(package), success=False)
    assert not (outside / "file").exists()
    # Removing a leaf symlink must unlink the link, even if its target is a directory.
    (root / "leaf").symlink_to(outside, target_is_directory=True)
    db = root / "var/lib/bbpkg/installed"
    (db / "links.files").write_text("leaf\n")
    run("remove", "links")
    assert not (root / "leaf").is_symlink() and outside.is_dir()
    # Missing tar must fail without recording a successful installation.
    archive(package, [("test-file", b"hello")])
    run("install", "no-tar", str(package), success=False, env={"PATH": str(base)})
    assert not (db / "no-tar.files").exists()
    print("bbpkg integration checks passed")
