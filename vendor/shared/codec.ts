// Wire codec for the iOS 2.9.0 HTTP API. See docs/protocol.md §2 and tools/api_crypto.py.
import { createCipheriv, createDecipheriv } from "node:crypto";

const KEY16 = Buffer.from("78U#5e8!4@481eeU", "utf8");
const IV16 = Buffer.from("@#5U552&91127e)!", "utf8");
const BLOCK = 16;

export function encryptText(plain: string): string {
  const data = Buffer.from(plain, "utf8");
  const padded = Buffer.alloc(Math.ceil(data.length / BLOCK) * BLOCK);
  data.copy(padded);
  const cipher = createCipheriv("aes-128-cbc", KEY16, IV16).setAutoPadding(false);
  return Buffer.concat([cipher.update(padded), cipher.final()]).toString("base64");
}

export function decryptText(body: string): string {
  const raw = Buffer.from(body.trim().replace(/^'+|'+$/g, ""), "base64");
  if (raw.length === 0 || raw.length % BLOCK !== 0) {
    throw new CodecError(`ciphertext length ${raw.length} is not a positive multiple of ${BLOCK}`);
  }
  const decipher = createDecipheriv("aes-128-cbc", KEY16, IV16).setAutoPadding(false);
  const plain = Buffer.concat([decipher.update(raw), decipher.final()]);
  let end = plain.length;
  while (end > 0 && plain[end - 1] === 0) end--;
  return plain.subarray(0, end).toString("utf8");
}

export function encodeBody(value: unknown): string {
  return encryptText(JSON.stringify(value));
}

export function decodeBody<T = unknown>(body: string): T {
  const text = decryptText(body);
  try {
    return JSON.parse(text) as T;
  } catch (err) {
    throw new CodecError(`decrypted body is not JSON: ${(err as Error).message}`);
  }
}

export class CodecError extends Error {
  override name = "CodecError";
}
