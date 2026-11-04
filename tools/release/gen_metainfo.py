#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the AppStream metainfo of OVT Studio (chapter 15 §15.8, WI-0.608 step 4).

    gen_metainfo.py --out org.openvideotools.Studio.metainfo.xml [--listings tools/release/listing_texts.toml]
                    [--version 1.0.0 --date 2026-11-04]

`project_license` comes from `[product].project_license` and the summary and description from
`[channels.flathub]` of listing_texts.toml, so the Flathub listing and the store texts have one source.
The output is deterministic (no timestamps). Check it with `appstreamcli validate --no-net <file>`.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import sys
import tomllib
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LISTINGS = REPO_ROOT / "tools" / "release" / "listing_texts.toml"
APP_ID = "org.openvideotools.Studio"  # chapter 0 D5: Flatpak app id
HOMEPAGE = "https://ovt.dev"
DEVELOPER = "The Open Video Tools contributors"


def paragraphs(text: str) -> list[str]:
    """Blank-line-separated paragraphs; build() turns bullet lines (•) into list items."""
    return [p.strip() for p in text.strip().split("\n\n") if p.strip()]


def build(listings: dict, version: str | None, date: str | None) -> ET.ElementTree:
    product = listings["product"]
    flathub = listings["channels"]["flathub"]
    root = ET.Element("component", type="desktop-application")
    ET.SubElement(root, "id").text = APP_ID
    ET.SubElement(root, "metadata_license").text = "CC0-1.0"
    ET.SubElement(root, "project_license").text = product["project_license"]
    ET.SubElement(root, "name").text = product["name"]
    ET.SubElement(root, "summary").text = flathub["short"].rstrip(".")
    dev = ET.SubElement(root, "developer", id="dev.ovt")
    ET.SubElement(dev, "name").text = DEVELOPER
    desc = ET.SubElement(root, "description")
    for para in paragraphs(flathub["long"]):
        lines = para.splitlines()
        items = [ln.lstrip("• ").strip() for ln in lines if ln.startswith("•")]
        text = " ".join(ln.strip() for ln in lines if not ln.startswith("•"))
        if text:
            ET.SubElement(desc, "p").text = text
        if items:
            ul = ET.SubElement(desc, "ul")
            for item in items:
                ET.SubElement(ul, "li").text = item
    ET.SubElement(root, "launchable", type="desktop-id").text = f"{APP_ID}.desktop"
    ET.SubElement(root, "url", type="homepage").text = HOMEPAGE
    ET.SubElement(root, "url", type="vcs-browser").text = "https://github.com/openvideotools/ovt"
    ET.SubElement(root, "content_rating", type="oars-1.1")
    categories = ET.SubElement(root, "categories")
    for c in ("AudioVideo", "Video", "Development"):
        ET.SubElement(categories, "category").text = c
    if version:
        releases = ET.SubElement(root, "releases")
        attrs = {"version": version}
        if date:
            attrs["date"] = date
        ET.SubElement(releases, "release", attrs)
    ET.indent(root, space="  ")
    return ET.ElementTree(root)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--listings", type=Path, default=DEFAULT_LISTINGS)
    ap.add_argument("--version", help="release version for <releases>")
    ap.add_argument("--date", help="release date (YYYY-MM-DD) for <releases>")
    args = ap.parse_args(argv)
    with args.listings.open("rb") as f:
        listings = tomllib.load(f)
    tree = build(listings, args.version, args.date)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("wb") as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write(b"<!-- Copyright 2026 The Open Video Tools contributors -->\n")
        tree.write(f, encoding="utf-8", xml_declaration=False)
        f.write(b"\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
