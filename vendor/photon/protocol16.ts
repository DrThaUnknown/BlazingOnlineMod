// Photon Protocol16 serialization, typed so values round-trip exactly (byte vs int matters: the
// client unboxes with exact casts). Layout per docs/photon-client-trace.md.

export const T = {
  Null: 0x2a,
  Dictionary: 0x44,
  StringArray: 0x61,
  Byte: 0x62,
  Custom: 0x63,
  Double: 0x64,
  EventData: 0x65,
  Float: 0x66,
  Hashtable: 0x68,
  Integer: 0x69,
  Short: 0x6b,
  Long: 0x6c,
  IntArray: 0x6e,
  Bool: 0x6f,
  OperationResponse: 0x70,
  OperationRequest: 0x71,
  String: 0x73,
  ByteArray: 0x78,
  Array: 0x79,
  ObjectArray: 0x7a,
} as const;

export type PV =
  | { t: "null" }
  | { t: "byte"; v: number }
  | { t: "bool"; v: boolean }
  | { t: "short"; v: number }
  | { t: "int"; v: number }
  | { t: "long"; v: bigint }
  | { t: "float"; v: number }
  | { t: "double"; v: number }
  | { t: "string"; v: string }
  | { t: "bytes"; v: Buffer }
  | { t: "ints"; v: number[] }
  | { t: "strings"; v: string[] }
  | { t: "objects"; v: PV[] }
  /** Typed array 'y'. `elem` is the element type tag; nested descriptors kept in `sub`. */
  | { t: "array"; elem: number; v: PV[]; custom?: number }
  | { t: "hashtable"; v: [PV, PV][] }
  | { t: "dict"; kt: number; vt: number; v: [PV, PV][] }
  | { t: "custom"; code: number; v: Buffer }
  | { t: "event"; code: number; params: ParamTable }
  | { t: "opresp"; code: number; rc: number; debug: PV; params: ParamTable }
  | { t: "opreq"; code: number; params: ParamTable };

export type ParamTable = Map<number, PV>;

export const P = {
  null: (): PV => ({ t: "null" }),
  byte: (v: number): PV => ({ t: "byte", v }),
  bool: (v: boolean): PV => ({ t: "bool", v }),
  int: (v: number): PV => ({ t: "int", v }),
  str: (v: string): PV => ({ t: "string", v }),
  bytes: (v: Buffer): PV => ({ t: "bytes", v }),
  ints: (v: number[]): PV => ({ t: "ints", v }),
  strings: (v: string[]): PV => ({ t: "strings", v }),
  bools: (v: boolean[]): PV => ({ t: "array", elem: T.Bool, v: v.map((b) => ({ t: "bool", v: b }) as PV) }),
  hashtable: (v: [PV, PV][]): PV => ({ t: "hashtable", v }),
};

export class Reader {
  readonly buf: Buffer;
  pos: number;
  constructor(buf: Buffer, pos = 0) {
    this.buf = buf;
    this.pos = pos;
  }
  u8(): number {
    if (this.pos + 1 > this.buf.length) throw new RangeError("protocol16: read past end");
    return this.buf[this.pos++]!;
  }
  i16(): number {
    const v = this.buf.readInt16BE(this.pos);
    this.pos += 2;
    return v;
  }
  i32(): number {
    const v = this.buf.readInt32BE(this.pos);
    this.pos += 4;
    return v;
  }
  take(n: number): Buffer {
    if (n < 0 || this.pos + n > this.buf.length) throw new RangeError("protocol16: read past end");
    const b = this.buf.subarray(this.pos, this.pos + n);
    this.pos += n;
    return Buffer.from(b);
  }
}

export function readTyped(r: Reader): PV {
  return readValue(r, r.u8());
}

export function readValue(r: Reader, tag: number): PV {
  switch (tag) {
    case 0:
    case T.Null:
      return { t: "null" };
    case T.Byte:
      return { t: "byte", v: r.u8() };
    case T.Bool:
      return { t: "bool", v: r.u8() !== 0 };
    case T.Short:
      return { t: "short", v: r.i16() };
    case T.Integer:
      return { t: "int", v: r.i32() };
    case T.Long: {
      const v = r.buf.readBigInt64BE(r.pos);
      r.pos += 8;
      return { t: "long", v };
    }
    case T.Float: {
      const v = r.buf.readFloatBE(r.pos);
      r.pos += 4;
      return { t: "float", v };
    }
    case T.Double: {
      const v = r.buf.readDoubleBE(r.pos);
      r.pos += 8;
      return { t: "double", v };
    }
    case T.String:
      return { t: "string", v: readString(r) };
    case T.ByteArray:
      return { t: "bytes", v: r.take(r.i32()) };
    case T.IntArray: {
      const n = r.i32();
      const v: number[] = [];
      for (let i = 0; i < n; i++) v.push(r.i32());
      return { t: "ints", v };
    }
    case T.StringArray: {
      const n = r.i16();
      const v: string[] = [];
      for (let i = 0; i < n; i++) v.push(readString(r));
      return { t: "strings", v };
    }
    case T.ObjectArray: {
      const n = r.i16();
      const v: PV[] = [];
      for (let i = 0; i < n; i++) v.push(readTyped(r));
      return { t: "objects", v };
    }
    case T.Array:
      return readArray(r);
    case T.Hashtable: {
      const n = r.i16();
      const v: [PV, PV][] = [];
      for (let i = 0; i < n; i++) v.push([readTyped(r), readTyped(r)]);
      return { t: "hashtable", v };
    }
    case T.Dictionary: {
      const kt = r.u8();
      const vt = r.u8();
      return { t: "dict", kt, vt, v: readDictEntries(r, kt, vt) };
    }
    case T.Custom: {
      const code = r.u8();
      return { t: "custom", code, v: r.take(r.i16() & 0xffff) };
    }
    case T.EventData:
      return { t: "event", code: r.u8(), params: readParamTable(r) };
    case T.OperationResponse: {
      const code = r.u8();
      const rc = r.i16();
      const debug = readTyped(r);
      return { t: "opresp", code, rc, debug, params: readParamTable(r) };
    }
    case T.OperationRequest:
      return { t: "opreq", code: r.u8(), params: readParamTable(r) };
    default:
      throw new Error(`protocol16: unsupported type tag 0x${tag.toString(16)} at ${r.pos - 1}`);
  }
}

function readString(r: Reader): string {
  const n = r.i16() & 0xffff;
  return r.take(n).toString("utf8");
}

function readDictEntries(r: Reader, kt: number, vt: number): [PV, PV][] {
  const n = r.i16();
  const v: [PV, PV][] = [];
  for (let i = 0; i < n; i++) {
    const k = kt === 0 || kt === T.Null ? readTyped(r) : readValue(r, kt);
    const val = vt === 0 || vt === T.Null ? readTyped(r) : readValue(r, vt);
    v.push([k, val]);
  }
  return v;
}

function readArray(r: Reader): PV {
  const n = r.i16();
  const elem = r.u8();
  const v: PV[] = [];
  if (elem === T.Custom) {
    const custom = r.u8();
    for (let i = 0; i < n; i++) v.push({ t: "custom", code: custom, v: r.take(r.i16() & 0xffff) });
    return { t: "array", elem, v, custom };
  }
  if (elem === T.Dictionary) {
    const kt = r.u8();
    const vt = r.u8();
    for (let i = 0; i < n; i++) v.push({ t: "dict", kt, vt, v: readDictEntries(r, kt, vt) });
    return { t: "array", elem, v };
  }
  for (let i = 0; i < n; i++) v.push(elem === T.Array ? readArray(r) : readValue(r, elem));
  return { t: "array", elem, v };
}

export function readParamTable(r: Reader): ParamTable {
  const n = r.i16();
  const m: ParamTable = new Map();
  for (let i = 0; i < n; i++) {
    const key = r.u8();
    m.set(key, readTyped(r));
  }
  return m;
}

// ---- writing ----

export class Writer {
  private parts: Buffer[] = [];
  u8(v: number): void {
    this.parts.push(Buffer.from([v & 0xff]));
  }
  i16(v: number): void {
    const b = Buffer.alloc(2);
    b.writeInt16BE(v);
    this.parts.push(b);
  }
  i32(v: number): void {
    const b = Buffer.alloc(4);
    b.writeInt32BE(v | 0);
    this.parts.push(b);
  }
  raw(b: Buffer): void {
    this.parts.push(b);
  }
  done(): Buffer {
    return Buffer.concat(this.parts);
  }
}

export function tagOf(v: PV): number {
  switch (v.t) {
    case "null":
      return T.Null;
    case "byte":
      return T.Byte;
    case "bool":
      return T.Bool;
    case "short":
      return T.Short;
    case "int":
      return T.Integer;
    case "long":
      return T.Long;
    case "float":
      return T.Float;
    case "double":
      return T.Double;
    case "string":
      return T.String;
    case "bytes":
      return T.ByteArray;
    case "ints":
      return T.IntArray;
    case "strings":
      return T.StringArray;
    case "objects":
      return T.ObjectArray;
    case "array":
      return T.Array;
    case "hashtable":
      return T.Hashtable;
    case "dict":
      return T.Dictionary;
    case "custom":
      return T.Custom;
    case "event":
      return T.EventData;
    case "opresp":
      return T.OperationResponse;
    case "opreq":
      return T.OperationRequest;
  }
}

export function writeTyped(w: Writer, v: PV): void {
  w.u8(tagOf(v));
  writeValue(w, v);
}

function writeString(w: Writer, s: string): void {
  const b = Buffer.from(s, "utf8");
  w.i16(b.length);
  w.raw(b);
}

export function writeValue(w: Writer, v: PV): void {
  switch (v.t) {
    case "null":
      return;
    case "byte":
      return w.u8(v.v);
    case "bool":
      return w.u8(v.v ? 1 : 0);
    case "short":
      return w.i16(v.v);
    case "int":
      return w.i32(v.v);
    case "long": {
      const b = Buffer.alloc(8);
      b.writeBigInt64BE(v.v);
      return w.raw(b);
    }
    case "float": {
      const b = Buffer.alloc(4);
      b.writeFloatBE(v.v);
      return w.raw(b);
    }
    case "double": {
      const b = Buffer.alloc(8);
      b.writeDoubleBE(v.v);
      return w.raw(b);
    }
    case "string":
      return writeString(w, v.v);
    case "bytes":
      w.i32(v.v.length);
      return w.raw(v.v);
    case "ints":
      w.i32(v.v.length);
      for (const x of v.v) w.i32(x);
      return;
    case "strings":
      w.i16(v.v.length);
      for (const s of v.v) writeString(w, s);
      return;
    case "objects":
      w.i16(v.v.length);
      for (const x of v.v) writeTyped(w, x);
      return;
    case "array":
      return writeArray(w, v);
    case "hashtable":
      w.i16(v.v.length);
      for (const [k, x] of v.v) {
        writeTyped(w, k);
        writeTyped(w, x);
      }
      return;
    case "dict":
      w.u8(v.kt);
      w.u8(v.vt);
      return writeDictEntries(w, v);
    case "custom":
      w.u8(v.code);
      w.i16(v.v.length);
      return w.raw(v.v);
    case "event":
      w.u8(v.code);
      return writeParamTable(w, v.params);
    case "opresp":
      w.u8(v.code);
      w.i16(v.rc);
      writeTyped(w, v.debug);
      return writeParamTable(w, v.params);
    case "opreq":
      w.u8(v.code);
      return writeParamTable(w, v.params);
  }
}

function writeDictEntries(w: Writer, v: Extract<PV, { t: "dict" }>): void {
  w.i16(v.v.length);
  for (const [k, x] of v.v) {
    if (v.kt === 0 || v.kt === T.Null) writeTyped(w, k);
    else writeValue(w, k);
    if (v.vt === 0 || v.vt === T.Null) writeTyped(w, x);
    else writeValue(w, x);
  }
}

function writeArray(w: Writer, v: Extract<PV, { t: "array" }>): void {
  w.i16(v.v.length);
  w.u8(v.elem);
  if (v.elem === T.Custom) {
    w.u8(v.custom ?? 0);
    for (const x of v.v) {
      const c = x as Extract<PV, { t: "custom" }>;
      w.i16(c.v.length);
      w.raw(c.v);
    }
    return;
  }
  if (v.elem === T.Dictionary) {
    const first = v.v[0] as Extract<PV, { t: "dict" }> | undefined;
    w.u8(first?.kt ?? 0);
    w.u8(first?.vt ?? 0);
    for (const x of v.v) writeDictEntries(w, x as Extract<PV, { t: "dict" }>);
    return;
  }
  for (const x of v.v) {
    if (v.elem === T.Array) writeArray(w, x as Extract<PV, { t: "array" }>);
    else writeValue(w, x);
  }
}

export function writeParamTable(w: Writer, params: ParamTable): void {
  w.i16(params.size);
  for (const [k, v] of params) {
    w.u8(k);
    writeTyped(w, v);
  }
}

// ---- convenience accessors ----

export function asString(v: PV | undefined): string | undefined {
  return v?.t === "string" ? v.v : undefined;
}
export function asNumber(v: PV | undefined): number | undefined {
  if (!v) return undefined;
  if (v.t === "byte" || v.t === "short" || v.t === "int" || v.t === "float" || v.t === "double") return v.v;
  if (v.t === "long") return Number(v.v);
  return undefined;
}
export function asBool(v: PV | undefined): boolean | undefined {
  return v?.t === "bool" ? v.v : undefined;
}
export function asInts(v: PV | undefined): number[] | undefined {
  if (v?.t === "ints") return v.v;
  if (v?.t === "array" || v?.t === "objects") return v.v.map((x) => asNumber(x) ?? 0);
  return undefined;
}
export function asBytes(v: PV | undefined): number[] | undefined {
  if (v?.t === "bytes") return [...v.v];
  return asInts(v);
}
export function asStrings(v: PV | undefined): string[] | undefined {
  if (v?.t === "strings") return v.v;
  if (v?.t === "array" || v?.t === "objects") return v.v.map((x) => asString(x) ?? "");
  return undefined;
}
export function asEntries(v: PV | undefined): [PV, PV][] | undefined {
  return v?.t === "hashtable" || v?.t === "dict" ? v.v : undefined;
}

/** Stable identity for a hashtable key (string "x" and byte 1 are different keys). */
export function keyId(k: PV): string {
  switch (k.t) {
    case "string":
      return `s:${k.v}`;
    case "byte":
      return `b:${k.v}`;
    case "int":
    case "short":
      return `i:${k.v}`;
    default:
      return `${k.t}:${JSON.stringify(k, (_, x) => (typeof x === "bigint" ? x.toString() : x))}`;
  }
}

export function describe(v: PV): unknown {
  switch (v.t) {
    case "null":
      return null;
    case "bytes":
    case "custom":
      return `<${v.t} ${v.v.length}>`;
    case "long":
      return v.v.toString();
    case "objects":
    case "array":
      return v.v.map(describe);
    case "hashtable":
    case "dict":
      return Object.fromEntries(v.v.map(([k, x]) => [keyId(k), describe(x)]));
    case "event":
    case "opreq":
    case "opresp":
      return `<${v.t} ${v.code}>`;
    default:
      return v.v;
  }
}
