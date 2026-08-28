import { createController, LAYOUT_MOVY } from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import fs from "fs";
const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
let affected = [], total = 0;
for (const m of fx.modules) {
  const cp = m.chain_params; if (!cp) continue;
  const params = typeof cp === "string" ? JSON.parse(cp) : cp;
  const vals = Object.create(null);
  for (const p of params) {
    if (p.type === "enum" && Array.isArray(p.options)) vals[p.key] = String(Math.min(1, p.options.length - 1));
    else if (p.type === "int") vals[p.key] = String(p.min ?? 0);
    else vals[p.key] = "0.5";
  }
  let clock = 1000, served = false; total++;
  try {
    const ctl = createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return m.ui_hierarchy ? JSON.stringify(m.ui_hierarchy) : "";
        if (b === "chain_params") return typeof cp === "string" ? cp : JSON.stringify(cp);
        if (!served && b in vals) return "";
        return b in vals ? vals[b] : "";
      }, setParam: () => {}, announce: () => {}, now: () => clock });
    ctl.setLayout(LAYOUT_MOVY); ctl.load({ prefix: "synth" });
    const shot = () => { const fb = createFramebuffer(); ctl.render(drawContext(fb)); return Buffer.from(fb.pixels).toString("base64"); };
    for (let i = 0; i < 25; i++) { ctl.tick(); shot(); }
    served = true;
    const keys = ((ctl.page && ctl.page.keys) || []).filter(Boolean);
    let g = 0;
    while (g++ < 300 && !keys.every((k) => ctl.state.values[k] !== undefined)) { ctl.tick(); shot(); }
    const a = shot(); clock += 900;
    if (a !== shot()) affected.push(m.id);
  } catch (e) {}
}
console.log("driven:", total, " animate themselves in:", affected.length);
