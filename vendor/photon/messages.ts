// Photon message framing + DH/AES encryption. docs/photon-client-trace.md.
import crypto from "node:crypto";
import { Reader, Writer, readParamTable, readTyped, writeParamTable, writeTyped, type PV, type ParamTable } from "./protocol16.ts";

export const MSG = {
  Init: 0,
  InitResponse: 1,
  Operation: 2,
  OperationResponse: 3,
  Event: 4,
  InternalOperationRequest: 6,
  InternalOperationResponse: 7,
} as const;

/** PhotonCodes..cctor stores 0x00010201: ClientKey=1, ModeKey=2, ServerKey=1, InitEncryption=0. */
export const PhotonCodes = { ClientKey: 1, ModeKey: 2, ServerKey: 1, InitEncryption: 0 } as const;

const MAGIC = 0xf3;

export type Incoming =
  | { kind: "init" }
  | { kind: "op"; internal: boolean; encrypted: boolean; code: number; params: ParamTable }
  | { kind: "other"; type: number };

/** Parses a reassembled message. `decrypt` is used for messages with the 0x80 flag. */
export function parseMessage(payload: Buffer, decrypt: ((b: Buffer) => Buffer) | null): Incoming {
  if (payload.length < 2 || (payload[0] !== MAGIC && payload[0] !== 0xfd)) throw new Error(`bad message magic 0x${payload[0]?.toString(16)}`);
  const raw = payload[1]!;
  const type = raw & 0x7f;
  const encrypted = (raw & 0x80) !== 0;
  if (type === MSG.Init) return { kind: "init" };
  if (type !== MSG.Operation && type !== MSG.InternalOperationRequest) return { kind: "other", type };
  let body = payload.subarray(2);
  if (encrypted) {
    if (!decrypt) throw new Error("encrypted message before key exchange");
    body = decrypt(body);
  }
  const r = new Reader(body);
  const code = r.u8();
  return { kind: "op", internal: type === MSG.InternalOperationRequest, encrypted, code, params: readParamTable(r) };
}

export function initResponse(): Buffer {
  return Buffer.from([MAGIC, MSG.InitResponse]);
}

export function operationResponse(code: number, rc: number, params: ParamTable = new Map(), internal = false, debug: string | null = null): Buffer {
  const w = new Writer();
  w.u8(MAGIC);
  w.u8(internal ? MSG.InternalOperationResponse : MSG.OperationResponse);
  w.u8(code);
  w.i16(rc);
  writeTyped(w, debug === null ? { t: "null" } : { t: "string", v: debug });
  writeParamTable(w, params);
  return w.done();
}

export function eventMessage(code: number, params: ParamTable): Buffer {
  const w = new Writer();
  w.u8(MAGIC);
  w.u8(MSG.Event);
  w.u8(code);
  writeParamTable(w, params);
  return w.done();
}

export function decodeEventMessage(payload: Buffer): { code: number; params: ParamTable } {
  const r = new Reader(payload, 2);
  return { code: r.u8(), params: readParamTable(r) };
}

export function decodeResponseMessage(payload: Buffer): { code: number; rc: number; debug: PV; params: ParamTable } {
  const r = new Reader(payload, 2);
  const code = r.u8();
  const rc = r.i16();
  const debug = readTyped(r);
  return { code, rc, debug, params: readParamTable(r) };
}

// ---- encryption ----

const OAKLEY_768_GENERATOR = Buffer.from([22]);

export function oakleyPrime768(): Buffer {
  return crypto.getDiffieHellman("modp1").getPrime();
}

/** Photon's BigInteger.GetBytes(): minimal big-endian. */
export function minimalBigEndian(b: Buffer): Buffer {
  let i = 0;
  while (i < b.length - 1 && b[i] === 0) i++;
  return b.subarray(i);
}

export class PhotonCrypto {
  private readonly dh = crypto.createDiffieHellman(oakleyPrime768(), OAKLEY_768_GENERATOR);
  key: Buffer | null = null;

  constructor() {
    this.dh.generateKeys();
  }

  get publicKey(): Buffer {
    return minimalBigEndian(this.dh.getPublicKey());
  }

  derive(otherPublicKey: Buffer): void {
    const shared = minimalBigEndian(this.dh.computeSecret(otherPublicKey));
    this.key = crypto.createHash("sha256").update(shared).digest();
  }

  decrypt(data: Buffer): Buffer {
    if (!this.key) throw new Error("no shared key");
    const d = crypto.createDecipheriv("aes-256-cbc", this.key, Buffer.alloc(16));
    return Buffer.concat([d.update(data), d.final()]);
  }

  encrypt(data: Buffer): Buffer {
    if (!this.key) throw new Error("no shared key");
    const c = crypto.createCipheriv("aes-256-cbc", this.key, Buffer.alloc(16));
    return Buffer.concat([c.update(data), c.final()]);
  }
}
