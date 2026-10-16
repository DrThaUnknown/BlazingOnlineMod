// Pure-JS stand-in for the parts of `node:crypto` the host server uses, so the same code runs in
// iOS's JavaScriptCore (no Node). The bundler aliases `node:crypto` to this file; under Node the
// test suite checks it against the real module (host/test/crypto.test.ts).
//   - AES-128/256-CBC (game body codec, Photon's encrypted Authenticate), with or without PKCS7
//   - SHA-256 (Photon key derivation)
//   - Diffie-Hellman over RFC 2409 Oakley group 1 (Photon key exchange), BigInt modpow
//   - randomBytes / randomUUID / randomInt from an injectable source (setRandomSource)
import { Buffer } from "buffer";

// ---- randomness ----
let randomSource: (n: number) => Uint8Array = (n) => {
  const g = (globalThis as { crypto?: { getRandomValues?(a: Uint8Array): Uint8Array } }).crypto;
  const out = new Uint8Array(n);
  if (g?.getRandomValues) return g.getRandomValues(out);
  for (let i = 0; i < n; i++) out[i] = Math.floor(Math.random() * 256);
  return out;
};
/** The iOS host passes arc4random-backed bytes here (native binding). */
export function setRandomSource(fn: (n: number) => Uint8Array): void {
  randomSource = fn;
}
export function randomBytes(n: number): Buffer {
  return Buffer.from(randomSource(n));
}
export function randomUUID(): string {
  const b = randomBytes(16);
  b[6] = (b[6]! & 0x0f) | 0x40;
  b[8] = (b[8]! & 0x3f) | 0x80;
  const h = b.toString("hex");
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}
export function randomInt(min: number, max?: number): number {
  if (max === undefined) [min, max] = [0, min];
  const span = max - min;
  const v = randomBytes(4).readUInt32BE(0);
  return min + (v % span);
}

// ---- SHA-256 ----
const K256 = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

export function sha256(data: Uint8Array): Buffer {
  const bitLen = data.length * 8;
  const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
  padded.set(data);
  padded[data.length] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 4, bitLen >>> 0);
  view.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000));
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const rotr = (x: number, n: number) => (x >>> n) | (x << (32 - n));
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15]!, 7) ^ rotr(w[i - 15]!, 18) ^ (w[i - 15]! >>> 3);
      const s1 = rotr(w[i - 2]!, 17) ^ rotr(w[i - 2]!, 19) ^ (w[i - 2]! >>> 10);
      w[i] = (w[i - 16]! + s0 + w[i - 7]! + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h as unknown as number[];
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e!, 6) ^ rotr(e!, 11) ^ rotr(e!, 25);
      const ch = (e! & f!) ^ (~e! & g!);
      const t1 = (hh! + S1 + ch + K256[i]! + w[i]!) >>> 0;
      const S0 = rotr(a!, 2) ^ rotr(a!, 13) ^ rotr(a!, 22);
      const maj = (a! & b!) ^ (a! & c!) ^ (b! & c!);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d! + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0]! + a!) >>> 0; h[1] = (h[1]! + b!) >>> 0; h[2] = (h[2]! + c!) >>> 0; h[3] = (h[3]! + d!) >>> 0;
    h[4] = (h[4]! + e!) >>> 0; h[5] = (h[5]! + f!) >>> 0; h[6] = (h[6]! + g!) >>> 0; h[7] = (h[7]! + hh!) >>> 0;
  }
  const out = Buffer.alloc(32);
  for (let i = 0; i < 8; i++) out.writeUInt32BE(h[i]!, i * 4);
  return out;
}

export function createHash(alg: string) {
  if (alg !== "sha256") throw new Error(`crypto shim: unsupported hash ${alg}`);
  const parts: Uint8Array[] = [];
  return {
    update(d: Uint8Array | string) {
      parts.push(typeof d === "string" ? Buffer.from(d, "utf8") : d);
      return this;
    },
    digest(enc?: "hex") {
      const out = sha256(Buffer.concat(parts));
      return enc === "hex" ? out.toString("hex") : out;
    },
  };
}

// ---- AES (FIPS-197) ----
const SBOX = new Uint8Array(256);
const INV_SBOX = new Uint8Array(256);
(() => {
  let p = 1, q = 1;
  do {
    p = p ^ ((p << 1) & 0xff) ^ (p & 0x80 ? 0x1b : 0);
    q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff;
    if (q & 0x80) q ^= 0x09;
    const x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4));
    SBOX[p] = (x ^ 0x63) & 0xff;
  } while (p !== 1);
  SBOX[0] = 0x63;
  for (let i = 0; i < 256; i++) INV_SBOX[SBOX[i]!] = i;
})();
const xt = (b: number) => ((b << 1) ^ (b & 0x80 ? 0x1b : 0)) & 0xff;
const mul = (a: number, b: number) => {
  let r = 0;
  while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
  return r;
};

function expandKey(key: Uint8Array): Uint8Array[] {
  const nk = key.length / 4, nr = nk + 6;
  const w = new Uint8Array(16 * (nr + 1));
  w.set(key);
  let rcon = 1;
  for (let i = nk; i < 4 * (nr + 1); i++) {
    let t = w.slice((i - 1) * 4, i * 4);
    if (i % nk === 0) {
      t = new Uint8Array([SBOX[t[1]!]! ^ rcon, SBOX[t[2]!]!, SBOX[t[3]!]!, SBOX[t[0]!]!]);
      rcon = xt(rcon);
    } else if (nk > 6 && i % nk === 4) {
      t = t.map((b) => SBOX[b]!);
    }
    for (let j = 0; j < 4; j++) w[i * 4 + j] = w[(i - nk) * 4 + j]! ^ t[j]!;
  }
  const rounds: Uint8Array[] = [];
  for (let r = 0; r <= nr; r++) rounds.push(w.slice(r * 16, r * 16 + 16));
  return rounds;
}

function encryptBlock(rk: Uint8Array[], input: Uint8Array): Uint8Array {
  const s = new Uint8Array(16);
  for (let i = 0; i < 16; i++) s[i] = input[i]! ^ rk[0]![i]!;
  const nr = rk.length - 1;
  for (let r = 1; r <= nr; r++) {
    for (let i = 0; i < 16; i++) s[i] = SBOX[s[i]!]!;
    const t = s.slice();
    for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) s[c * 4 + row] = t[((c + row) % 4) * 4 + row]!;
    if (r !== nr) {
      for (let c = 0; c < 4; c++) {
        const a0 = s[c * 4]!, a1 = s[c * 4 + 1]!, a2 = s[c * 4 + 2]!, a3 = s[c * 4 + 3]!;
        s[c * 4] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
        s[c * 4 + 1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
        s[c * 4 + 2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
        s[c * 4 + 3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
      }
    }
    for (let i = 0; i < 16; i++) s[i] = s[i]! ^ rk[r]![i]!;
  }
  return s;
}

function decryptBlock(rk: Uint8Array[], input: Uint8Array): Uint8Array {
  const nr = rk.length - 1;
  const s = new Uint8Array(16);
  for (let i = 0; i < 16; i++) s[i] = input[i]! ^ rk[nr]![i]!;
  for (let r = nr - 1; r >= 0; r--) {
    const t = s.slice();
    for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) s[((c + row) % 4) * 4 + row] = t[c * 4 + row]!;
    for (let i = 0; i < 16; i++) s[i] = INV_SBOX[s[i]!]!;
    for (let i = 0; i < 16; i++) s[i] = s[i]! ^ rk[r]![i]!;
    if (r !== 0) {
      for (let c = 0; c < 4; c++) {
        const a0 = s[c * 4]!, a1 = s[c * 4 + 1]!, a2 = s[c * 4 + 2]!, a3 = s[c * 4 + 3]!;
        s[c * 4] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
        s[c * 4 + 1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
        s[c * 4 + 2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
        s[c * 4 + 3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
      }
    }
  }
  return s;
}

function cbc(alg: string, key: Uint8Array, iv: Uint8Array, decrypt: boolean) {
  const size = alg === "aes-128-cbc" ? 16 : alg === "aes-256-cbc" ? 32 : 0;
  if (!size || key.length !== size || iv.length !== 16) throw new Error(`crypto shim: unsupported cipher ${alg}`);
  const rk = expandKey(key);
  let autoPadding = true;
  let chunks: Uint8Array[] = [];
  return {
    setAutoPadding(on: boolean) {
      autoPadding = on;
      return this;
    },
    update(d: Uint8Array) {
      chunks.push(d);
      return Buffer.alloc(0);
    },
    final() {
      let data = Buffer.concat(chunks);
      chunks = [];
      if (!decrypt && autoPadding) {
        const pad = 16 - (data.length % 16);
        data = Buffer.concat([data, Buffer.alloc(pad, pad)]);
      }
      if (data.length % 16) throw new Error("crypto shim: data is not a multiple of the block size");
      const out = Buffer.alloc(data.length);
      let prev: Uint8Array = Uint8Array.from(iv);
      for (let off = 0; off < data.length; off += 16) {
        const block = data.subarray(off, off + 16);
        if (decrypt) {
          const plain = decryptBlock(rk, block);
          for (let i = 0; i < 16; i++) out[off + i] = plain[i]! ^ prev[i]!;
          prev = Uint8Array.from(block);
        } else {
          const x = new Uint8Array(16);
          for (let i = 0; i < 16; i++) x[i] = block[i]! ^ prev[i]!;
          prev = encryptBlock(rk, x);
          out.set(prev, off);
        }
      }
      if (decrypt && autoPadding) {
        const pad = out[out.length - 1]!;
        if (!pad || pad > 16) throw new Error("crypto shim: bad PKCS7 padding");
        return out.subarray(0, out.length - pad);
      }
      return out;
    },
  };
}
export function createCipheriv(alg: string, key: Uint8Array, iv: Uint8Array) {
  return cbc(alg, key, iv, false);
}
export function createDecipheriv(alg: string, key: Uint8Array, iv: Uint8Array) {
  return cbc(alg, key, iv, true);
}

// ---- Diffie-Hellman (RFC 2409 Oakley group 1, 768-bit) ----
const MODP1 =
  "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD" +
  "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A63A3620FFFFFFFFFFFFFFFF";

const toBig = (b: Uint8Array) => (b.length ? BigInt("0x" + Buffer.from(b).toString("hex")) : 0n);
function toBuf(n: bigint, size?: number): Buffer {
  let hex = n.toString(16);
  if (hex.length % 2) hex = "0" + hex;
  const b = Buffer.from(hex, "hex");
  return size && b.length < size ? Buffer.concat([Buffer.alloc(size - b.length), b]) : b;
}
function modPow(base: bigint, exp: bigint, mod: bigint): bigint {
  let result = 1n;
  base %= mod;
  while (exp > 0n) {
    if (exp & 1n) result = (result * base) % mod;
    base = (base * base) % mod;
    exp >>= 1n;
  }
  return result;
}

export function getDiffieHellman(group: string) {
  if (group !== "modp1") throw new Error(`crypto shim: unsupported group ${group}`);
  const prime = Buffer.from(MODP1, "hex");
  return { getPrime: () => prime };
}

export function createDiffieHellman(prime: Uint8Array, generator: Uint8Array) {
  const p = toBig(prime), g = toBig(generator), size = prime.length;
  let priv = 0n, pub = 0n;
  return {
    generateKeys() {
      priv = toBig(randomBytes(32)) % (p - 2n) + 1n;
      pub = modPow(g, priv, p);
      return toBuf(pub, size);
    },
    getPublicKey: () => toBuf(pub, size),
    computeSecret: (other: Uint8Array) => toBuf(modPow(toBig(other), priv, p), size),
  };
}

export default {
  createDiffieHellman, getDiffieHellman, createHash, createCipheriv, createDecipheriv,
  randomBytes, randomUUID, randomInt, setRandomSource,
};
