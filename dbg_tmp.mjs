import fs from "node:fs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import * as RM from "./src/shared/param_pages/render_page_movy.mjs";

const strips = [];
function probe(label, hints) {
    const fb = createFramebuffer(128, 64);
    RM.drawFooter(drawContext(fb), hints);
    let right = 0;
    for (let yy = 55; yy < 64; yy++)
        for (let x = 0; x < 128; x++) if (fb.pixels[yy * 128 + x]) right = Math.max(right, x);
    const dropped = hints.length >= 3 && right <= 95;
    console.log(`${label.padEnd(34)} rightmost=${String(right).padStart(3)}  ` +
                (dropped ? "PAIR DROPPED" : "fits"));
    if (!dropped) strips.push(fb);
}

console.log("today:");
probe("JOG PAGE / CLK PUSH", [["JOG", "PAGE"], ["CLK", "PUSH"]]);
console.log("\nA — two pairs, one verb each key:");
probe("JOG PAGE / CLK FIRE / KNB FIRE", [["JOG", "PAGE"], ["CLK", "FIRE"], ["KNB", "FIRE"]]);
console.log("\nB — one pair naming both keys:");
probe("JOG PAGE / CLK/KNB PUSH", [["JOG", "PAGE"], ["CLK/KNB", "PUSH"]]);
probe("JOG PAGE / CLK/KNB FIRE", [["JOG", "PAGE"], ["CLK/KNB", "FIRE"]]);
probe("JOG PAGE / KNB/CLK PUSH", [["JOG", "PAGE"], ["KNB/CLK", "PUSH"]]);

/* Stack them so they can be compared at a glance. */
const H = 11;
const sheet = createFramebuffer(128, H * strips.length);
const sctx = drawContext(sheet);
strips.forEach((fb, i) => {
    for (let y = 55; y < 64; y++)
        for (let x = 0; x < 128; x++)
            if (fb.pixels[y * 128 + x]) sctx.fillRect(x, i * H + (y - 55), 1, 1, 1);
});
fs.writeFileSync("/tmp/footer-options.png", sheet.toPng(4));
console.log("\nwrote /tmp/footer-options.png (" + strips.length + " strips, in the order printed)");
