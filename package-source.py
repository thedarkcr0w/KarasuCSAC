#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import zipfile
from pathlib import Path, PurePosixPath


VERSION = "1.0.12"
AMBUILD_COMMIT = "d89ec91a7ac2607da07b50bb62346f9a10e9a998"
ARCHIVE_ROOT = f"CS2AC-{VERSION}-source"


def git(repo: Path, *args: str, text: bool = True):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=text)


def require_clean(repo: Path, name: str) -> None:
    if git(repo, "status", "--porcelain", "--untracked-files=no").strip():
        raise SystemExit(f"{name} has tracked changes. Commit them before creating the source package.")


def tracked_files(repo: Path, prefix: PurePosixPath):
    output = git(repo, "ls-files", "--stage", "-z", text=False)
    for record in output.split(b"\0"):
        if not record:
            continue
        metadata, relative = record.split(b"\t", 1)
        mode = metadata.split(b" ", 1)[0].decode()
        if mode == "160000":
            continue
        relative_path = Path(os.fsdecode(relative))
        yield repo / relative_path, prefix / PurePosixPath(relative_path.as_posix()), mode


def submodules(root: Path):
    result = []
    for line in git(root, "submodule", "status", "--recursive").splitlines():
        if not line or line[0] != " ":
            raise SystemExit(f"A submodule is not at its recorded commit: {line.strip()}")
        commit, relative = line[1:].split(maxsplit=2)[:2]
        path = root / Path(relative)
        if git(path, "rev-parse", "HEAD").strip() != commit:
            raise SystemExit(f"The {relative} submodule is not at its recorded commit.")
        require_clean(path, relative)
        result.append((path, PurePosixPath(relative), commit))
    return result


def file_bytes(path: Path, mode: str) -> bytes:
    if mode == "120000":
        return os.readlink(path).encode()
    return path.read_bytes()


def add_file(archive: zipfile.ZipFile, name: PurePosixPath, source: Path, mode: str, timestamp) -> None:
    info = zipfile.ZipInfo(f"{ARCHIVE_ROOT}/{name.as_posix()}", timestamp)
    info.create_system = 3
    permissions = 0o120777 if mode == "120000" else (0o100755 if mode == "100755" else 0o100644)
    info.external_attr = permissions << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, file_bytes(source, mode), compresslevel=9)


def add_text(archive: zipfile.ZipFile, name: str, content: str, timestamp) -> None:
    info = zipfile.ZipInfo(f"{ARCHIVE_ROOT}/{name}", timestamp)
    info.create_system = 3
    info.external_attr = 0o644 << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, content.encode(), compresslevel=9)


def main() -> None:
    parser = argparse.ArgumentParser(description="Create the complete CS2AC source archive.")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    require_clean(root, "CS2AC")
    commit = git(root, "rev-parse", "HEAD").strip()
    commit_time = int(git(root, "show", "-s", "--format=%ct", "HEAD").strip())
    timestamp = tuple(__import__("time").gmtime(max(commit_time, 315532800))[:6])

    dependencies = submodules(root)
    ambuild = root / ".tools" / "ambuild"
    if not (ambuild / "ambuild2").is_dir():
        raise SystemExit("Pinned AMBuild is missing. Run bootstrap.sh or bootstrap.ps1 first.")
    require_clean(ambuild, "AMBuild")
    ambuild_commit = git(ambuild, "rev-parse", "HEAD").strip()
    if ambuild_commit != AMBUILD_COMMIT:
        raise SystemExit("AMBuild is not at the revision pinned by CS2AC.")

    image_match = re.search(r'IMAGE="([^"]+)"', (root / "build-linux.sh").read_text())
    if not image_match:
        raise SystemExit("The pinned Steam Runtime image could not be read.")

    entries = list(tracked_files(root, PurePosixPath()))
    for path, prefix, _ in dependencies:
        entries.extend(tracked_files(path, prefix))
    entries.extend(tracked_files(ambuild, PurePosixPath(".tools/ambuild")))
    entries.sort(key=lambda entry: entry[1].as_posix())

    names = [entry[1].as_posix() for entry in entries]
    if len(names) != len(set(names)):
        raise SystemExit("The source package contains duplicate paths.")
    required = {
        "licenses/CLIENTCVARVALUE-GPL-3.0.txt",
        "licenses/CS2KZ-AGPL-3.0.txt",
        "licenses/DYNLIBUTILS-MIT.txt",
    }
    if not required.issubset(names):
        raise SystemExit("The source package is missing a required legal file.")

    metadata = [
        f"CS2AC version: {VERSION}",
        f"CS2AC commit: {commit}",
        f"AMBuild commit: {ambuild_commit}",
        f"Steam Runtime image: {image_match.group(1)}",
        "",
        "Git submodules:",
    ]
    metadata.extend(f"{prefix.as_posix()}: {revision}" for _, prefix, revision in dependencies)
    metadata.append("")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", allowZip64=True) as archive:
        for source, name, mode in entries:
            add_file(archive, name, source, mode, timestamp)
        add_text(archive, "SOURCE_DEPENDENCIES.txt", "\n".join(metadata), timestamp)

    with zipfile.ZipFile(args.output) as archive:
        broken = archive.testzip()
        if broken:
            raise SystemExit(f"The source package failed its integrity check at {broken}.")

    print(f"Source package ready: {args.output}")


if __name__ == "__main__":
    main()
