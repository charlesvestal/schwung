#!/usr/bin/env python3
"""
Migrate module.json from chain_params to ui_hierarchy format.

Converts chain_params array entries into full param objects within
ui_hierarchy levels, then removes chain_params.

Usage: ./scripts/migrate-module-params.py path/to/module.json [--dry-run]
"""

import json
import sys
from pathlib import Path


def convert_param(cp):
    """Convert a chain_params entry to a ui_hierarchy param object."""
    param = {
        "key": cp["key"],
        "label": cp.get("name", cp["key"]),
        "type": cp["type"]
    }

    if cp["type"] == "enum":
        if "options" in cp:
            param["options"] = cp["options"]
        if "default" in cp:
            # Convert string default to index if options exist
            if isinstance(cp["default"], str) and "options" in cp:
                try:
                    param["default"] = cp["options"].index(cp["default"])
                except ValueError:
                    param["default"] = 0
            else:
                param["default"] = cp["default"]
    else:
        # Numeric types (float, int)
        if "min" in cp:
            param["min"] = cp["min"]
        if "max" in cp:
            param["max"] = cp["max"]
        if "max_param" in cp:
            param["max_param"] = cp["max_param"]
        if "default" in cp:
            param["default"] = cp["default"]
        if "step" in cp:
            param["step"] = cp["step"]

    # Optional display fields
    if "unit" in cp:
        param["unit"] = cp["unit"]
    if "display_format" in cp:
        param["display_format"] = cp["display_format"]

    return param


def migrate_module(module_path, dry_run=False):
    with open(module_path, "r") as f:
        module = json.load(f)

    caps = module.get("capabilities", {})
    chain_params = caps.get("chain_params", [])
    ui_hierarchy = caps.get("ui_hierarchy", {})

    if not chain_params:
        print(f"No chain_params found in {module_path}")
        return False

    # Build lookup from key -> converted param object
    param_lookup = {}
    for cp in chain_params:
        converted = convert_param(cp)
        param_lookup[cp["key"]] = converted

    # Get existing hierarchy or create new
    levels = ui_hierarchy.get("levels", {})

    if not levels:
        # No hierarchy at all - create root level with all params
        root = {
            "label": module.get("name", "Module"),
            "params": list(param_lookup.values()),
            "knobs": [cp["key"] for cp in chain_params[:8]]
        }
        levels["root"] = root
    else:
        # Replace string references in params with full objects
        for level_name, level in levels.items():
            params = level.get("params", [])
            new_params = []
            for p in params:
                if isinstance(p, str):
                    # String reference - replace with full object if found
                    if p in param_lookup:
                        new_params.append(param_lookup[p])
                    else:
                        print(f"  Warning: param '{p}' in level '{level_name}' not found in chain_params, keeping as string")
                        new_params.append(p)
                elif isinstance(p, dict):
                    if "level" in p:
                        # Navigation item - keep as-is
                        new_params.append(p)
                    elif "key" in p and "type" not in p:
                        # Partial param (key only) - merge with chain_params
                        key = p["key"]
                        if key in param_lookup:
                            merged = param_lookup[key].copy()
                            # Keep any overrides from existing hierarchy
                            for k, v in p.items():
                                if k != "key":
                                    merged[k] = v
                            new_params.append(merged)
                        else:
                            new_params.append(p)
                    else:
                        # Already a full object - keep as-is
                        new_params.append(p)
                else:
                    new_params.append(p)
            level["params"] = new_params

            # Update level name to use "label" consistently
            if "name" in level and "label" not in level:
                level["label"] = level.pop("name")

        # Find chain_params not referenced in any level
        referenced_keys = set()
        for level in levels.values():
            for p in level.get("params", []):
                if isinstance(p, dict) and "key" in p:
                    referenced_keys.add(p["key"])
        unreferenced = [param_lookup[k] for k in param_lookup if k not in referenced_keys]
        if unreferenced:
            # Append to root level
            root = levels.get("root", {})
            root.setdefault("params", []).extend(unreferenced)
            levels["root"] = root
            print(f"  Added {len(unreferenced)} unreferenced params to root: {[p['key'] for p in unreferenced]}", file=sys.stderr)

    # Update hierarchy
    ui_hierarchy["levels"] = levels
    caps["ui_hierarchy"] = ui_hierarchy

    # Remove chain_params
    del caps["chain_params"]

    module["capabilities"] = caps

    if dry_run:
        print(json.dumps(module, indent=2))
        return True

    # Write back
    with open(module_path, "w") as f:
        json.dump(module, f, indent=2)
        f.write("\n")

    print(f"Migrated {module_path}")
    print(f"  Converted {len(chain_params)} chain_params to ui_hierarchy")
    print(f"  Levels: {', '.join(levels.keys())}")
    return True


if __name__ == "__main__":
    dry_run = "--dry-run" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]

    if len(args) != 1:
        print("Usage: ./scripts/migrate-module-params.py path/to/module.json [--dry-run]")
        sys.exit(1)

    module_path = Path(args[0])
    if not module_path.exists():
        print(f"File not found: {module_path}")
        sys.exit(1)

    if migrate_module(module_path, dry_run):
        if not dry_run:
            print("Done!")
    else:
        print("Nothing to migrate")
