// Off-device check of the dylib's startup patch against the server's own codec.
//   node test/check-patch.ts make  <out>   -> encrypted startup body with empty league tables
//   node test/check-patch.ts check <file>  -> decrypt the patched body and verify it
import { readFileSync, writeFileSync } from "node:fs";
import { decodeBody, encodeBody } from "../vendor/shared/codec.ts";

const [mode, file] = process.argv.slice(2);
const startup = JSON.parse(readFileSync("server/config/startup.json", "utf8")) as Record<string, unknown>;
const engine = { status_code: 200, ...startup, m_rtpvp_open: {}, m_rtpvp2_open: {}, note: "a \"quoted\" {brace} string" };

if (mode === "make") {
  writeFileSync(file!, encodeBody(engine));
} else {
  const patched = decodeBody<Record<string, unknown>>(readFileSync(file!, "utf8"));
  const fail = (m: string) => { console.error("FAIL:", m); process.exit(1); };
  for (const key of ["m_rtpvp_open", "m_rtpvp2_open"]) {
    const row = (patched[key] as Record<string, Record<string, string>>)?.["1"];
    if (!row) fail(`${key} missing row 1`);
    if (row!.ed_date_p !== "2099-12-31T23:59:59Z" || row!.ed_date_c !== "2000-01-02T00:00:00Z" || row!.hours !== "1".repeat(24)) fail(`${key} row wrong: ${JSON.stringify(row)}`);
  }
  for (const [k, v] of Object.entries(engine)) {
    if (k === "m_rtpvp_open" || k === "m_rtpvp2_open") continue;
    if (JSON.stringify(patched[k]) !== JSON.stringify(v)) fail(`field ${k} changed`);
  }
  console.log(`PASS: league windows opened, ${Object.keys(engine).length - 2} other fields byte-identical`);
}
