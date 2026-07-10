#!/usr/bin/env python3
"""Build same-origin versioned web-installer manifests for GitHub Pages.

The browser installer is happiest when every binary it flashes comes from the
same origin as the manifest. GitHub Release asset URLs redirect through GitHub's
download infrastructure and can fail inside esp-web-tools with a generic
"Failed to fetch". This script mirrors recent release assets into the Pages
artifact under releases/<tag>/ and writes release-index.json for the dropdown.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path


PARTS = [
    ("bootloader.bin", 0x0),
    ("partition-table.bin", 0x8000),
    ("ota_data_initial.bin", 0x109000),
    ("CapsuleRadar-ota.bin", 0x110000),
    ("PrintSphere-ota.bin", 0x610000),
]


def log(message: str) -> None:
    print(f"[release-index] {message}", flush=True)


def request_json(url: str) -> object:
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "capsule-companion-webflasher",
    }
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=45) as resp:
        return json.loads(resp.read().decode("utf-8"))


def download(url: str, dest: Path) -> None:
    headers = {"User-Agent": "capsule-companion-webflasher"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=180) as resp, dest.open("wb") as fh:
        shutil.copyfileobj(resp, fh)


def write_manifest(version_dir: Path, tag: str) -> None:
    manifest = {
        "name": "Capsule Companion",
        "version": tag.removeprefix("v"),
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [
                    {"path": filename, "offset": offset}
                    for filename, offset in PARTS
                ],
            }
        ],
    }
    (version_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )


def copy_current_release(site: Path, tag: str) -> bool:
    if not tag:
        return False
    missing = [name for name, _ in PARTS if not (site / name).exists()]
    if missing:
        log(f"current release {tag} not mirrored from local site; missing {', '.join(missing)}")
        return False
    version_dir = site / "releases" / tag
    version_dir.mkdir(parents=True, exist_ok=True)
    for name, _ in PARTS:
        shutil.copy2(site / name, version_dir / name)
    write_manifest(version_dir, tag)
    log(f"mirrored current build {tag}")
    return True


def release_asset_map(release: dict) -> dict[str, str]:
    return {
        asset.get("name", ""): asset.get("browser_download_url", "")
        for asset in release.get("assets", [])
    }


def mirror_release(site: Path, release: dict) -> bool:
    tag = release.get("tag_name", "")
    assets = release_asset_map(release)
    missing = [name for name, _ in PARTS if not assets.get(name)]
    if missing:
        log(f"skipping {tag}: missing {', '.join(missing)}")
        return False

    version_dir = site / "releases" / tag
    version_dir.mkdir(parents=True, exist_ok=True)
    for name, _ in PARTS:
        dest = version_dir / name
        if dest.exists() and dest.stat().st_size > 0:
            continue
        log(f"downloading {tag}/{name}")
        download(assets[name], dest)
    write_manifest(version_dir, tag)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--site", default="site", type=Path)
    parser.add_argument("--limit", default=12, type=int)
    parser.add_argument("--current-tag", default=os.environ.get("CURRENT_TAG", ""))
    args = parser.parse_args()

    site = args.site
    site.mkdir(parents=True, exist_ok=True)
    (site / "releases").mkdir(parents=True, exist_ok=True)

    repo = os.environ.get("GITHUB_REPOSITORY", "Lerxtwood/capsule-radar")
    api_url = f"https://api.github.com/repos/{repo}/releases?per_page={args.limit}"

    mirrored: set[str] = set()
    if copy_current_release(site, args.current_tag):
        mirrored.add(args.current_tag)

    try:
        releases = request_json(api_url)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        log(f"warning: could not fetch release list: {exc}")
        releases = []

    if not isinstance(releases, list):
        releases = []

    entries = []
    for release in releases:
        if release.get("draft"):
            continue
        tag = release.get("tag_name", "")
        if not tag:
            continue
        if mirror_release(site, release):
            mirrored.add(tag)
            entries.append(
                {
                    "tag": tag,
                    "version": tag.removeprefix("v"),
                    "name": release.get("name") or tag,
                    "manifest": f"releases/{tag}/manifest.json",
                }
            )

    if args.current_tag and args.current_tag not in {entry["tag"] for entry in entries}:
        if (site / "releases" / args.current_tag / "manifest.json").exists():
            entries.insert(
                0,
                {
                    "tag": args.current_tag,
                    "version": args.current_tag.removeprefix("v"),
                    "name": args.current_tag,
                    "manifest": f"releases/{args.current_tag}/manifest.json",
                },
            )

    if entries:
        entries[0]["latest"] = True

    (site / "release-index.json").write_text(
        json.dumps(entries, indent=2) + "\n",
        encoding="utf-8",
    )
    log(f"wrote {len(entries)} release-index entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
