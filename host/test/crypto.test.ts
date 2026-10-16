// The pure-JS node:crypto shim (runs in JavaScriptCore on the host phone) must match Node exactly.
import nodeCrypto from "node:crypto";
import { describe, expect, it } from "vitest";
import shim from "../src/shims/crypto.ts";

const rnd = (n: number) => nodeCrypto.randomBytes(n);

describe("crypto shim vs node:crypto", () => {
  it("SHA-256 matches for many lengths", () => {
    for (const n of [0, 1, 55, 56, 63, 64, 65, 1000]) {
      const d = rnd(n);
      expect(shim.createHash("sha256").update(d).digest()).toEqual(nodeCrypto.createHash("sha256").update(d).digest());
    }
  });

  it("AES-128-CBC without padding (the game body codec) matches both ways", () => {
    const key = Buffer.from("78U#5e8!4@481eeU"), iv = Buffer.from("@#5U552&91127e)!");
    const data = rnd(160);
    const a = shim.createCipheriv("aes-128-cbc", key, iv).setAutoPadding(false);
    a.update(data);
    const enc = a.final();
    const n = nodeCrypto.createCipheriv("aes-128-cbc", key, iv).setAutoPadding(false);
    expect(enc).toEqual(Buffer.concat([n.update(data), n.final()]));
    const d = shim.createDecipheriv("aes-128-cbc", key, iv).setAutoPadding(false);
    d.update(enc);
    expect(d.final()).toEqual(data);
  });

  it("AES-256-CBC with PKCS7 (Photon) matches both ways", () => {
    const key = rnd(32), iv = Buffer.alloc(16);
    for (const len of [0, 5, 16, 33]) {
      const data = rnd(len);
      const n = nodeCrypto.createCipheriv("aes-256-cbc", key, iv);
      const enc = Buffer.concat([n.update(data), n.final()]);
      const d = shim.createDecipheriv("aes-256-cbc", key, iv);
      d.update(enc);
      expect(d.final()).toEqual(data);
      const s = shim.createCipheriv("aes-256-cbc", key, iv);
      s.update(data);
      expect(s.final()).toEqual(enc);
    }
  });

  it("Diffie-Hellman (Oakley 768, generator 22) agrees with Node", () => {
    const prime = shim.getDiffieHellman("modp1").getPrime();
    expect(prime).toEqual(nodeCrypto.getDiffieHellman("modp1").getPrime());
    const a = shim.createDiffieHellman(prime, Buffer.from([22]));
    a.generateKeys();
    const b = nodeCrypto.createDiffieHellman(prime, Buffer.from([22]));
    b.generateKeys();
    expect(a.computeSecret(b.getPublicKey())).toEqual(b.computeSecret(a.getPublicKey()));
  });

  it("randomUUID looks like a v4 uuid", () => {
    expect(shim.randomUUID()).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  });
});
