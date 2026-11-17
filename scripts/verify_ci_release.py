#!/usr/bin/env python3
"""Fail closed on wrong source/tag/gitlinks or mismatched packaged versions."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_FIELDS = ("bundleName", "versionName", "versionCode")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def load_app(root):
    from json5_data import load
    return load(root / "AppScope/app.json5")["app"]


def verify_source(root, expected_sha, release_tag):
    sha = git(root, "rev-parse", "HEAD")
    if sha != expected_sha:
        raise ValueError(f"checkout SHA mismatch: expected {expected_sha}, got {sha}")
    app = load_app(root)
    if release_tag:
        if not re.fullmatch(r"(?:rc|dev)-[0-9A-Za-z][0-9A-Za-z._-]*", release_tag):
            raise ValueError("invalid release tag")
        try:
            tag_sha = git(root, "rev-parse", f"refs/tags/{release_tag}^{{commit}}")
        except subprocess.CalledProcessError as error:
            raise ValueError(f"release tag not in checkout: {release_tag}") from error
        if tag_sha != sha:
            raise ValueError("release tag points to a different commit")
        if release_tag.startswith("rc-"):
            # A final snapshot keeps the numeric app version. Accept only this
            # explicit qualifier; a wrong version or arbitrary suffix fails.
            expected_tag = "rc-" + app["versionName"]
            if release_tag not in (expected_tag, expected_tag + "-final"):
                raise ValueError("RC tag does not match AppScope versionName")

    submodules = {}

    def visit(repo, prefix=""):
        # Read committed trees, not .gitmodules branch tips or a modified index.
        subprocess.run(["git", "-C", str(repo), "diff", "--quiet", "HEAD",
                        "--ignore-submodules=all"], check=True)
        for record in git(repo, "ls-tree", "-rz", "HEAD").split("\0"):
            if not record:
                continue
            meta, path = record.split("\t", 1)
            mode, kind, pinned = meta.split()
            if mode != "160000":
                continue
            child = repo / path
            label = prefix + path
            if not (child / ".git").exists():
                raise ValueError(f"submodule not initialized: {label}")
            actual = git(child, "rev-parse", "HEAD")
            if actual != pinned:
                raise ValueError(f"gitlink mismatch: {label}: {actual} != {pinned}")
            submodules[label] = pinned
            visit(child, label + "/")

    visit(root)
    patches = {str(p.relative_to(root)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted((root / "patches").rglob("*.patch"))}
    result = {"commit": sha, "releaseTag": release_tag,
            "app": {key: app[key] for key in APP_FIELDS},
            "submodules": submodules, "patches": patches}
    if (root/'docs/upstream-sync/wine-candidate.lock.json').is_file():
        from local_source_receipt import runtime_recipes
        result['runtimeRecipes']=runtime_recipes(root)
    return result


def record_runtime(root, manifest_path, wine_source, native_arch, wine_arch):
    from prepare_runtime_sources import load, verify
    lock,_ = load('wine',root)
    verify(Path(wine_source),lock)
    source = 'locked-proton'
    from local_source_receipt import runtime_recipes
    links = {}
    for record in git(root, 'ls-tree', '-rz', 'HEAD').split('\0'):
        if record:
            meta, name = record.split('\t', 1)
            mode, _, pinned = meta.split()
            if mode == '160000':
                links[name] = pinned
    if links.get('thirdparty/wine-valve') != lock['upstream']['commit']:
        raise ValueError('Wine public source gitlink differs from the recipe')
    value = json.loads(manifest_path.read_text(encoding='utf-8'))
    value['sourceBuild'] = dict(commit=git(root, 'rev-parse', 'HEAD'),
        nativeArch=native_arch, wineArch=wine_arch, wineSource=source, submodules=links,
        runtimeRecipes=runtime_recipes(root))
    manifest_path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    return value['sourceBuild']


def application_build_record(manifest):
    # A selected component runtime can predate the host application. Preserve
    # that origin, and bind the newly built host to its own effective sources.
    canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    return dict(schemaVersion=1, commit=manifest['commit'], app=manifest['app'],
                sourceMode=manifest.get('sourceMode', 'clean-ci-source'),
                sourceReceiptSha256=hashlib.sha256(canonical).hexdigest())


def verify_artifact(hap, manifest):
    with zipfile.ZipFile(hap) as archive:
        app = json.loads(archive.read("module.json"))["app"]
        for key in APP_FIELDS:
            if app.get(key) != manifest["app"][key]:
                raise ValueError(f"HAP {key} differs from checked-out source")
        for key in ("minAPIVersion", "targetAPIVersion"):
            # OHOS metadata may encode 6.1.0(23) as 60100023 or as 23.
            if app.get(key) not in (23, 60100023):
                raise ValueError(f"unexpected HAP {key}: {app.get(key)}")
        names = archive.namelist()
        abis = {name.split("/")[1] for name in names if name.startswith("libs/")
                and len(name.split("/")) > 2}
        if abis != {"arm64-v8a"}:
            raise ValueError(f"unexpected native ABIs: {sorted(abis)}")
        for library in ("libentry.so", "libwine_child.so",
                        "libwinehua_vtest_server.so",
                        "libvirglrenderer.so.1", "libvirgl_child.so"):
            with archive.open("libs/arm64-v8a/" + library) as stream:
                header = stream.read(20)
            if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
                raise ValueError(f"not an AArch64 ELF: {library}")
        runtime = archive.getinfo("resources/rawfile/wine-data.zip")
        if runtime.file_size == 0:
            raise ValueError("empty guest runtime")
        version = archive.read("resources/rawfile/wine_runtime.version").decode("utf-8").strip()
        runtime_fields = dict(field.split("=", 1) for field in version.split(";"))
        for module,component in (("wine-valve","wine"), ("fex","fex")):
            pinned = manifest['runtimeRecipes'][component]['commit']
            recorded = runtime_fields.get(module, "")
            if len(recorded) < 7 or not pinned.startswith(recorded):
                raise ValueError(f"HAP {module} runtime {recorded or 'unknown'} differs from pinned {pinned[:10]}")
        runtime_manifest = json.loads(archive.read("resources/rawfile/wine-runtime-manifest.json"))
        if runtime_manifest.get("payload") != "wine-data.zip":
            raise ValueError("invalid HAP runtime manifest")
        provenance = runtime_manifest.get('sourceBuild', {})
        if (provenance.get('commit') != manifest['commit'] or
                provenance.get('nativeArch') != 'arm64-v8a' or
                provenance.get('wineArch') != 'aarch64' or
                provenance.get('wineSource') != 'locked-proton' or
                provenance.get('runtimeRecipes') != manifest.get('runtimeRecipes') or
                runtime_fields.get('arch') != 'arm64-v8a:aarch64'):
            raise ValueError('HAP runtime source or architecture differs from checked-out source')
        for module in ('wine-valve', 'fex', 'box64', 'mesa', 'virglrenderer',
                       'gstreamer', 'gnutls', 'nettle', 'glib', 'pcre2', 'dxvk', 'dxvk-modern'):
            name = 'thirdparty/' + module
            if provenance.get('submodules', {}).get(name) != manifest['submodules'][name]:
                raise ValueError('HAP runtime submodule differs from pinned source: ' + name)
        runtime_digest = hashlib.sha256()
        with archive.open(runtime) as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                runtime_digest.update(chunk)
        if runtime_digest.hexdigest() != runtime_manifest.get("payloadSha256"):
            raise ValueError("HAP guest runtime differs from its SHA-256 manifest")
    digest = hashlib.sha256()
    with hap.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"commit": manifest["commit"], "releaseTag": manifest["releaseTag"],
            "app": manifest["app"], "abi": "arm64-v8a", "api": 23,
            "file": hap.name, "size": hap.stat().st_size, "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    source = modes.add_parser("source")
    source.add_argument("--expected-sha", required=True)
    source.add_argument("--release-tag", default="")
    source.add_argument("--output", type=Path, required=True)
    artifact = modes.add_parser("artifact")
    artifact.add_argument("--manifest", type=Path, required=True)
    artifact.add_argument("--hap", type=Path, required=True)
    artifact.add_argument("--output", type=Path, required=True)
    runtime = modes.add_parser('runtime')
    runtime.add_argument('--manifest', type=Path, required=True)
    runtime.add_argument('--wine-source', required=True)
    runtime.add_argument('--native-arch', required=True)
    runtime.add_argument('--wine-arch', required=True)
    args = parser.parse_args()
    if args.mode == "source":
        result = verify_source(ROOT, args.expected_sha, args.release_tag)
    elif args.mode == 'artifact':
        result = verify_artifact(args.hap, json.loads(args.manifest.read_text(encoding="utf-8")))
    else:
        record_runtime(ROOT, args.manifest, args.wine_source, args.native_arch, args.wine_arch)
        print('Recorded pinned source identity in runtime manifest')
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"CI version gate: {error}", file=sys.stderr)
        sys.exit(1)
