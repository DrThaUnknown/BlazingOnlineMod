// The native mod (src/bo.h) and the JS host (host/src/version.ts) report versions to each other;
// they must never drift apart.
import fs from "node:fs";
import { describe, expect, it } from "vitest";
import { HOST_VERSION, PROTOCOL } from "../src/version.ts";

const header = fs.readFileSync("src/bo.h", "utf8");
const define = (name: string) => header.match(new RegExp("#define " + name + " \"?([0-9.]+)"))?.[1];

describe("version constants", () => {
  it("host version equals BO_VERSION", () => expect(HOST_VERSION).toBe(define("BO_VERSION")));
  it("host protocol equals BO_PROTOCOL", () => expect(String(PROTOCOL)).toBe(define("BO_PROTOCOL")));
});
