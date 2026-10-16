// Bundles the host for JavaScriptCore: one IIFE file, node:crypto -> the pure-JS shim, Buffer from
// the `buffer` package injected as a global. Output: build/host.js (embedded in the dylib).
import { build } from "esbuild";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
await build({
  entryPoints: [path.join(here, "src/jsc-entry.ts")],
  outfile: path.join(here, "../build/host.js"),
  bundle: true,
  format: "iife",
  platform: "neutral",
  target: "es2020",
  mainFields: ["browser", "module", "main"],
  alias: { "node:crypto": path.join(here, "src/shims/crypto.ts"), crypto: path.join(here, "src/shims/crypto.ts") },
  inject: [path.join(here, "src/shims/buffer-global.ts")],
  legalComments: "none",
  logLevel: "warning",
});
console.log("built build/host.js");
