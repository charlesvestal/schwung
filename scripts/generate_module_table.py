#!/usr/bin/env python3
"""Generate a markdown module table from module-catalog.json for the README."""

import json
import sys

CATEGORY_ORDER = [
    ("sound_generator", "Sound Generators"),
    ("audio_fx", "Audio FX"),
    ("midi_fx", "MIDI FX"),
    ("overtake", "Overtake"),
    ("tool", "Tools"),
    ("utility", "Utilities"),
]

def main():
    with open("module-catalog.json", "r") as f:
        catalog = json.load(f)

    modules = catalog.get("modules", [])

    # Group by component_type
    groups = {}
    for mod in modules:
        ct = mod.get("component_type", "utility")
        groups.setdefault(ct, []).append(mod)

    lines = []
    for ct, heading in CATEGORY_ORDER:
        mods = groups.get(ct)
        if not mods:
            continue

        lines.append(f"### {heading}")
        lines.append("")
        lines.append("| Module | Description | Author |")
        lines.append("|--------|-------------|--------|")

        for mod in mods:
            name = mod["name"]
            repo = mod["github_repo"]
            url = f"https://github.com/{repo}"
            desc = mod.get("description", "")
            author = mod.get("author", "")
            # Escape pipe characters in fields
            desc = desc.replace("|", "\\|")
            author = author.replace("|", "\\|")
            lines.append(f"| [{name}]({url}) | {desc} | {author} |")

        lines.append("")

    print("\n".join(lines).rstrip())

if __name__ == "__main__":
    main()
