/**
 * taxonomy.mjs -- the module taxonomy vocabulary, and what it refuses.
 *
 * Pure by design: tests/host/ drives these functions under node directly, and
 * nothing here reads the network or the device.
 *
 * THE ONE RULE THAT IS NOT OBVIOUS. A derived tag is COMPUTED, never authored.
 * `needs-assets` restates `requires`, so a hand-written copy is a fact that can
 * go stale against the thing it describes -- the module drops its ROM
 * dependency, `requires` is cleared, and the tag stays, filtering the module
 * out of a list it now belongs in. Both directions are errors here: missing
 * when it should be present, and present when it should not be.
 */

import fs from "node:fs";

export function loadJson(path) {
  return JSON.parse(fs.readFileSync(path, "utf8"));
}

/** Tags this entry MUST carry, computed from data the catalog already holds. */
export function derivedTags(entry) {
  const out = [];
  const req = entry.requires;
  if (typeof req === "string" && req.trim() !== "") out.push("needs-assets");
  return out;
}

/** The block embedded into module-catalog.json so one fetch carries labels. */
export function embedBlock(taxonomy) {
  return {
    taxonomy_version: taxonomy.taxonomy_version,
    subcategories: taxonomy.subcategories,
    tags: taxonomy.tags,
  };
}

/**
 * @param {object} taxonomy  parsed taxonomy.json
 * @param {object} catalog   parsed module-catalog.json
 * @param {{requireSubcategory?: boolean}} opts
 * @returns {string[]} human-readable errors; empty means valid
 */
export function validate(taxonomy, catalog, opts = {}) {
  const requireSubcategory = opts.requireSubcategory === true;
  const errors = [];

  const byType = new Map();
  for (const [ct, list] of Object.entries(taxonomy.subcategories || {})) {
    const ids = new Set();
    for (const sc of list || []) {
      if (!sc || !sc.id || !sc.label) {
        errors.push(`taxonomy: ${ct} has an entry missing id or label`);
        continue;
      }
      if (ids.has(sc.id)) errors.push(`taxonomy: ${ct} lists "${sc.id}" twice`);
      ids.add(sc.id);
    }
    byType.set(ct, ids);
  }

  const knownTags = new Set(taxonomy.tags || []);

  const embedded = catalog.taxonomy;
  const want = JSON.stringify(embedBlock(taxonomy));
  if (JSON.stringify(embedded ?? null) !== want) {
    errors.push(
      'catalog: embedded "taxonomy" block is out of date -- run: node tools/catalog/sync_taxonomy.mjs --write'
    );
  }

  for (const m of catalog.modules || []) {
    const where = `module "${m.id}"`;
    const allowed = byType.get(m.component_type);
    if (!allowed) {
      errors.push(`${where}: component_type "${m.component_type}" is not in taxonomy.json`);
      continue;
    }

    if (m.subcategory === undefined || m.subcategory === null || m.subcategory === "") {
      if (requireSubcategory) errors.push(`${where}: no subcategory`);
    } else if (!allowed.has(m.subcategory)) {
      errors.push(
        `${where}: subcategory "${m.subcategory}" is not one of ${m.component_type}'s`
      );
    }

    const tags = m.tags;
    if (tags === undefined) continue;
    if (!Array.isArray(tags)) {
      errors.push(`${where}: tags is not an array`);
      continue;
    }
    for (const t of tags) {
      if (!knownTags.has(t)) errors.push(`${where}: unknown tag "${t}"`);
    }
    if (new Set(tags).size !== tags.length) errors.push(`${where}: duplicate tag`);
    const sorted = [...tags].sort();
    if (JSON.stringify(tags) !== JSON.stringify(sorted)) {
      errors.push(`${where}: tags are not sorted`);
    }
    const derived = derivedTags(m);
    for (const d of derived) {
      if (!tags.includes(d)) errors.push(`${where}: missing derived tag "${d}"`);
    }
    for (const d of ["needs-assets"]) {
      if (tags.includes(d) && !derived.includes(d)) {
        errors.push(`${where}: derived tag "${d}" written by hand, but nothing derives it`);
      }
    }
  }

  return errors;
}
