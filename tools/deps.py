#!/usr/bin/env python3
"""Fetch locked dependencies and apply reviewed patches without resetting changes."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
HOST_DEPS = ("fatfs", "littlefs", "cjson")


class DependencyError(RuntimeError):
    pass


def git(checkout, *args, env=None, input=None):
    result = subprocess.run(["git", "-C", str(checkout), *args], input=input,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if result.returncode:
        raise DependencyError(f"git {' '.join(args)} in {checkout}:\n{result.stderr.decode(errors='replace')}")
    return result.stdout


def load_lock(path):
    lock = json.loads(Path(path).read_text())
    if lock.get("version") != 1:
        raise DependencyError("Unsupported dependency lock version")
    deps = lock["dependencies"]
    for name, spec in deps.items():
        if not re.fullmatch(r"[a-z0-9-]+", name) or not re.fullmatch(r"[0-9a-f]{40}", spec["commit"]):
            raise DependencyError("Dependency names and full commit SHAs are required")
    return deps


def expected_tree(checkout, patches):
    # Compute the expected patched tree in a temporary index. Never alter the
    # dependency's real index, HEAD or existing worktree while calculating it.
    with tempfile.TemporaryDirectory(prefix="tufty-deps-index-") as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git(checkout, "read-tree", "HEAD", env=env)
        for patch in patches:
            git(checkout, "apply", "--ignore-space-change", "--cached", str(patch), env=env)
        return git(checkout, "write-tree", env=env).decode().strip()


def verify_index(checkout):
    if git(checkout, "diff", "--cached", "--name-only", "HEAD").strip():
        raise DependencyError(f"Staged changes in {checkout}; preserved. Review them before building.")
    if git(checkout, "ls-files", "--others", "--exclude-standard").strip():
        raise DependencyError(f"Untracked files in {checkout}; preserved. Review them before building.")


def patch_checkout(checkout, patches, check_only=False):
    verify_index(checkout)
    tree = expected_tree(checkout, patches)
    changed = git(checkout, "diff", "--name-only", "-z", "HEAD").decode().split("\0")
    # Only an exact expected patch result is accepted as an existing change.
    # An unrelated edit is never erased or silently compiled into the release.
    for name in filter(None, changed):
        path = checkout / name
        try:
            expected = git(checkout, "show", f"{tree}:{name}")
        except DependencyError:
            raise DependencyError(f"Unexpected deletion/change in {path}; preserved")
        if not path.is_file() or path.is_symlink() or path.read_bytes() != expected:
            raise DependencyError(f"Unexpected local change in {path}; preserved")
    for patch in patches:
        reverse = subprocess.run(["git", "-C", str(checkout), "apply", "--ignore-space-change", "--reverse", "--check", str(patch)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if reverse.returncode == 0:
            continue
        if check_only:
            raise DependencyError(f"Patch not applied: {patch}. Run python3 tools/deps.py")
        git(checkout, "apply", "--ignore-space-change", "--check", str(patch))
        git(checkout, "apply", "--ignore-space-change", str(patch))
        print(f"Applied {patch.name}", flush=True)
    if git(checkout, "diff", "--name-only", tree).strip():
        raise DependencyError(f"Checkout differs from the locked sources + patches: {checkout}")


def ensure_checkout(path, spec, check_only=False):
    path = Path(path)
    if not path.exists():
        if check_only:
            raise DependencyError(f"Missing {path}. Run python3 tools/deps.py")
        path.parent.mkdir(parents=True, exist_ok=True)
        # Failed downloads leave no half-initialized final checkout to mistake
        # for a user's repository on the next invocation.
        with tempfile.TemporaryDirectory(prefix=f".{path.name}-fetch-", dir=path.parent) as tmp:
            stage = Path(tmp) / "checkout"
            stage.mkdir()
            git(stage, "init", "--quiet")
            git(stage, "remote", "add", "origin", spec["url"])
            print(f"Fetching {path.name}@{spec['commit'][:12]}", flush=True)
            git(stage, "fetch", "--depth", "1", "origin", spec["commit"])
            git(stage, "checkout", "--quiet", "--detach", "FETCH_HEAD")
            stage.rename(path)
    if path.is_symlink() or not (path / ".git").exists():
        raise DependencyError(f"Not a managed Git checkout: {path}; preserved")
    if Path(git(path, "rev-parse", "--show-toplevel").decode().strip()).resolve() != path.resolve():
        raise DependencyError(f"Unexpected checkout root: {path}")
    actual = git(path, "rev-parse", "HEAD").decode().strip()
    if actual != spec["commit"]:
        raise DependencyError(f"{path.name}: expected {spec['commit']}, found {actual}; preserved. Review and update explicitly.")


def ensure_submodule(parent, relative, check_only=False):
    fields = git(parent, "ls-tree", "HEAD", "--", relative).decode().split()
    if len(fields) < 3 or fields[0] != "160000":
        raise DependencyError(f"Missing pinned gitlink for {relative}")
    path = parent / relative
    if not (path / ".git").exists():
        if check_only:
            raise DependencyError(f"Missing {relative}; run tools/deps.py with the required profile")
        print(f"Initializing SDK {relative}", flush=True)
        git(parent, "submodule", "update", "--init", "--depth", "1", "--", relative)
    if git(path, "rev-parse", "HEAD").decode().strip() != fields[2]:
        raise DependencyError(f"SDK {relative} differs from the pinned gitlink; preserved")
    patch_checkout(path, [], check_only)


def prepare(project=PROJECT, profile="firmware", radio=False, check_only=False):
    project = Path(project).resolve()
    deps = load_lock(project / "deps.lock.json")
    names = HOST_DEPS if profile == "host" else tuple(deps)
    if profile == "host" and radio:
        names = (*names, "pico-sdk")
    for name in names:
        spec = deps[name]
        checkout = project / "lib" / name
        ensure_checkout(checkout, spec, check_only)
        patches = [project / p for p in spec.get("patches", [])]
        patch_checkout(checkout, patches, check_only)
        if name == "pico-sdk":
            submodules = spec.get("submodules", []) + (spec.get("radio_submodules", []) if radio else [])
            for relative in submodules:
                ensure_submodule(checkout, relative, check_only)
        print(f"OK {name}@{spec['commit'][:12]}", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("firmware", "host"), default="firmware")
    parser.add_argument("--radio", action="store_true", help="include SDK CYW43/BTstack sources (also for host BLE tests)")
    parser.add_argument("--check", action="store_true", help="verify existing checkouts without downloading or applying patches")
    args = parser.parse_args(argv)
    try:
        prepare(profile=args.profile, radio=args.radio, check_only=args.check)
    except (DependencyError, OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"Dependency error: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
