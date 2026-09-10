#!/usr/bin/env node
/**
 * sync_taxonomy.mjs -- check or regenerate the catalog's embedded taxonomy.
 *
 *   node tools/catalog/sync_taxonomy.mjs --check   exit 1 on any error
 *   node tools/catalog/sync_taxonomy.mjs --write   rewrite the embedded block
 *
 * --write touches ONLY the top-level "taxonomy" key. Per-module subcategory and
 * tags are authored by hand; nothing here invents them.
 */

import fs from "node:fs";
import { loadJson, validate, embedBlock } from "./taxonomy.mjs";

const args = process.argv.slice(2);
const write = args.includes("--write");
// Every catalogued module now has one, so absence is an error rather than a
// module the taxonomy has not reached yet. --allow-missing-subcategory exists
// only for bisecting a catalog from before Task 2.
const requireSubcategory = !args.includes("--allow-missing-subcategory");

const taxonomy = loadJson("taxonomy.json");
const catalogPath = "module-catalog.json";

if (write) {
  const raw = fs.readFileSync(catalogPath, "utf8");
  const catalog = JSON.parse(raw);
  const rebuilt = {};
  for (const [k, v] of Object.entries(catalog)) {
    // The stale block is DROPPED, never carried across: copying it after the
    // fresh one is written re-instates the drift --write exists to repair, and
    // it is invisible whenever the block is already correct.
    if (k === "taxonomy") continue;
    rebuilt[k] = v;
    if (k === "host") rebuilt.taxonomy = embedBlock(taxonomy);
  }
  if (!rebuilt.taxonomy) rebuilt.taxonomy = embedBlock(taxonomy);
  delete rebuilt.modules;
  rebuilt.modules = catalog.modules;
  fs.writeFileSync(catalogPath, JSON.stringify(rebuilt, null, 2) + "\n");
  console.log("wrote taxonomy block into " + catalogPath);
  process.exit(0);
}

const errors = validate(taxonomy, loadJson(catalogPath), { requireSubcategory });
if (errors.length) {
  for (const e of errors) console.error("ERROR: " + e);
  console.error(errors.length + " error(s)");
  process.exit(1);
}
console.log("taxonomy ok");
