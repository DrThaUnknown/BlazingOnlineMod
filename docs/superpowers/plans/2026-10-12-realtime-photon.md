# Realtime PvP and Co-op (Photon-compatible server) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a new Node realtime server that speaks enough of Photon's published wire protocol to run Ninja World Ultimate Showdown (real-time ranked PvP) and realtime co-op rooms for the Naruto Blazing private server, as a relay only — never a battle simulator.

**Architecture:** A new `realtime` npm workspace runs a standalone UDP process alongside the existing `server` HTTP process. It shares `@nb/shared` types and reuses `@nb/server`'s `Stores`/session modules by direct import (same monorepo, no new package boundary beyond the workspace itself). It never simulates combat: it decodes/encodes Photon's binary protocol, tracks rooms and connected peers in memory, and relays `RaiseEvent` payloads between the two clients in a room. Match results and rewards flow through the existing HTTP server exactly like Phantom Castle does today — this plan adds the matchmaking queue and room-join endpoints there, not a new combat-authority layer.

**Tech Stack:** Node 24 (native TypeScript, no build step — matches `server`/`shared`), `dgram` (Node's built-in UDP), the existing Fastify HTTP server for matchmaking/result endpoints, Postgres via the existing `Stores` abstraction, Vitest for tests.

**Spec:** `docs/superpowers/specs/2026-10-12-realtime-photon-design.md`

## Global Constraints

- **Relay only.** The realtime server never understands combat; it only moves bytes between clients in a room (spec: Non-goals, Architecture).
- **No battle-result trust changes.** Win/loss and rewards are decided server-side from the client's reported result, exactly like `resultPvp` already does for Phantom Castle (spec: Matchmaking and ranking data model, step 5).
- **No real regions / multi-tier topology.** One process plays name-server, master-server and game-server roles at once (spec: Non-goals).
- **No seasonal tiers yet.** Single points ladder only; Training Time / Qualifier League / Tiers F→S / Five Kage League are explicitly deferred (spec: Non-goals, Out of scope).
- **Validate on PC before phone.** Any protocol-layer task's "done" criterion includes a PC-based check; the real IPA is only touched in the final validation task (spec: De-risking plan).
- **New `rtpvp` state bucket stays separate from Phantom Castle's existing `pvp` bucket** — different ladder, different currency (spec: Matchmaking and ranking data model).
- **Co-op's reward/result HTTP flow (`mission/startmultihost|startmultiguest|resultmultihost|resultmultiguest`, `server/src/game/multi.ts`) does not change.** This plan only adds the live room underneath it (spec: Co-op rooms).

---

## Task 1: Protocol research spike — pin down the exact wire format before writing any transport code

This is a research task, not a coding task, but it produces a concrete, checked-in artifact that every later task cites by name. Per the spec's De-risking plan, we validate against Photon's own published protocol/SDK rather than guessing byte values.

**Files:**
- Create: `docs/photon-protocol.md` (the reference doc every later task cites)
- Create: `realtime/src/protocol/constants.ts` (the byte-level constants, sourced from the doc)
- Test: none (research task — verified by the next task's codec tests exercising these constants)

**Interfaces:**
- Produces: `realtime/src/protocol/constants.ts` exporting at minimum:
  - `ProtocolType16` — an object/enum of Protocol16 type-tag byte values (at minimum: `Null`, `Boolean`, `Byte`, `Short`, `Integer`, `Float`, `Double`, `String`, `Hashtable`, `Array`, `ObjectArray`, `IntegerArray`, `StringArray`)
  - `CommandType` — the ENet-style command type byte values used by the transport (at minimum: `ConnectRequest`, `ConnectResponse`, `Disconnect`, `Ping`, `SendReliable`, `SendUnreliable`, `Acknowledge`)
  - `OperationCode` — the LoadBalancing operation codes actually used: `Authenticate`, `JoinLobby`, `CreateGame`, `JoinGame`, `RaiseEvent`, `Leave`
  - `EventCode` — at minimum `Join`, `Leave`, custom game events pass-through

- [ ] **Step 1: Search for an existing open-source Node/JS Photon client or server implementation**

Search npm and GitHub for terms like "photon-client", "photon-realtime", "enet-js", "photon protocol16". Document in `docs/photon-protocol.md` under a "Prior art" heading: what was found, its license, whether it's a client or server implementation, and whether it is usable as a reference or a dependency. If nothing usable is found, state that explicitly — this is a real, useful finding, not a failure.

- [ ] **Step 2: Document the Protocol16 binary encoding**

In `docs/photon-protocol.md`, under a "Protocol16 encoding" heading, write out: the one-byte type tag followed by type-specific payload for each type in scope (Null, Boolean, Byte, Short, Integer, Float, Double, String, Hashtable, Array, ObjectArray, IntegerArray, StringArray). For each: the exact tag byte value and the exact byte layout that follows (e.g. "String: tag byte, then a 2-byte big-endian length, then UTF-8 bytes"). Source every value from Step 1's findings or Photon's own publicly published protocol documentation — do not invent a byte value without a cited source in the doc.

- [ ] **Step 3: Document the ENet-style transport framing**

Under an "ENet-style transport" heading: the UDP datagram header layout, the command header layout (type byte, channel, flags, length), the connect/verify-connect handshake sequence, how sequence numbers and acknowledgements work, how a reliable command is retransmitted, and the keep-alive ping interval/format. Same sourcing rule as Step 2.

- [ ] **Step 4: Document the LoadBalancing operations in scope**

Under an "Operations in scope" heading, list `Authenticate`, `JoinLobby`, `CreateGame`, `JoinGame`, `RaiseEvent`, `Leave`: each operation's code byte, its request parameter keys (Photon operations key parameters by a single byte, not a string — document each key byte and its Protocol16 type), and its response shape.

- [ ] **Step 5: Write `realtime/src/protocol/constants.ts` from the doc**

```typescript
// realtime/src/protocol/constants.ts
// Values sourced from docs/photon-protocol.md -- do not edit a value here
// without updating its citation in that doc.

export const ProtocolType16 = {
  Null: 0x2a,
  Boolean: 0x6f,
  Byte: 0x62,
  Short: 0x6b,
  Integer: 0x69,
  Float: 0x66,
  Double: 0x64,
  String: 0x73,
  Hashtable: 0x68,
  Array: 0x79,
  ObjectArray: 0x7a,
  IntegerArray: 0x6e,
  StringArray: 0x61,
} as const;

export const CommandType = {
  ConnectRequest: 1,
  ConnectResponse: 2,
  Disconnect: 4,
  Ping: 6,
  SendReliable: 6,
  SendUnreliable: 7,
  Acknowledge: 1,
} as const;

export const OperationCode = {
  Authenticate: 230,
  JoinLobby: 229,
  CreateGame: 227,
  JoinGame: 226,
  RaiseEvent: 253,
  Leave: 254,
} as const;

export const EventCode = {
  Join: 255,
  Leave: 254,
} as const;
```

(Every numeric value above is a placeholder for whatever Steps 1-4 actually found — replace each with the cited value from `docs/photon-protocol.md` before moving on. Do not leave two constants with the same value unless the doc confirms they really share a code.)

- [ ] **Step 6: Commit**

```bash
git add docs/photon-protocol.md realtime/src/protocol/constants.ts
git commit -m "docs: pin down Photon Protocol16/ENet wire format before building the transport"
```

---

## Task 2: `realtime` workspace scaffolding

**Files:**
- Create: `realtime/package.json`
- Create: `realtime/tsconfig.json`
- Create: `realtime/src/main.ts`
- Create: `realtime/test/health.test.ts`
- Modify: `package.json:6-8` (add `"realtime"` to `workspaces`)
- Modify: `docker-compose.yml` (add a `realtime` service)

**Interfaces:**
- Consumes: nothing yet
- Produces: a running process that binds a UDP socket on a configurable port and logs a startup line, importable as `realtime/src/main.ts`'s exported `startRealtimeServer(port: number): { close(): void }`

- [ ] **Step 1: Write `realtime/package.json`**

```json
{
  "name": "@nb/realtime",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "node --watch src/main.ts",
    "start": "node src/main.ts"
  },
  "dependencies": {
    "@nb/shared": "0.1.0",
    "@nb/server": "0.1.0"
  }
}
```

- [ ] **Step 2: Write `realtime/tsconfig.json`**

```json
{
  "extends": "../tsconfig.base.json",
  "include": ["src", "test"]
}
```

- [ ] **Step 3: Write the failing test**

```typescript
// realtime/test/health.test.ts
import { describe, expect, it } from "vitest";
import { startRealtimeServer } from "../src/main.ts";

describe("realtime server startup", () => {
  it("binds its UDP port and can be closed cleanly", async () => {
    const srv = startRealtimeServer(0); // port 0 = OS picks a free port
    expect(srv.port).toBeGreaterThan(0);
    srv.close();
  });
});
```

- [ ] **Step 4: Run test to verify it fails**

Run: `npx vitest run --root realtime`
Expected: FAIL — `realtime/src/main.ts` does not exist yet.

- [ ] **Step 5: Write `realtime/src/main.ts`**

```typescript
// realtime/src/main.ts
import dgram from "node:dgram";

export interface RealtimeServer {
  port: number;
  close(): void;
}

export function startRealtimeServer(port: number): RealtimeServer {
  const socket = dgram.createSocket("udp4");
  socket.bind(port);
  const address = socket.address();
  return {
    port: address.port,
    close: () => socket.close(),
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.env.REALTIME_PORT ?? 5058);
  const srv = startRealtimeServer(port);
  console.log(`realtime server listening on udp:${srv.port}`);
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `npx vitest run --root realtime`
Expected: PASS

- [ ] **Step 7: Add the workspace to the root `package.json`**

In `package.json`, change:
```json
  "workspaces": [
    "shared",
    "server"
  ],
```
to:
```json
  "workspaces": [
    "shared",
    "server",
    "realtime"
  ],
```

- [ ] **Step 8: Add a `docker-compose.yml` service**

Add alongside the existing `server` service:
```yaml
  realtime:
    build:
      context: .
      dockerfile: realtime/Dockerfile
    restart: unless-stopped
    depends_on:
      postgres: { condition: service_healthy }
      redis: { condition: service_healthy }
    environment:
      DATABASE_URL: postgres://${POSTGRES_USER:-nb}:${POSTGRES_PASSWORD}@postgres:5432/${POSTGRES_DB:-nb}
      REDIS_URL: redis://redis:6379
      REALTIME_PORT: 5058
    ports:
      - "${API_BIND:-0.0.0.0}:5058:5058/udp"
      - "${API_BIND:-0.0.0.0}:5055:5055/udp"
```

Create `realtime/Dockerfile`:

```dockerfile
# Build context: repository root (see docker-compose.yml).
FROM node:24-alpine

WORKDIR /app
ENV NODE_ENV=production

COPY package.json package-lock.json ./
COPY shared/package.json shared/
COPY server/package.json server/
COPY realtime/package.json realtime/
RUN npm ci --omit=dev --ignore-scripts && npm cache clean --force

COPY shared/src shared/src
COPY shared/protocol shared/protocol
COPY server/src server/src
COPY realtime/src realtime/src

USER node
EXPOSE 5058/udp
EXPOSE 5055/udp
CMD ["node", "realtime/src/main.ts"]
```

(`server/src` is copied in because `@nb/realtime` imports `@nb/server`'s `Stores`/session modules directly, per the spec's Architecture section — Task 8 is the first task that actually uses this.)

- [ ] **Step 9: Run the whole suite to confirm nothing else broke**

Run: `npx vitest run --root server && npx vitest run --root realtime`
Expected: both PASS

- [ ] **Step 10: Commit**

```bash
git add realtime package.json docker-compose.yml
git commit -m "feat: scaffold the realtime workspace and docker-compose service"
```

---

## Task 3: Protocol16 codec — primitives

**Files:**
- Create: `realtime/src/protocol/codec.ts`
- Test: `realtime/test/codec.test.ts`

**Interfaces:**
- Consumes: `ProtocolType16` from `realtime/src/protocol/constants.ts` (Task 1)
- Produces:
  - `encodeValue(value: boolean | number | string | null): Buffer`
  - `decodeValue(buf: Buffer, offset: number): { value: boolean | number | string | null; nextOffset: number }`

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/codec.test.ts
import { describe, expect, it } from "vitest";
import { decodeValue, encodeValue } from "../src/protocol/codec.ts";

describe("Protocol16 primitive codec", () => {
  it("round-trips a boolean", () => {
    const buf = encodeValue(true);
    const { value, nextOffset } = decodeValue(buf, 0);
    expect(value).toBe(true);
    expect(nextOffset).toBe(buf.length);
  });

  it("round-trips an integer, including negative values", () => {
    for (const n of [0, 1, -1, 2147483647, -2147483648]) {
      const buf = encodeValue(n);
      const { value } = decodeValue(buf, 0);
      expect(value).toBe(n);
    }
  });

  it("round-trips a string, including empty and unicode", () => {
    for (const s of ["", "hello", "ナルト"]) {
      const buf = encodeValue(s);
      const { value } = decodeValue(buf, 0);
      expect(value).toBe(s);
    }
  });

  it("round-trips null", () => {
    const buf = encodeValue(null);
    const { value } = decodeValue(buf, 0);
    expect(value).toBeNull();
  });

  it("decodes starting at a non-zero offset, for values embedded in a larger buffer", () => {
    const prefix = Buffer.from([0xff, 0xff]);
    const encoded = encodeValue(42);
    const combined = Buffer.concat([prefix, encoded]);
    const { value, nextOffset } = decodeValue(combined, prefix.length);
    expect(value).toBe(42);
    expect(nextOffset).toBe(combined.length);
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/codec.test.ts`
Expected: FAIL — `realtime/src/protocol/codec.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/protocol/codec.ts`**

```typescript
// realtime/src/protocol/codec.ts
import { ProtocolType16 } from "./constants.ts";

export function encodeValue(value: boolean | number | string | null): Buffer {
  if (value === null) return Buffer.from([ProtocolType16.Null]);
  if (typeof value === "boolean") return Buffer.from([ProtocolType16.Boolean, value ? 1 : 0]);
  if (typeof value === "string") {
    const body = Buffer.from(value, "utf8");
    const header = Buffer.alloc(3);
    header[0] = ProtocolType16.String;
    header.writeUInt16BE(body.length, 1);
    return Buffer.concat([header, body]);
  }
  if (typeof value === "number") {
    if (Number.isInteger(value) && value >= -2147483648 && value <= 2147483647) {
      const buf = Buffer.alloc(5);
      buf[0] = ProtocolType16.Integer;
      buf.writeInt32BE(value, 1);
      return buf;
    }
    const buf = Buffer.alloc(9);
    buf[0] = ProtocolType16.Double;
    buf.writeDoubleBE(value, 1);
    return buf;
  }
  throw new Error(`encodeValue: unsupported value ${String(value)}`);
}

export function decodeValue(buf: Buffer, offset: number): { value: boolean | number | string | null; nextOffset: number } {
  const tag = buf[offset];
  switch (tag) {
    case ProtocolType16.Null:
      return { value: null, nextOffset: offset + 1 };
    case ProtocolType16.Boolean:
      return { value: buf[offset + 1] !== 0, nextOffset: offset + 2 };
    case ProtocolType16.Integer:
      return { value: buf.readInt32BE(offset + 1), nextOffset: offset + 5 };
    case ProtocolType16.Double:
      return { value: buf.readDoubleBE(offset + 1), nextOffset: offset + 9 };
    case ProtocolType16.String: {
      const len = buf.readUInt16BE(offset + 1);
      const start = offset + 3;
      return { value: buf.toString("utf8", start, start + len), nextOffset: start + len };
    }
    default:
      throw new Error(`decodeValue: unsupported type tag 0x${tag?.toString(16)}`);
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/codec.test.ts`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add realtime/src/protocol/codec.ts realtime/test/codec.test.ts
git commit -m "feat: Protocol16 primitive value codec"
```

---

## Task 4: Protocol16 codec — composite types (Hashtable, Array)

**Files:**
- Modify: `realtime/src/protocol/codec.ts`
- Modify: `realtime/test/codec.test.ts`

**Interfaces:**
- Consumes: `encodeValue`/`decodeValue` from Task 3 (extended to a union type, see below)
- Produces: `encodeValue`/`decodeValue` extended to also accept/return `Map<number | string, unknown>` (Photon's `Hashtable`) and `unknown[]` (Photon's `Array`/`ObjectArray`)

- [ ] **Step 1: Write the failing tests**

```typescript
// append to realtime/test/codec.test.ts
describe("Protocol16 composite codec", () => {
  it("round-trips a Hashtable with mixed byte and string keys", () => {
    const table = new Map<number | string, unknown>([
      [1, "one"],
      ["two", 2],
      [3, true],
    ]);
    const buf = encodeValue(table);
    const { value } = decodeValue(buf, 0) as { value: Map<number | string, unknown> };
    expect(value.get(1)).toBe("one");
    expect(value.get("two")).toBe(2);
    expect(value.get(3)).toBe(true);
  });

  it("round-trips an Array of mixed primitive types", () => {
    const arr = [1, "two", true, null];
    const buf = encodeValue(arr);
    const { value } = decodeValue(buf, 0) as { value: unknown[] };
    expect(value).toEqual(arr);
  });

  it("round-trips a Hashtable nested inside an Array", () => {
    const nested = [new Map<number | string, unknown>([[1, "a"]]), 5];
    const buf = encodeValue(nested);
    const { value } = decodeValue(buf, 0) as { value: unknown[] };
    const first = value[0] as Map<number | string, unknown>;
    expect(first.get(1)).toBe("a");
    expect(value[1]).toBe(5);
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/codec.test.ts`
Expected: FAIL — `encodeValue` does not accept a `Map` or `Array` yet.

- [ ] **Step 3: Extend `realtime/src/protocol/codec.ts`**

```typescript
// realtime/src/protocol/codec.ts -- replace the type union and add composite handling
export type PhotonValue = boolean | number | string | null | PhotonValue[] | Map<number | string, PhotonValue>;

export function encodeValue(value: PhotonValue): Buffer {
  if (value instanceof Map) {
    const parts: Buffer[] = [Buffer.from([ProtocolType16.Hashtable])];
    const countBuf = Buffer.alloc(2);
    countBuf.writeUInt16BE(value.size, 0);
    parts.push(countBuf);
    for (const [k, v] of value) {
      parts.push(encodeValue(k));
      parts.push(encodeValue(v));
    }
    return Buffer.concat(parts);
  }
  if (Array.isArray(value)) {
    const parts: Buffer[] = [Buffer.from([ProtocolType16.ObjectArray])];
    const countBuf = Buffer.alloc(2);
    countBuf.writeUInt16BE(value.length, 0);
    parts.push(countBuf);
    for (const item of value) parts.push(encodeValue(item));
    return Buffer.concat(parts);
  }
  if (value === null) return Buffer.from([ProtocolType16.Null]);
  if (typeof value === "boolean") return Buffer.from([ProtocolType16.Boolean, value ? 1 : 0]);
  if (typeof value === "string") {
    const body = Buffer.from(value, "utf8");
    const header = Buffer.alloc(3);
    header[0] = ProtocolType16.String;
    header.writeUInt16BE(body.length, 1);
    return Buffer.concat([header, body]);
  }
  if (typeof value === "number") {
    if (Number.isInteger(value) && value >= -2147483648 && value <= 2147483647) {
      const buf = Buffer.alloc(5);
      buf[0] = ProtocolType16.Integer;
      buf.writeInt32BE(value, 1);
      return buf;
    }
    const buf = Buffer.alloc(9);
    buf[0] = ProtocolType16.Double;
    buf.writeDoubleBE(value, 1);
    return buf;
  }
  throw new Error(`encodeValue: unsupported value ${String(value)}`);
}

export function decodeValue(buf: Buffer, offset: number): { value: PhotonValue; nextOffset: number } {
  const tag = buf[offset];
  switch (tag) {
    case ProtocolType16.Null:
      return { value: null, nextOffset: offset + 1 };
    case ProtocolType16.Boolean:
      return { value: buf[offset + 1] !== 0, nextOffset: offset + 2 };
    case ProtocolType16.Integer:
      return { value: buf.readInt32BE(offset + 1), nextOffset: offset + 5 };
    case ProtocolType16.Double:
      return { value: buf.readDoubleBE(offset + 1), nextOffset: offset + 9 };
    case ProtocolType16.String: {
      const len = buf.readUInt16BE(offset + 1);
      const start = offset + 3;
      return { value: buf.toString("utf8", start, start + len), nextOffset: start + len };
    }
    case ProtocolType16.Hashtable: {
      const count = buf.readUInt16BE(offset + 1);
      let cursor = offset + 3;
      const table = new Map<number | string, PhotonValue>();
      for (let i = 0; i < count; i++) {
        const key = decodeValue(buf, cursor);
        cursor = key.nextOffset;
        const val = decodeValue(buf, cursor);
        cursor = val.nextOffset;
        table.set(key.value as number | string, val.value);
      }
      return { value: table, nextOffset: cursor };
    }
    case ProtocolType16.ObjectArray: {
      const count = buf.readUInt16BE(offset + 1);
      let cursor = offset + 3;
      const arr: PhotonValue[] = [];
      for (let i = 0; i < count; i++) {
        const item = decodeValue(buf, cursor);
        cursor = item.nextOffset;
        arr.push(item.value);
      }
      return { value: arr, nextOffset: cursor };
    }
    default:
      throw new Error(`decodeValue: unsupported type tag 0x${tag?.toString(16)}`);
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/codec.test.ts`
Expected: PASS (all tests, including Task 3's)

- [ ] **Step 5: Commit**

```bash
git add realtime/src/protocol/codec.ts realtime/test/codec.test.ts
git commit -m "feat: Protocol16 Hashtable and Array codec"
```

---

## Task 5: ENet-style transport — command framing and connect handshake

**Files:**
- Create: `realtime/src/transport/commands.ts`
- Create: `realtime/src/transport/peer.ts`
- Test: `realtime/test/transport.test.ts`

**Interfaces:**
- Consumes: `CommandType` from `realtime/src/protocol/constants.ts` (Task 1)
- Produces:
  - `parseCommand(buf: Buffer, offset: number): { type: number; channel: number; length: number; body: Buffer; nextOffset: number }`
  - `encodeCommand(type: number, channel: number, body: Buffer): Buffer`
  - `class Peer` with `handleConnectRequest(buf: Buffer): Buffer` (returns the connect-response datagram)

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/transport.test.ts
import { describe, expect, it } from "vitest";
import { encodeCommand, parseCommand } from "../src/transport/commands.ts";
import { Peer } from "../src/transport/peer.ts";
import { CommandType } from "../src/protocol/constants.ts";

describe("command framing", () => {
  it("round-trips a command's type, channel and body", () => {
    const body = Buffer.from([1, 2, 3]);
    const encoded = encodeCommand(CommandType.Ping, 0, body);
    const parsed = parseCommand(encoded, 0);
    expect(parsed.type).toBe(CommandType.Ping);
    expect(parsed.channel).toBe(0);
    expect(parsed.body).toEqual(body);
    expect(parsed.nextOffset).toBe(encoded.length);
  });

  it("parses two concatenated commands from one datagram", () => {
    const a = encodeCommand(CommandType.Ping, 0, Buffer.from([1]));
    const b = encodeCommand(CommandType.Disconnect, 0, Buffer.alloc(0));
    const combined = Buffer.concat([a, b]);
    const first = parseCommand(combined, 0);
    expect(first.type).toBe(CommandType.Ping);
    const second = parseCommand(combined, first.nextOffset);
    expect(second.type).toBe(CommandType.Disconnect);
    expect(second.nextOffset).toBe(combined.length);
  });
});

describe("Peer connect handshake", () => {
  it("responds to a connect request with a connect response", () => {
    const peer = new Peer();
    const request = encodeCommand(CommandType.ConnectRequest, 0, Buffer.alloc(0));
    const response = peer.handleConnectRequest(request);
    const parsed = parseCommand(response, 0);
    expect(parsed.type).toBe(CommandType.ConnectResponse);
  });

  it("assigns the peer an id on connect, retrievable afterward", () => {
    const peer = new Peer();
    expect(peer.id).toBeUndefined();
    const request = encodeCommand(CommandType.ConnectRequest, 0, Buffer.alloc(0));
    peer.handleConnectRequest(request);
    expect(peer.id).toBeDefined();
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/transport.test.ts`
Expected: FAIL — neither file exists.

- [ ] **Step 3: Write `realtime/src/transport/commands.ts`**

```typescript
// realtime/src/transport/commands.ts
// Command header layout per docs/photon-protocol.md "ENet-style transport":
// [type: 1 byte][channel: 1 byte][length: 2 bytes BE][body: length bytes]

export function encodeCommand(type: number, channel: number, body: Buffer): Buffer {
  const header = Buffer.alloc(4);
  header[0] = type;
  header[1] = channel;
  header.writeUInt16BE(body.length, 2);
  return Buffer.concat([header, body]);
}

export function parseCommand(buf: Buffer, offset: number) {
  const type = buf[offset]!;
  const channel = buf[offset + 1]!;
  const length = buf.readUInt16BE(offset + 2);
  const bodyStart = offset + 4;
  const body = buf.subarray(bodyStart, bodyStart + length);
  return { type, channel, length, body, nextOffset: bodyStart + length };
}
```

- [ ] **Step 4: Write `realtime/src/transport/peer.ts`**

```typescript
// realtime/src/transport/peer.ts
import { randomUUID } from "node:crypto";
import { CommandType } from "../protocol/constants.ts";
import { encodeCommand, parseCommand } from "./commands.ts";

export class Peer {
  id: string | undefined;

  handleConnectRequest(datagram: Buffer): Buffer {
    const cmd = parseCommand(datagram, 0);
    if (cmd.type !== CommandType.ConnectRequest) {
      throw new Error(`expected ConnectRequest, got command type ${cmd.type}`);
    }
    this.id = randomUUID();
    return encodeCommand(CommandType.ConnectResponse, 0, Buffer.alloc(0));
  }
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/transport.test.ts`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add realtime/src/transport realtime/test/transport.test.ts
git commit -m "feat: ENet-style command framing and connect handshake"
```

---

## Task 6: Reliable delivery — acknowledgement tracking and retransmission

**Pre-flight finding (recorded in the ledger during Task 5's review):** Task 1's protocol research
(`docs/photon-protocol.md`, committed) found the real Photon command header already carries a
4-byte `sequenceNumber` field (fixed in Task 5). This task's original text manually prepended a
second, redundant 4-byte sequence number into the command *body* — that is now wrong: it would
double up the sequence number and shift every reliable command's body by 4 bytes for no reason.
Use `encodeCommand`'s `sequenceNumber` option (Task 5) instead.

**Files:**
- Create: `realtime/src/transport/reliable.ts`
- Test: `realtime/test/reliable.test.ts`

**Interfaces:**
- Consumes: `encodeCommand`/`parseCommand` from Task 5
- Produces: `class ReliableChannel` with:
  - `send(body: Buffer): Buffer` — returns the outgoing datagram, remembers it for retransmission
  - `onAck(sequence: number): void` — stops retransmitting that sequence
  - `pendingRetransmits(now: number): Buffer[]` — datagrams whose ack has not arrived within a timeout, due to be resent

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/reliable.test.ts
import { describe, expect, it } from "vitest";
import { ReliableChannel } from "../src/transport/reliable.ts";

describe("ReliableChannel", () => {
  it("assigns increasing sequence numbers to sent commands", () => {
    const channel = new ReliableChannel();
    const a = channel.send(Buffer.from("a"));
    const b = channel.send(Buffer.from("b"));
    expect(a).not.toEqual(b);
  });

  it("does not retransmit a command once it has been acknowledged", () => {
    const channel = new ReliableChannel();
    channel.send(Buffer.from("a"));
    channel.onAck(0); // the first sent command's sequence number is 0
    const pending = channel.pendingRetransmits(Date.now() + 10_000);
    expect(pending).toHaveLength(0);
  });

  it("retransmits an unacknowledged command after the timeout elapses", () => {
    const channel = new ReliableChannel({ retransmitTimeoutMs: 100 });
    const sentAt = 1_000;
    channel.send(Buffer.from("a"), sentAt);
    expect(channel.pendingRetransmits(sentAt + 50)).toHaveLength(0);
    expect(channel.pendingRetransmits(sentAt + 150)).toHaveLength(1);
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/reliable.test.ts`
Expected: FAIL — `realtime/src/transport/reliable.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/transport/reliable.ts`**

```typescript
// realtime/src/transport/reliable.ts
import { CommandType } from "../protocol/constants.ts";
import { encodeCommand } from "./commands.ts";

interface PendingCommand {
  sequence: number;
  datagram: Buffer;
  sentAt: number;
  acked: boolean;
}

export class ReliableChannel {
  private nextSequence = 0;
  private readonly pending = new Map<number, PendingCommand>();
  private readonly retransmitTimeoutMs: number;

  constructor(opts: { retransmitTimeoutMs?: number } = {}) {
    this.retransmitTimeoutMs = opts.retransmitTimeoutMs ?? 300;
  }

  send(body: Buffer, now = Date.now()): Buffer {
    const sequence = this.nextSequence++;
    const datagram = encodeCommand(CommandType.SendReliable, 0, body, { sequenceNumber: sequence });
    this.pending.set(sequence, { sequence, datagram, sentAt: now, acked: false });
    return datagram;
  }

  onAck(sequence: number): void {
    const entry = this.pending.get(sequence);
    if (entry) entry.acked = true;
  }

  pendingRetransmits(now: number): Buffer[] {
    const due: Buffer[] = [];
    for (const entry of this.pending.values()) {
      if (!entry.acked && now - entry.sentAt >= this.retransmitTimeoutMs) {
        due.push(entry.datagram);
        entry.sentAt = now;
      }
    }
    return due;
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/reliable.test.ts`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add realtime/src/transport/reliable.ts realtime/test/reliable.test.ts
git commit -m "feat: reliable-channel acknowledgement tracking and retransmission"
```

---

## Task 7: Wire the UDP socket to the transport layer

**Pre-flight finding (recorded in the ledger during Task 5's review):** `docs/photon-protocol.md`
documents a separate, OUTER 12-byte UDP datagram header (`peerId`, `flags`, `commandCount`,
`timestamp`, `challenge`) that wraps one-or-more command headers in every real Photon datagram —
Task 5 only fixed the inner *command* header. This task is where that outer header belongs (it's
the layer that actually receives raw socket bytes), so it now includes parsing and emitting it,
not just dispatching one bare command per datagram as originally drafted. Real per-connection
`peerId` allocation (the datagram header's 2-byte numeric id, distinct from `Peer.id`'s UUID
placeholder) is still a later gap — this task uses `peerId: 0` for every response, which is fine
for the single-room, pre-matchmaking testing this task's own tests do, and is called out again in
Task 17's validation notes as something to revisit once a real PC Photon client is in the loop.

**Files:**
- Modify: `realtime/src/main.ts`
- Create: `realtime/src/transport/datagram.ts`
- Create: `realtime/src/transport/server.ts`
- Test: `realtime/test/datagram.test.ts`, `realtime/test/server.test.ts`

**Interfaces:**
- Consumes: `Peer` (Task 5), `ReliableChannel` (Task 6), `parseCommand`/`encodeCommand` (Task 5, now with the real 12-byte header)
- Produces:
  - `parseDatagramHeader(buf: Buffer): { peerId: number; flags: number; commandCount: number; timestamp: number; challenge: number; commandsOffset: number }` and `encodeDatagramHeader(opts: { peerId: number; flags?: number; commandCount: number; timestamp?: number; challenge?: number }): Buffer` from `realtime/src/transport/datagram.ts`
  - `class TransportServer` with `handleDatagram(msg: Buffer, respond: (buf: Buffer) => void): void`. **Correction (found during this task's implementation):** this line originally also promised "a retransmit timer driven by `pendingRetransmits`," but nothing in this task's actual steps wires `ReliableChannel` into `TransportServer` — there is nothing yet that sends a reliable command needing retries, so a timer here would drive an always-empty queue. Deferred to whichever later task first sends a reliable, retriable command; tracked for the final whole-branch review, not fixed here.

- [ ] **Step 1: Write the failing test for the datagram header**

```typescript
// realtime/test/datagram.test.ts
import { describe, expect, it } from "vitest";
import { encodeDatagramHeader, parseDatagramHeader } from "../src/transport/datagram.ts";

describe("UDP datagram header", () => {
  it("round-trips peerId, commandCount, timestamp and challenge", () => {
    const header = encodeDatagramHeader({ peerId: 7, commandCount: 2, timestamp: 12345, challenge: 999 });
    const parsed = parseDatagramHeader(header);
    expect(parsed.peerId).toBe(7);
    expect(parsed.commandCount).toBe(2);
    expect(parsed.timestamp).toBe(12345);
    expect(parsed.challenge).toBe(999);
    expect(parsed.commandsOffset).toBe(12);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run --root realtime realtime/test/datagram.test.ts`
Expected: FAIL — `realtime/src/transport/datagram.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/transport/datagram.ts`**

```typescript
// realtime/src/transport/datagram.ts
// UDP datagram header (12 bytes) per docs/photon-protocol.md "UDP datagram header (12 bytes)":
// [peerId:2 BE][flags:1][commandCount:1][timestamp:4 BE][challenge:4 BE]

const DATAGRAM_HEADER_LENGTH = 12;

export interface ParsedDatagramHeader {
  peerId: number;
  flags: number;
  commandCount: number;
  timestamp: number;
  challenge: number;
  commandsOffset: number;
}

export function parseDatagramHeader(buf: Buffer): ParsedDatagramHeader {
  return {
    peerId: buf.readUInt16BE(0),
    flags: buf[2]!,
    commandCount: buf[3]!,
    timestamp: buf.readInt32BE(4),
    challenge: buf.readInt32BE(8),
    commandsOffset: DATAGRAM_HEADER_LENGTH,
  };
}

export function encodeDatagramHeader(opts: { peerId: number; flags?: number; commandCount: number; timestamp?: number; challenge?: number }): Buffer {
  const header = Buffer.alloc(DATAGRAM_HEADER_LENGTH);
  header.writeUInt16BE(opts.peerId, 0);
  header[2] = opts.flags ?? 0;
  header[3] = opts.commandCount;
  header.writeInt32BE(opts.timestamp ?? 0, 4);
  header.writeInt32BE(opts.challenge ?? 0, 8);
  return header;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run --root realtime realtime/test/datagram.test.ts`
Expected: PASS

- [ ] **Step 5: Write the failing test for `TransportServer`**

```typescript
// realtime/test/server.test.ts
import { describe, expect, it, vi } from "vitest";
import { TransportServer } from "../src/transport/server.ts";
import { CommandType } from "../src/protocol/constants.ts";
import { encodeCommand } from "../src/transport/commands.ts";
import { encodeDatagramHeader } from "../src/transport/datagram.ts";

function datagram(...commands: Buffer[]): Buffer {
  return Buffer.concat([encodeDatagramHeader({ peerId: 0, commandCount: commands.length }), ...commands]);
}

describe("TransportServer", () => {
  it("responds to a connect request from a new remote address", () => {
    const server = new TransportServer();
    const respond = vi.fn();
    const request = datagram(encodeCommand(CommandType.ConnectRequest, 0, Buffer.alloc(0)));
    server.handleDatagram(request, "1.2.3.4:5000", respond);
    expect(respond).toHaveBeenCalledOnce();
  });

  it("tracks one Peer per remote address across multiple datagrams", () => {
    const server = new TransportServer();
    const respond = vi.fn();
    const request = datagram(encodeCommand(CommandType.ConnectRequest, 0, Buffer.alloc(0)));
    server.handleDatagram(request, "1.2.3.4:5000", respond);
    const peerId = server.peerFor("1.2.3.4:5000")?.id;
    server.handleDatagram(request, "1.2.3.4:5000", respond);
    expect(server.peerFor("1.2.3.4:5000")?.id).toBe(peerId);
  });

  it("dispatches every command packed into one datagram, not just the first", () => {
    const server = new TransportServer();
    const respond = vi.fn();
    const request = datagram(
      encodeCommand(CommandType.ConnectRequest, 0, Buffer.alloc(0)),
      encodeCommand(CommandType.Ping, 0, Buffer.alloc(0)),
    );
    server.handleDatagram(request, "1.2.3.4:5000", respond);
    expect(respond).toHaveBeenCalledOnce(); // only ConnectRequest produces a reply today; Ping is a no-op until a later task
  });
});
```

- [ ] **Step 6: Run test to verify it fails**

Run: `npx vitest run --root realtime realtime/test/server.test.ts`
Expected: FAIL — `realtime/src/transport/server.ts` does not exist.

- [ ] **Step 7: Write `realtime/src/transport/server.ts`**

```typescript
// realtime/src/transport/server.ts
import { Peer } from "./peer.ts";
import { parseCommand } from "./commands.ts";
import { parseDatagramHeader, encodeDatagramHeader } from "./datagram.ts";
import { CommandType } from "../protocol/constants.ts";

export class TransportServer {
  private readonly peers = new Map<string, Peer>();

  peerFor(remoteKey: string): Peer | undefined {
    return this.peers.get(remoteKey);
  }

  handleDatagram(msg: Buffer, remoteKey: string, respond: (buf: Buffer) => void): void {
    let peer = this.peers.get(remoteKey);
    if (!peer) {
      peer = new Peer();
      this.peers.set(remoteKey, peer);
    }

    const { commandCount, commandsOffset } = parseDatagramHeader(msg);
    const replies: Buffer[] = [];
    let offset = commandsOffset;
    for (let i = 0; i < commandCount; i++) {
      const cmd = parseCommand(msg, offset);
      offset = cmd.nextOffset;
      if (cmd.type === CommandType.ConnectRequest) {
        replies.push(peer.handleConnectRequest(msg.subarray(offset - (cmd.nextOffset - offset === 0 ? 0 : 0))));
      }
      // Further command types (reliable sends, pings, operation requests) are
      // dispatched here by later tasks (Task 8 onward) once they exist.
    }
    if (replies.length) {
      // peerId 0 is a placeholder: real per-connection peerId allocation is a later gap (see
      // this task's pre-flight note above).
      respond(Buffer.concat([encodeDatagramHeader({ peerId: 0, commandCount: replies.length }), ...replies]));
    }
  }
}
```

The expression `msg.subarray(offset - (cmd.nextOffset - offset === 0 ? 0 : 0))` above is deliberately broken filler — fix it before running the tests: `peer.handleConnectRequest` (Task 5) expects to parse its OWN command starting at offset 0 of whatever buffer it's given, but here each command starts at a variable `offset` within `msg`, not at 0. Pass `peer.handleConnectRequest(msg.subarray(offset - (offset - commandsOffset)))` is also wrong for the same reason — the correct fix is to re-slice from THIS command's own start, which you captured before advancing: track `const commandStart = offset;` at the top of the loop body (before calling `parseCommand`), then pass `peer.handleConnectRequest(msg.subarray(commandStart))` (safe even though it includes trailing bytes from later commands, since `parseCommand`/`handleConnectRequest` only reads the one header+body it needs from the start of what it's given).

- [ ] **Step 8: Run test to verify it passes**

Run: `npx vitest run --root realtime realtime/test/server.test.ts`
Expected: PASS

- [ ] **Step 9: Wire it into `realtime/src/main.ts`**

`realtime/src/main.ts` currently (after Task 2's fix round) looks like this — modify it in place, do not reintroduce Task 2's already-fixed duplicate-CLI-block bug:

```typescript
// realtime/src/main.ts
import dgram from "node:dgram";
import { TransportServer } from "./transport/server.ts";

export interface RealtimeServer {
  port: number;
  close(): void;
}

export function startRealtimeServer(port: number): Promise<RealtimeServer> {
  return new Promise((resolve) => {
    const transport = new TransportServer();
    const socket = dgram.createSocket("udp4");
    socket.on("message", (msg, rinfo) => {
      const remoteKey = `${rinfo.address}:${rinfo.port}`;
      transport.handleDatagram(msg, remoteKey, (buf) => socket.send(buf, rinfo.port, rinfo.address));
    });
    socket.once("listening", () => {
      const address = socket.address();
      resolve({ port: address.port, close: () => socket.close() });
    });
    socket.bind(port);
  });
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.env.REALTIME_PORT ?? 5058);
  startRealtimeServer(port).then((srv) => {
    console.log(`realtime server listening on udp:${srv.port}`);
  });
}
```

- [ ] **Step 10: Run the full realtime suite**

Run: `npx vitest run --root realtime`
Expected: all PASS

- [ ] **Step 11: Commit**

```bash
git add realtime/src/main.ts realtime/src/transport/datagram.ts realtime/src/transport/server.ts realtime/test/datagram.test.ts realtime/test/server.test.ts
git commit -m "feat: wire the UDP socket to the transport layer, including the outer datagram header"
```

---

## Task 8: Authenticate operation — tie a Photon peer to an existing session

**Files:**
- Create: `realtime/src/operations/authenticate.ts`
- Modify: `realtime/src/transport/peer.ts` (add an optional `playerId` field, set once authenticated)
- Modify: `realtime/src/transport/server.ts` (dispatch operation requests to `authenticate`)
- Test: `realtime/test/authenticate.test.ts`

**Interfaces:**
- Consumes: `OperationCode` (Task 1), `decodeValue`/`encodeValue` (Tasks 3-4), `@nb/server`'s session store (`import { memoryStores } from "@nb/server/src/store/memory.ts"` pattern, or whatever concrete store the running process uses — this task takes a `Stores`-shaped dependency, not a concrete store, so it is testable without Postgres)
- Produces: `authenticate(ssid: string, stores: { sessions: { touch(ssid: string): Promise<number | null> } }): Promise<{ playerId: number } | null>`

- [ ] **Step 1: Write the failing test**

```typescript
// realtime/test/authenticate.test.ts
import { describe, expect, it } from "vitest";
import { authenticate } from "../src/operations/authenticate.ts";

describe("authenticate", () => {
  it("resolves a player id for a valid session", async () => {
    const stores = { sessions: { touch: async (ssid: string) => (ssid === "good" ? 42 : null) } };
    const result = await authenticate("good", stores);
    expect(result).toEqual({ playerId: 42 });
  });

  it("returns null for an invalid or expired session", async () => {
    const stores = { sessions: { touch: async () => null } };
    const result = await authenticate("bad", stores);
    expect(result).toBeNull();
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run --root realtime realtime/test/authenticate.test.ts`
Expected: FAIL — `realtime/src/operations/authenticate.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/operations/authenticate.ts`**

```typescript
// realtime/src/operations/authenticate.ts
export interface SessionLookup {
  sessions: { touch(ssid: string): Promise<number | null> };
}

export async function authenticate(ssid: string, stores: SessionLookup): Promise<{ playerId: number } | null> {
  const playerId = await stores.sessions.touch(ssid);
  if (playerId === null) return null;
  return { playerId };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run --root realtime realtime/test/authenticate.test.ts`
Expected: PASS

- [ ] **Step 5: Add `playerId` to `Peer` and dispatch `Authenticate` in `TransportServer`**

In `realtime/src/transport/peer.ts`, add:
```typescript
  playerId: number | undefined;
```

In `realtime/src/transport/server.ts`, `TransportServer`'s constructor now takes a `SessionLookup` and, on an `Authenticate` operation request (decoded via `decodeValue` into a Hashtable with the ssid at whatever key byte Task 1's doc records), calls `authenticate` and sets `peer.playerId` on success. This step's exact decode-and-dispatch code depends on Task 1's documented parameter-key byte for `Authenticate`'s ssid parameter — write it using that cited key, not a guess.

- [ ] **Step 6: Run the full realtime suite**

Run: `npx vitest run --root realtime`
Expected: all PASS

- [ ] **Step 7: Commit**

```bash
git add realtime/src/operations/authenticate.ts realtime/src/transport/peer.ts realtime/src/transport/server.ts realtime/test/authenticate.test.ts
git commit -m "feat: Authenticate operation ties a Photon peer to an existing session"
```

---

## Task 9: Room registry — CreateGame and JoinGame

**Files:**
- Create: `realtime/src/rooms/registry.ts`
- Test: `realtime/test/rooms.test.ts`

**Interfaces:**
- Consumes: nothing new
- Produces:
  - `class RoomRegistry` with `createRoom(roomId: string, creatorPlayerId: number, kind?: "pvp" | "coop"): Room`, `joinRoom(roomId: string, playerId: number): Room`, `getRoom(roomId: string): Room | undefined`
  - `interface Room { id: string; playerIds: number[]; kind: "pvp" | "coop" }` — `kind` distinguishes a PvP match (auto-forfeit on disconnect, Task 15) from a co-op room (no forfeit, Task 15); defaults to `"pvp"` since this task's own tests never need to care, but the field is real from this task onward, not deferred.

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/rooms.test.ts
import { describe, expect, it } from "vitest";
import { RoomRegistry } from "../src/rooms/registry.ts";

describe("RoomRegistry", () => {
  it("creates a room with its creator as the first player", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    expect(room.playerIds).toEqual([1]);
  });

  it("refuses to create a room id that already exists", () => {
    const registry = new RoomRegistry();
    registry.createRoom("room1", 1);
    expect(() => registry.createRoom("room1", 2)).toThrow();
  });

  it("joins an existing room, adding the joiner to its player list", () => {
    const registry = new RoomRegistry();
    registry.createRoom("room1", 1);
    const room = registry.joinRoom("room1", 2);
    expect(room.playerIds).toEqual([1, 2]);
  });

  it("refuses to join a room that does not exist", () => {
    const registry = new RoomRegistry();
    expect(() => registry.joinRoom("nope", 1)).toThrow();
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/rooms.test.ts`
Expected: FAIL — `realtime/src/rooms/registry.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/rooms/registry.ts`**

```typescript
// realtime/src/rooms/registry.ts
export interface Room {
  id: string;
  playerIds: number[];
  kind: "pvp" | "coop";
}

export class RoomRegistry {
  private readonly rooms = new Map<string, Room>();

  createRoom(roomId: string, creatorPlayerId: number, kind: "pvp" | "coop" = "pvp"): Room {
    if (this.rooms.has(roomId)) throw new Error(`room ${roomId} already exists`);
    const room: Room = { id: roomId, playerIds: [creatorPlayerId], kind };
    this.rooms.set(roomId, room);
    return room;
  }

  joinRoom(roomId: string, playerId: number): Room {
    const room = this.rooms.get(roomId);
    if (!room) throw new Error(`room ${roomId} does not exist`);
    room.playerIds.push(playerId);
    return room;
  }

  getRoom(roomId: string): Room | undefined {
    return this.rooms.get(roomId);
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/rooms.test.ts`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add realtime/src/rooms/registry.ts realtime/test/rooms.test.ts
git commit -m "feat: room registry for CreateGame/JoinGame"
```

---

## Task 10: Event relay — RaiseEvent and Leave

**Files:**
- Create: `realtime/src/rooms/relay.ts`
- Modify: `realtime/src/rooms/registry.ts` (add `deleteRoom`, used by `leave` below)
- Test: `realtime/test/relay.test.ts`

**Interfaces:**
- Consumes: `Room`/`RoomRegistry` (Task 9)
- Produces: `class EventRelay` with `raiseEvent(room: Room, senderPlayerId: number, payload: Buffer, sendTo: (playerId: number, payload: Buffer) => void): void` (forwards to every other player in the room) and `leave(registry: RoomRegistry, room: Room, playerId: number): void` (removes the player from the room, deletes the room if empty)

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/relay.test.ts
import { describe, expect, it, vi } from "vitest";
import { EventRelay } from "../src/rooms/relay.ts";
import { RoomRegistry } from "../src/rooms/registry.ts";

describe("EventRelay", () => {
  it("forwards a raised event to every other player in the room, not the sender", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    registry.joinRoom("room1", 2);
    registry.joinRoom("room1", 3);
    const relay = new EventRelay();
    const sendTo = vi.fn();
    const payload = Buffer.from("hit");
    relay.raiseEvent(room, 1, payload, sendTo);
    expect(sendTo).toHaveBeenCalledWith(2, payload);
    expect(sendTo).toHaveBeenCalledWith(3, payload);
    expect(sendTo).not.toHaveBeenCalledWith(1, payload);
  });

  it("removes a leaving player from the room", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    registry.joinRoom("room1", 2);
    const relay = new EventRelay();
    relay.leave(registry, room, 2);
    expect(registry.getRoom("room1")!.playerIds).toEqual([1]);
  });

  it("deletes the room once its last player leaves", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    const relay = new EventRelay();
    relay.leave(registry, room, 1);
    expect(registry.getRoom("room1")).toBeUndefined();
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/relay.test.ts`
Expected: FAIL — `realtime/src/rooms/relay.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/rooms/relay.ts`**

```typescript
// realtime/src/rooms/relay.ts
import type { Room, RoomRegistry } from "./registry.ts";

export class EventRelay {
  raiseEvent(room: Room, senderPlayerId: number, payload: Buffer, sendTo: (playerId: number, payload: Buffer) => void): void {
    for (const playerId of room.playerIds) {
      if (playerId !== senderPlayerId) sendTo(playerId, payload);
    }
  }

  leave(registry: RoomRegistry, room: Room, playerId: number): void {
    room.playerIds = room.playerIds.filter((id) => id !== playerId);
    if (room.playerIds.length === 0) {
      (registry as unknown as { rooms: Map<string, Room> })["rooms" as never];
      // RoomRegistry has no public delete; add one rather than reach into internals.
    }
  }
}
```

Before finishing this step, add a public `deleteRoom(roomId: string): void` to `RoomRegistry` (Task 9's file) and call `registry.deleteRoom(room.id)` from `leave` once the room is empty, instead of the internals-reaching line above (which is intentionally wrong — it exists only to make clear a public method is needed, not to ship). The corrected `leave`:

```typescript
  leave(registry: RoomRegistry, room: Room, playerId: number): void {
    room.playerIds = room.playerIds.filter((id) => id !== playerId);
    if (room.playerIds.length === 0) registry.deleteRoom(room.id);
  }
```

And in `realtime/src/rooms/registry.ts`, add:
```typescript
  deleteRoom(roomId: string): void {
    this.rooms.delete(roomId);
  }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/relay.test.ts realtime/test/rooms.test.ts`
Expected: all PASS

- [ ] **Step 5: Commit**

```bash
git add realtime/src/rooms/relay.ts realtime/src/rooms/registry.ts realtime/test/relay.test.ts
git commit -m "feat: RaiseEvent relay and room Leave handling"
```

---

## Task 10.5: Wire CreateGame, JoinGame, RaiseEvent and Leave into the operation dispatcher

**Pre-flight finding (recorded in the ledger during Task 10's review):** `RoomRegistry` (Task 9)
and `EventRelay` (Task 10) exist as pure, well-tested data structures, but nothing in the entire
plan ever calls them from `TransportServer`'s network dispatch loop — only `Authenticate` (Task 8)
has real wire-level dispatch. Task 17's validation explicitly exercises `CreateGame`, `JoinGame`,
`RaiseEvent`, and `Leave` against a real Photon client; without this task, all four fail at the
first attempt. This task closes that gap, reusing Task 8's `encodeOperationResponse` and Task 9/10's
room/relay logic — no new data structures, just wiring.

**The one genuinely new piece:** `RaiseEvent` must deliver bytes to OTHER connected peers, not just
reply to whoever sent the request. `TransportServer.handleDatagram`'s `respond` callback is scoped
to the current sender's address only. This task adds a second, separately-injected sender —
`setSender(send: (remoteKey: string, buf: Buffer) => void): void`, called once from `main.ts` — and
a `playerId -> remoteKey` reverse index populated whenever a peer's `playerId` becomes known, so an
event raised by one peer can be posted to every other peer actually in the room.

**Files:**
- Modify: `realtime/src/transport/server.ts` (dispatch `CreateGame`/`JoinGame`/`RaiseEvent`/`Leave`; add `setSender`, the reverse index, a `RoomRegistry` dependency)
- Modify: `realtime/src/main.ts` (construct one `RoomRegistry` shared with the room API — reuse the instance Task 12 will add there, see note below — and call `setSender`)
- Create: `realtime/src/operations/event.ts` (`encodeEventData`)
- Test: `realtime/test/roomOperations.test.ts`

**Note on `main.ts` ordering:** this task is dispatched before Task 12 (which is the task that
actually adds a `RoomRegistry` instance and the internal room API to `main.ts`). If `main.ts`
doesn't yet construct a `RoomRegistry` when this task runs, construct one here (`new
RoomRegistry()`) and pass it into `TransportServer`'s constructor; Task 12 will then reuse that
same instance for its internal API rather than creating a second, disconnected one — check Task
12's own text for this when it's dispatched, and update it if needed.

**Interfaces:**
- Consumes: `RoomRegistry`/`Room` (Task 9), `EventRelay` (Task 10), `encodeOperationResponse` (Task 8), `decodeValue`/`PhotonValue` (Tasks 3-4), `OperationCode`/`ParameterCode` (Task 1)
- Produces:
  - `encodeEventData(code: number, data: PhotonValue): Buffer` from `realtime/src/operations/event.ts` — Protocol16 tag `0x65` per `docs/photon-protocol.md`'s "Protocol16 encoding" table: `[0x65][eventCode:1][paramCount:2 BE = 1][entry: key=245 (ParameterCode.Data), tagged value]`. (Real Photon events can carry more parameters; this project only needs to relay one custom payload per event, so only `Data` is encoded — not `ActorNr`/other bookkeeping keys, which no client-side code in this project reads yet.)
  - `TransportServer.setSender(send: (remoteKey: string, buf: Buffer) => void): void`
  - Actor numbers (needed for `JoinGame`'s `vals[ActorList]`) are `room.playerIds.indexOf(playerId) + 1` — a 1-based position in join order, matching Photon's own "first to join is actor 1" convention. No separate actor-number table is introduced.

- [ ] **Step 1: Write the failing tests**

```typescript
// realtime/test/roomOperations.test.ts
import { describe, expect, it, vi } from "vitest";
import { TransportServer } from "../src/transport/server.ts";
import { RoomRegistry } from "../src/rooms/registry.ts";
import { CommandType, MessageType, OperationCode, ParameterCode, ProtocolType16 } from "../src/protocol/constants.ts";
import { encodeCommand } from "../src/transport/commands.ts";
import { encodeDatagramHeader } from "../src/transport/datagram.ts";
import { encodeValue } from "../src/protocol/codec.ts";

function operationRequest(code: number, params: Map<number, unknown> = new Map()): Buffer {
  const parts: Buffer[] = [Buffer.from([ProtocolType16.OperationRequest, code])];
  const countBuf = Buffer.alloc(2);
  countBuf.writeUInt16BE(params.size, 0);
  parts.push(countBuf);
  for (const [k, v] of params) {
    parts.push(Buffer.from([k]));
    parts.push(encodeValue(v as never));
  }
  return Buffer.concat(parts);
}

function sendReliableCommand(opBody: Buffer): Buffer {
  const body = Buffer.concat([Buffer.from([0, MessageType.OperationRequest]), opBody]);
  return encodeCommand(CommandType.SendReliable, 0, body);
}

function datagram(...commands: Buffer[]): Buffer {
  return Buffer.concat([encodeDatagramHeader({ peerId: 0, commandCount: commands.length }), ...commands]);
}

/** Authenticates a fresh server/peer pair to a given playerId, bypassing real session lookup. */
async function authedServer(playerId: number, remoteKey: string) {
  const server = new TransportServer({ sessions: { touch: async () => playerId } });
  const respond = vi.fn();
  const authReq = operationRequest(OperationCode.Authenticate, new Map([[ParameterCode.ClientAuthenticationParams, "any"]]));
  await server.handleDatagram(datagram(sendReliableCommand(authReq)), remoteKey, respond);
  return { server, respond };
}

describe("room operation dispatch", () => {
  it("CreateGame creates a room owned by the requesting player", async () => {
    const { server, respond } = await authedServer(1, "1.1.1.1:1");
    const req = operationRequest(OperationCode.CreateGame, new Map([[ParameterCode.RoomName, "room1"]]));
    await server.handleDatagram(datagram(sendReliableCommand(req)), "1.1.1.1:1", respond);
    expect(respond).toHaveBeenCalled();
    // (decoding the response fully is covered by the JoinGame test below, which checks vals)
  });

  it("JoinGame returns the actor list of players already in the room", async () => {
    const { server } = await authedServer(1, "1.1.1.1:1");
    await server.handleDatagram(
      datagram(sendReliableCommand(operationRequest(OperationCode.CreateGame, new Map([[ParameterCode.RoomName, "room1"]])))),
      "1.1.1.1:1",
      vi.fn(),
    );
    const { server: server2, respond: respond2 } = await authedServer(2, "2.2.2.2:2");
    // Both peers must share the SAME registry: construct server2 sharing server's registry.
    // (See Step 3's actual constructor shape -- this test's real version passes a shared
    // RoomRegistry instance into both TransportServer instances, not two independent ones.)
    const joinReq = operationRequest(OperationCode.JoinGame, new Map([[ParameterCode.RoomName, "room1"]]));
    await server2.handleDatagram(datagram(sendReliableCommand(joinReq)), "2.2.2.2:2", respond2);
    expect(respond2).toHaveBeenCalled();
  });

  it("RaiseEvent delivers the event to the other player in the room, not back to the sender", async () => {
    // Full test constructs two TransportServer instances sharing one RoomRegistry, has both join
    // the same room, wires setSender to a spy, raises an event from player 1, and asserts the
    // spy was called with player 2's remoteKey and an EventData-tagged payload -- not player 1's.
  });

  it("Leave removes the player from their room", async () => {
    // Full test: create+join a room, send Leave, confirm registry.getRoom(...) no longer lists
    // that player (and the room is deleted if that was the last player, per EventRelay.leave).
  });
});
```

The three abbreviated tests above (`RaiseEvent`, `Leave`, and the cross-server sharing in `JoinGame`)
are deliberately left as descriptions, not full code — they depend on the exact shared-`RoomRegistry`
constructor shape you design in Step 3, which isn't fixed yet. Write them for real, asserting real
observed behavior (an actual spy call with the actual other player's remoteKey for `RaiseEvent`; an
actual `registry.getRoom(...)` check for `Leave`), not placeholders — the two given in full above are
your pattern to extend.

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root realtime realtime/test/roomOperations.test.ts`
Expected: FAIL — the new dispatch branches don't exist yet.

- [ ] **Step 3: Write `realtime/src/operations/event.ts`**

```typescript
// realtime/src/operations/event.ts
import { ProtocolType16, ParameterCode } from "../protocol/constants.ts";
import { encodeValue, type PhotonValue } from "../protocol/codec.ts";

/**
 * Encodes a Protocol16 EventData (tag 0x65) per docs/photon-protocol.md "Protocol16 encoding"
 * table: [0x65][eventCode:1][paramCount:2 BE][entries]. This project only ever relays one
 * payload per event (ParameterCode.Data, key 245) -- real Photon events can carry more, but
 * nothing here produces or consumes them yet.
 */
export function encodeEventData(code: number, data: PhotonValue): Buffer {
  const header = Buffer.from([ProtocolType16.EventData, code]);
  const countBuf = Buffer.from([0, 1]); // paramCount = 1, big-endian uint16
  const entry = Buffer.concat([Buffer.from([ParameterCode.Data]), encodeValue(data)]);
  return Buffer.concat([header, countBuf, entry]);
}
```

- [ ] **Step 4: Extend `realtime/src/transport/server.ts`**

Add a `RoomRegistry` constructor dependency, a `setSender` method, and a reverse `playerId ->
remoteKey` index, then extend the `if (code === OperationCode.Authenticate) { ... }` chain with
`else if` branches for the other three operations:

```typescript
// realtime/src/transport/server.ts -- additions (merge into the existing file, don't replace it)
import { RoomRegistry } from "../rooms/registry.ts";
import { EventRelay } from "../rooms/relay.ts";
import { encodeEventData } from "../operations/event.ts";

export class TransportServer {
  // ... existing `peers`, `sessions` fields ...
  private readonly registry: RoomRegistry;
  private readonly relay = new EventRelay();
  private readonly remoteKeyByPlayerId = new Map<number, string>();
  private send: ((remoteKey: string, buf: Buffer) => void) | undefined;

  constructor(sessions: SessionLookup = UNAUTHENTICATED_STORE, registry: RoomRegistry = new RoomRegistry()) {
    this.sessions = sessions;
    this.registry = registry;
  }

  setSender(send: (remoteKey: string, buf: Buffer) => void): void {
    this.send = send;
  }

  // helper, called once a peer's playerId is known (after a successful Authenticate):
  private trackPlayer(playerId: number, remoteKey: string): void {
    this.remoteKeyByPlayerId.set(playerId, remoteKey);
  }

  private wrapReply(messageType: number, body: Buffer): Buffer {
    return encodeCommand(CommandType.SendReliable, 0, Buffer.concat([Buffer.from([0, messageType]), body]));
  }

  private findRoomForPlayer(playerId: number) {
    // RoomRegistry (Task 9) has no "find by player" method -- add one rather than scan from
    // outside. Add `findRoomForPlayer(playerId: number): Room | undefined` to RoomRegistry
    // itself (scanning its internal map), and call that here instead of re-implementing the
    // scan in this file.
  }
}
```

Fill in `findRoomForPlayer`'s body by adding the described method to `RoomRegistry`
(`realtime/src/rooms/registry.ts`) — a straightforward scan over its internal `rooms` Map
returning the first room whose `playerIds` includes the given id, `undefined` if none. Add a test
for it in `realtime/test/rooms.test.ts` alongside Task 9's existing tests.

Inside `handleDatagram`'s existing `if (result) peer.playerId = result.playerId;` line (the
Authenticate success path), also call `this.trackPlayer(result.playerId, remoteKey)`.

Then add the three new `else if` branches alongside the existing `if (code ===
OperationCode.Authenticate)`:

```typescript
} else if (code === OperationCode.CreateGame || code === OperationCode.JoinGame) {
  const roomName = params.get(ParameterCode.RoomName);
  const responseCode = code; // echo back whichever of the two operations was requested
  if (typeof roomName === "string" && peer.playerId !== undefined) {
    try {
      const room = code === OperationCode.CreateGame
        ? this.registry.createRoom(roomName, peer.playerId)
        : this.registry.joinRoom(roomName, peer.playerId);
      const responseParams = new Map<number, PhotonValue>([[ParameterCode.ActorList, room.playerIds]]);
      replies.push(this.wrapReply(MessageType.OperationResponse, encodeOperationResponse(responseCode, 0, responseParams)));
    } catch {
      replies.push(this.wrapReply(MessageType.OperationResponse, encodeOperationResponse(responseCode, 1)));
    }
  } else {
    replies.push(this.wrapReply(MessageType.OperationResponse, encodeOperationResponse(responseCode, 1)));
  }
} else if (code === OperationCode.RaiseEvent) {
  const eventCode = params.get(ParameterCode.Code);
  const data = params.get(ParameterCode.Data) ?? null;
  const room = peer.playerId !== undefined ? this.registry.findRoomForPlayer(peer.playerId) : undefined;
  if (room && typeof eventCode === "number" && peer.playerId !== undefined && this.send) {
    const eventBody = this.wrapReply(MessageType.Event, encodeEventData(eventCode, data));
    const fullEventDatagram = Buffer.concat([encodeDatagramHeader({ peerId: 0, commandCount: 1 }), eventBody]);
    this.relay.raiseEvent(room, peer.playerId, fullEventDatagram, (targetPlayerId, payload) => {
      const targetKey = this.remoteKeyByPlayerId.get(targetPlayerId);
      if (targetKey) this.send!(targetKey, payload);
    });
  }
  replies.push(this.wrapReply(MessageType.OperationResponse, encodeOperationResponse(OperationCode.RaiseEvent, 0)));
} else if (code === OperationCode.Leave) {
  const room = peer.playerId !== undefined ? this.registry.findRoomForPlayer(peer.playerId) : undefined;
  if (room && peer.playerId !== undefined) this.relay.leave(this.registry, room, peer.playerId);
  replies.push(this.wrapReply(MessageType.OperationResponse, encodeOperationResponse(OperationCode.Leave, 0)));
}
```

`MessageType.Event` (value `4`, per `docs/photon-protocol.md`'s "Inner message type" table) needs
adding to `constants.ts`'s `MessageType` export if it isn't already there — check before assuming.

Also replace the earlier, now-duplicated inline `wrapReply`-shaped code for `Authenticate`'s own
response (the `Buffer.concat([Buffer.from([0, MessageType.OperationResponse]), ...])` line already
in the file) with a call to the new `this.wrapReply(...)` helper, so there's one reply-wrapping
implementation, not two.

- [ ] **Step 5: Fill in the two abbreviated tests from Step 1 for real**

Write real `RaiseEvent` and `Leave` tests per Step 1's description, using two `TransportServer`
instances constructed with the SAME `RoomRegistry` instance (pass one `new RoomRegistry()` into
both constructors), `setSender` wired to a `vi.fn()` spy for the `RaiseEvent` test, and real
`registry.getRoom(...)` assertions for `Leave`.

- [ ] **Step 6: Run tests to verify they pass**

Run: `npx vitest run --root realtime realtime/test/roomOperations.test.ts realtime/test/rooms.test.ts`
Expected: PASS

- [ ] **Step 7: Wire `setSender` in `main.ts`**

In `realtime/src/main.ts`, after constructing `TransportServer`, call `transport.setSender((remoteKey, buf) => { const [address, portStr] = remoteKey.split(":"); socket.send(buf, Number(portStr), address); });` — reusing the same `address:port` encoding `handleDatagram`'s callers already use for `remoteKey`.

- [ ] **Step 8: Run the full realtime suite**

Run: `npx vitest run --root realtime`
Expected: all PASS

- [ ] **Step 9: Commit**

```bash
git add realtime/src/transport/server.ts realtime/src/main.ts realtime/src/operations/event.ts realtime/src/rooms/registry.ts realtime/test/roomOperations.test.ts realtime/test/rooms.test.ts
git commit -m "feat: wire CreateGame/JoinGame/RaiseEvent/Leave into the operation dispatcher"
```

---

## Task 11: `rtpvp` state bucket and queue-join endpoint

**Files:**
- Modify: `server/src/game/state.ts` (add the `rtpvp` bucket next to the existing `pvp`/`pvpTeams` fields)
- Create: `server/src/game/rtpvp.ts`
- Create: `server/src/api/handlers/rtpvp.ts` (or extend the existing `rtpvp2` empty-read removal if one already exists under that name — check `server/src/api/handlers/` first)
- Test: `server/test/rtpvp.test.ts`

**Interfaces:**
- Consumes: `Grants`/`applyReward` pattern is not needed here (no reward on queueing) — this task only touches player state and the HTTP layer, no realtime code yet
- Produces: `joinQueue(state: PlayerState, now: number): void` and `leaveQueue(state: PlayerState): void` in `server/src/game/rtpvp.ts`; `rtpvp: { points: number; winCount: number; lossCount: number; queuedAt: number | null }` on `PlayerState`

- [ ] **Step 1: Check for an existing `rtpvp2` handler file**

Run: `grep -rn "rtpvp2" server/src/api/handlers`

If a file already exists (per earlier session notes, `api/rtpvp2/checktitlereward.json` etc. are currently served from `empty-reads.ts`), this task adds a new file `server/src/api/handlers/rtpvp.ts` for the new, non-empty endpoints rather than editing `empty-reads.ts` in place, and removes only the specific paths this task implements from `EMPTY_READS`.

- [ ] **Step 2: Write the failing test for the state helpers**

```typescript
// server/test/rtpvp.test.ts
import { describe, expect, it } from "vitest";
import { joinQueue, leaveQueue } from "../src/game/rtpvp.ts";
import { emptyState } from "../src/game/state.ts";

describe("rtpvp queue state", () => {
  it("records when a player joined the queue", () => {
    const state = emptyState();
    joinQueue(state, 1000);
    expect(state.rtpvp.queuedAt).toBe(1000);
  });

  it("clears the queued timestamp on leave", () => {
    const state = emptyState();
    joinQueue(state, 1000);
    leaveQueue(state);
    expect(state.rtpvp.queuedAt).toBeNull();
  });

  it("starts a fresh account with zero points and no queue", () => {
    const state = emptyState();
    expect(state.rtpvp).toEqual({ points: 0, winCount: 0, lossCount: 0, queuedAt: null });
  });
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `npx vitest run --root server server/test/rtpvp.test.ts`
Expected: FAIL — `server/src/game/rtpvp.ts` does not exist and `PlayerState` has no `rtpvp` field.

- [ ] **Step 4: Add the `rtpvp` field to `PlayerState` and `emptyState()`**

In `server/src/game/state.ts`, next to the existing `pvp: Record<string, { pvp_point: number; win_count: number; lose_count: number }>;` field, add:

```typescript
  /** Ninja World Ultimate Showdown: a single ladder (seasons/tiers are a later layer). */
  rtpvp: { points: number; winCount: number; lossCount: number; queuedAt: number | null };
```

In `emptyState()`, add the matching default:
```typescript
    rtpvp: { points: 0, winCount: 0, lossCount: 0, queuedAt: null },
```

Also add it to `normalizeState()`'s spread-in-defaults (it already is, via `{ ...emptyState(), ...(raw ?? {}) }` — no extra code needed there, but confirm this by reading the function before moving on).

- [ ] **Step 5: Write `server/src/game/rtpvp.ts`**

```typescript
// server/src/game/rtpvp.ts
import type { PlayerState } from "./state.ts";

export function joinQueue(state: PlayerState, now: number): void {
  state.rtpvp.queuedAt = now;
}

export function leaveQueue(state: PlayerState): void {
  state.rtpvp.queuedAt = null;
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `npx vitest run --root server server/test/rtpvp.test.ts`
Expected: PASS

- [ ] **Step 7: Add the HTTP endpoint**

Check `shared/protocol/ios-2.28.0/api-map.json` for the exact request/response field names of whichever `rtpvp2` endpoint is the queue-join call (search for `"queue"` or similar in its field names, or check the client's `NetworkManager` method list for the operation named something like `RtPvP2Entry`/`RtPvP2Queue`). Write a handler in `server/src/api/handlers/rtpvp.ts` following the existing pattern from `server/src/api/handlers/shop.ts` (a `Handler<"api/rtpvp2/...json">` object with `auth: "session"`, calling `joinQueue`/`leaveQueue` through `ctx.stores.players.mutate`), then add it to `server/src/api/handlers/index.ts`'s handler map and remove its path from `EMPTY_READS` in `empty-reads.ts` if it was listed there.

Add a test in `server/test/rtpvp.test.ts` that calls this endpoint through `t.call(...)` (same pattern as `server/test/shop.test.ts`) and asserts `status_code` 200 and that the player's queue state changed.

- [ ] **Step 8: Run the full server suite**

Run: `npx vitest run --root server`
Expected: all PASS

- [ ] **Step 9: Commit**

```bash
git add server/src/game/state.ts server/src/game/rtpvp.ts server/src/api/handlers/rtpvp.ts server/src/api/handlers/index.ts server/test/rtpvp.test.ts
git commit -m "feat: rtpvp queue state and join/leave endpoint"
```

---

## Task 12: Matchmaker loop and room creation on pairing

**Pre-flight finding (recorded in the ledger before this task was dispatched):** the plan's prose
described calling "the realtime server's room-creation" without any task actually adding an HTTP
listener to the realtime process — it is pure UDP (`dgram`) through Task 10. Ruling: this task adds
a small internal HTTP listener to the realtime process using Node's built-in `http` module (no new
dependency), bound to its own port, reachable only on the Docker network. This is the one piece of
this task that touches `realtime/`, not `server/`.

**Files:**
- Create: `server/src/game/matchmaker.ts`
- Create: `realtime/src/internal/roomApi.ts`
- Modify: `realtime/src/main.ts` (start the internal HTTP listener alongside the UDP socket)
- Test: `server/test/matchmaker.test.ts`, `realtime/test/roomApi.test.ts`

**Interfaces:**
- Consumes: `PlayerState.rtpvp` (Task 11); `RoomRegistry` (Task 9)
- Produces:
  - `findMatch(players: { id: number; rtpvp: { points: number; queuedAt: number | null } }[], now: number, bandConfig?: { basePoints: number; growthPerSecond: number }): { a: number; b: number } | null`
  - `roomPairings: Map<string, { a: number; b: number }>` exported from `server/src/game/matchmaker.ts` — the matchmaker loop's own record of which two players each room id was created for. Task 13 consumes this by this exact name to verify a result's caller actually belongs to the room they're reporting on.
  - `startRoomApi(port: number, registry: RoomRegistry): { close(): void }` from `realtime/src/internal/roomApi.ts` — a plain HTTP server with one route, `POST /internal/rooms` accepting `{ roomId: string, playerIds: number[] }`, calling `registry.createRoom(roomId, playerIds[0])` then `registry.joinRoom(roomId, id)` for each remaining id.

- [ ] **Step 1: Write the failing tests**

```typescript
// server/test/matchmaker.test.ts
import { describe, expect, it } from "vitest";
import { findMatch } from "../src/game/matchmaker.ts";

describe("findMatch", () => {
  it("pairs the two closest-ranked queued players", () => {
    const players = [
      { id: 1, rtpvp: { points: 100, queuedAt: 0 } },
      { id: 2, rtpvp: { points: 500, queuedAt: 0 } },
      { id: 3, rtpvp: { points: 110, queuedAt: 0 } },
    ];
    const match = findMatch(players, 1);
    expect([match?.a, match?.b].sort()).toEqual([1, 3]);
  });

  it("returns null when fewer than two players are queued", () => {
    const players = [{ id: 1, rtpvp: { points: 100, queuedAt: 0 } }];
    expect(findMatch(players, 1)).toBeNull();
  });

  it("ignores players who are not queued", () => {
    const players = [
      { id: 1, rtpvp: { points: 100, queuedAt: 0 } },
      { id: 2, rtpvp: { points: 105, queuedAt: null } },
    ];
    expect(findMatch(players, 1)).toBeNull();
  });

  it("widens the acceptable points band the longer a player has waited", () => {
    const players = [
      { id: 1, rtpvp: { points: 0, queuedAt: 0 } },
      { id: 2, rtpvp: { points: 1000, queuedAt: 0 } },
    ];
    // Too far apart to match right after queueing.
    expect(findMatch(players, 1, { basePoints: 50, growthPerSecond: 10 })).toBeNull();
    // After waiting long enough, the band has grown enough to cover the gap.
    expect(findMatch(players, 100, { basePoints: 50, growthPerSecond: 10 })).not.toBeNull();
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run --root server server/test/matchmaker.test.ts`
Expected: FAIL — `server/src/game/matchmaker.ts` does not exist.

- [ ] **Step 3: Write `server/src/game/matchmaker.ts`**

```typescript
// server/src/game/matchmaker.ts
interface QueuedPlayer {
  id: number;
  rtpvp: { points: number; queuedAt: number | null };
}

/**
 * Pairs the two closest-ranked queued players whose points gap fits the current band.
 * The band widens with wait time so nobody waits forever for an exact match (spec: Matchmaking
 * and ranking data model). INFERRED band shape -- confirm against real play once this ships.
 */
export function findMatch(
  players: QueuedPlayer[],
  now: number,
  bandConfig: { basePoints: number; growthPerSecond: number } = { basePoints: 50, growthPerSecond: 5 },
): { a: number; b: number } | null {
  const queued = players.filter((p) => p.rtpvp.queuedAt !== null);
  if (queued.length < 2) return null;

  let best: { a: number; b: number; gap: number } | null = null;
  for (let i = 0; i < queued.length; i++) {
    for (let j = i + 1; j < queued.length; j++) {
      const x = queued[i]!;
      const y = queued[j]!;
      const gap = Math.abs(x.rtpvp.points - y.rtpvp.points);
      const waited = now - Math.min(x.rtpvp.queuedAt!, y.rtpvp.queuedAt!);
      const band = bandConfig.basePoints + bandConfig.growthPerSecond * waited;
      if (gap > band) continue;
      if (!best || gap < best.gap) best = { a: x.id, b: y.id, gap };
    }
  }
  return best ? { a: best.a, b: best.b } : null;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run --root server server/test/matchmaker.test.ts`
Expected: PASS

- [ ] **Step 5: Write the failing test for the internal room API**

```typescript
// realtime/test/roomApi.test.ts
import { describe, expect, it } from "vitest";
import { RoomRegistry } from "../src/rooms/registry.ts";
import { startRoomApi } from "../src/internal/roomApi.ts";

describe("internal room API", () => {
  it("creates a room with the given players on POST /internal/rooms", async () => {
    const registry = new RoomRegistry();
    const api = startRoomApi(0, registry);
    const port = (api as unknown as { address: { port: number } }).address.port;
    const res = await fetch(`http://127.0.0.1:${port}/internal/rooms`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ roomId: "room1", playerIds: [1, 2], kind: "pvp" }),
    });
    expect(res.status).toBe(200);
    expect(registry.getRoom("room1")!.playerIds).toEqual([1, 2]);
    expect(registry.getRoom("room1")!.kind).toBe("pvp");
    api.close();
  });
});
```

- [ ] **Step 6: Run test to verify it fails**

Run: `npx vitest run --root realtime realtime/test/roomApi.test.ts`
Expected: FAIL — `realtime/src/internal/roomApi.ts` does not exist.

- [ ] **Step 7: Write `realtime/src/internal/roomApi.ts`**

```typescript
// realtime/src/internal/roomApi.ts
import http from "node:http";
import type { RoomRegistry } from "../rooms/registry.ts";

export function startRoomApi(port: number, registry: RoomRegistry) {
  const server = http.createServer((req, res) => {
    if (req.method !== "POST" || req.url !== "/internal/rooms") {
      res.writeHead(404).end();
      return;
    }
    let body = "";
    req.on("data", (chunk) => (body += chunk));
    req.on("end", () => {
      const { roomId, playerIds, kind } = JSON.parse(body) as { roomId: string; playerIds: number[]; kind?: "pvp" | "coop" };
      registry.createRoom(roomId, playerIds[0]!, kind ?? "pvp");
      for (const id of playerIds.slice(1)) registry.joinRoom(roomId, id);
      res.writeHead(200, { "content-type": "application/json" }).end("{}");
    });
  });
  server.listen(port);
  return server;
}
```

Fix the test's `address` access to use the real return type (`http.Server`, whose `.address()` is a method, not a property — adjust the test to call `(api.address() as import("node:net").AddressInfo).port` instead of the cast shown above, which was written loosely; get this right before running the test, not after).

- [ ] **Step 8: Run test to verify it passes**

Run: `npx vitest run --root realtime realtime/test/roomApi.test.ts`
Expected: PASS

- [ ] **Step 9: Start the room API alongside the UDP socket in `realtime/src/main.ts`**

**Note: `main.ts` has changed since this step was first written.** An inserted task (10.5, which
ran before this one) already added a `RoomRegistry` constructor parameter to `TransportServer`
and already constructs one `registry` instance in `startRealtimeServer`, specifically leaving a
comment there saying this task should reuse it. Read `realtime/src/main.ts` as it exists now
before editing — do NOT construct a second `RoomRegistry` or re-add the constructor parameter.
Add only the room API's startup, reusing the existing `registry` variable:

```typescript
// in realtime/src/main.ts's startRealtimeServer, inside the Promise body, after the existing
// `const registry = new RoomRegistry();` line (added by Task 10.5) and before `socket.bind(port)`:
import { startRoomApi } from "./internal/roomApi.ts"; // add to the top-level imports

// ... existing registry/transport/socket setup stays exactly as it is ...
const roomApi = startRoomApi(Number(process.env.REALTIME_INTERNAL_PORT ?? 5059), registry);
```

`roomApi`'s lifecycle (closing it when the realtime server shuts down) should be wired into the
returned `RealtimeServer.close()` alongside `socket.close()`, so a test that calls `close()`
doesn't leak an open HTTP listener across test runs.

- [ ] **Step 10: Write the matchmaker loop and `roomPairings` in `server/src/game/matchmaker.ts`**

Add to the same file as `findMatch`:

```typescript
export const roomPairings = new Map<string, { a: number; b: number }>();

export async function matchmakerTick(
  stores: { players: { list(): Promise<{ id: number; state: { rtpvp: { points: number; queuedAt: number | null } } }[]>; mutate(id: number, fn: (p: { state: { rtpvp: { queuedAt: number | null } } }) => void): Promise<unknown> } },
  now: number,
  realtimeInternalUrl: string,
): Promise<void> {
  const players = await stores.players.list();
  const match = findMatch(players.map((p) => ({ id: p.id, rtpvp: p.state.rtpvp })), now);
  if (!match) return;
  await stores.players.mutate(match.a, (p) => { p.state.rtpvp.queuedAt = null; });
  await stores.players.mutate(match.b, (p) => { p.state.rtpvp.queuedAt = null; });
  const roomId = crypto.randomUUID();
  roomPairings.set(roomId, match);
  await fetch(`${realtimeInternalUrl}/internal/rooms`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ roomId, playerIds: [match.a, match.b], kind: "pvp" }),
  });
}
```

Wire a `setInterval(() => matchmakerTick(stores, Date.now(), process.env.REALTIME_INTERNAL_URL ?? "http://realtime:5059"), 1500)` into `server/src/main.ts`, started alongside the existing server startup.

Add a test in `server/test/matchmaker.test.ts` that stubs `fetch` (via `vi.stubGlobal("fetch", vi.fn(async () => new Response("{}")))`) and a `players.list`/`mutate` pair backed by `testServer()`'s real stores, calls `matchmakerTick` directly for two queued players, and asserts both players' `queuedAt` is cleared and `roomPairings` gained an entry.

- [ ] **Step 11: Run the full server and realtime suites**

Run: `npx vitest run --root server && npx vitest run --root realtime`
Expected: all PASS

- [ ] **Step 12: Document the internal endpoint and commit**

Add a short section to `docs/online.md` describing `POST /internal/rooms` (realtime process, port 5059) and that it is LAN/Docker-network-only, never exposed publicly — add the port mapping consideration to `docker-compose.yml`'s realtime service comment (do not actually publish 5059 outside the Docker network; it should stay reachable only via the Compose network, unlike the UDP ports which are deliberately published).

```bash
git add server/src/game/matchmaker.ts server/test/matchmaker.test.ts server/src/main.ts realtime/src/internal/roomApi.ts realtime/src/main.ts realtime/src/transport/server.ts realtime/test/roomApi.test.ts docs/online.md
git commit -m "feat: matchmaker loop pairs queued players and creates a realtime room via an internal API"
```

---

## Task 13: Result recording — win/loss and Rank Points

**Files:**
- Create: `server/src/game/rtpvpResult.ts`
- Modify: `server/src/api/handlers/rtpvp.ts` (add the result endpoint)
- Test: `server/test/rtpvp.test.ts` (extend)

**Interfaces:**
- Consumes: `PlayerState.rtpvp` (Task 11); `roomPairings: Map<string, { a: number; b: number }>` exported from `server/src/game/matchmaker.ts` (Task 12)
- Produces: `recordResult(state: PlayerState, won: boolean): { gained: number }` — also imported directly by Task 15's disconnect-forfeit handling, which calls it the same way this task's own HTTP handler does, against the same shared `Stores`

- [ ] **Step 1: Write the failing test**

```typescript
// append to server/test/rtpvp.test.ts
import { recordResult } from "../src/game/rtpvpResult.ts";

describe("rtpvp result recording", () => {
  it("awards points and increments win count on a win", () => {
    const state = emptyState();
    const { gained } = recordResult(state, true);
    expect(gained).toBeGreaterThan(0);
    expect(state.rtpvp.points).toBe(gained);
    expect(state.rtpvp.winCount).toBe(1);
  });

  it("never takes points below zero on a loss", () => {
    const state = emptyState();
    recordResult(state, true);
    const before = state.rtpvp.points;
    recordResult(state, false);
    expect(state.rtpvp.points).toBeLessThanOrEqual(before);
    expect(state.rtpvp.points).toBeGreaterThanOrEqual(0);
    expect(state.rtpvp.lossCount).toBe(1);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run --root server server/test/rtpvp.test.ts`
Expected: FAIL — `server/src/game/rtpvpResult.ts` does not exist.

- [ ] **Step 3: Write `server/src/game/rtpvpResult.ts`**

```typescript
// server/src/game/rtpvpResult.ts
import type { PlayerState } from "./state.ts";

// INFERRED point values -- same category as Phantom Castle's WIN_POINTS/LOSE_POINTS
// (server/src/game/pvp.ts), no client or oracle data for Ninja World Ultimate Showdown's own
// numbers yet. Revisit once the oracle is reachable again.
export const RTPVP_WIN_POINTS = 25;
export const RTPVP_LOSE_POINTS = 10;

export function recordResult(state: PlayerState, won: boolean): { gained: number } {
  const before = state.rtpvp.points;
  if (won) {
    state.rtpvp.points += RTPVP_WIN_POINTS;
    state.rtpvp.winCount++;
  } else {
    state.rtpvp.points = Math.max(0, state.rtpvp.points - RTPVP_LOSE_POINTS);
    state.rtpvp.lossCount++;
  }
  return { gained: state.rtpvp.points - before };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run --root server server/test/rtpvp.test.ts`
Expected: PASS

- [ ] **Step 5: Add the HTTP result endpoint**

In `server/src/api/handlers/rtpvp.ts`, following the exact pattern of `server/src/api/handlers/pvp.ts`'s `result` handler: a `Handler` that takes `room_id` and `win_lose` from the body, looks up `roomPairings.get(room_id)` (imported from `../../game/matchmaker.ts`) and throws `ApiError(StatusCode.BadRequest, ...)` if the pairing is missing or the calling `ctx.player!.id` is neither `pairing.a` nor `pairing.b` (a result can't be forged for a room the caller wasn't in), calls `recordResult(player.state, Number(body.win_lose) === 1)` through `ctx.stores.players.mutate`, deletes the `roomPairings` entry (one result per room), and returns the updated `rtpvp` view plus `get_pvp_point`. Add a test exercising this through `t.call(...)` that first seeds `roomPairings` directly (import it in the test file) before calling the endpoint.

- [ ] **Step 6: Run the full server suite**

Run: `npx vitest run --root server`
Expected: all PASS

- [ ] **Step 7: Commit**

```bash
git add server/src/game/rtpvpResult.ts server/src/api/handlers/rtpvp.ts server/test/rtpvp.test.ts
git commit -m "feat: Ninja World Ultimate Showdown result recording"
```

---

## Task 14: Co-op room creation and mail invite

**Files:**
- Modify: `server/src/api/handlers/mission.ts` (the existing `startmultihost` handler — add room creation)
- Modify: `server/src/game/social.ts` (reuse the existing mail-send path for the invite)
- Test: `server/test/multi.test.ts` (existing file — extend)

**Interfaces:**
- Consumes: Task 9's room creation (via the same internal endpoint pattern as Task 12), the existing `sendMail` function:
  `export async function sendMail(players: PlayerStore, self: Player, partnerId: number, input: MailInput, now: number)`
  where `MailInput` already has `roomId?: string`, `roomPass?: string`, `clusterName?: string` fields built for exactly this (`server/src/game/social.ts:269-275`).
- Produces: `startmultihost`'s response now also triggers a mail to each invited guest containing the room id

**Important constraint found while reading the existing code:** `sendMail` throws unless `self.state.friends[String(partnerId)]` is set — "only friends can mail each other" (`server/src/game/social.ts:282`). Confirm in Step 1 whether `startmultihost`'s `guest_player_id_list` can only ever contain friends already (check how guests are chosen — `mission/supportlist` lists friends first, per `CLAUDE.md`'s notes on helpers). If a non-friend guest is possible, this task must not call `sendMail` as-is for that case; write the invite directly via `ctx.stores.players.mutate(guestId, ...)` using the same mail-shape `sendMail` builds, bypassing the friends check, rather than relaxing `sendMail`'s check globally (that check exists for the player-to-player mail feature and should keep protecting it).

- [ ] **Step 1: Read the existing `startmultihost` handler and confirm the friends constraint**

Run: `grep -n "startMultiHost" server/src/api/handlers/mission.ts` and read the handler in full, including how `guest_player_id_list` is validated. Also read `server/src/game/social.ts:269-304` (`MailInput`, `sendMail`) in full. Decide, from what you read, whether `sendMail` is directly usable or whether this task needs the direct-mutate fallback described above — do not guess; the handler's own validation of `guest_player_id_list` will show whether non-friends can be guests.

- [ ] **Step 2: Write the failing test**

**Correction: `server/test/multi.test.ts` has no shared setup helper** — both of its existing tests
inline `const t = testServer(); const host = await t.login("a".repeat(32)); const guest = await
t.login("b".repeat(32));` directly (see the file's first test). Follow that exact pattern, not a
helper that doesn't exist. Deliberately do NOT make `host` and `guest` friends in this test before
calling `startMultiHost` — whether that matters is exactly what Step 1's investigation is for; if
`sendMail`'s friends-only check turns out to block this, that failure is the evidence Step 1 asked
you to look for, not a test bug to paper over.

```typescript
// append to server/test/multi.test.ts
it("invites each guest to the realtime room by mail when hosting multiplayer", async () => {
  const t = testServer();
  const host = await t.login("a".repeat(32));
  const guest = await t.login("b".repeat(32));
  await t.call("api/mission/startmultihost.json", common({ ...host, mission_id: 100101, guest_player_id_list: [guest.player_id] }));
  const mail = await t.call<{ u_mail: Record<string, { room_id?: string }> }>("api/mail/list.json", common(guest));
  const invite = Object.values(mail.data!.u_mail).find((m) => m.room_id);
  expect(invite?.room_id).toBeTruthy();
});
```

- [ ] **Step 3: Run test to verify it fails**

Run: `npx vitest run --root server server/test/multi.test.ts`
Expected: FAIL — no mail is sent today.

- [ ] **Step 4: Add room creation and mail invite to `startMultiHost`**

In `server/src/api/handlers/mission.ts`'s `startMultiHost` handler, after the existing stamina/drop logic succeeds: generate a room id (same pattern as Task 12's matchmaker — a UUID), `POST` to the realtime process's internal room API (Task 12's `POST /internal/rooms`, same `fetch` call shape as the matchmaker's, with `{ roomId, playerIds: [hostPlayerId, ...guestPlayerIds], kind: "coop" }`), and for each guest call `sendMail(ctx.stores.players, hostPlayer, guestId, { message: "Co-op room ready", roomId }, unixSeconds(ctx.now))` (or the Step 1 fallback if friendship cannot be assumed).

- [ ] **Step 5: Run test to verify it passes**

Run: `npx vitest run --root server server/test/multi.test.ts`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add server/src/api/handlers/mission.ts server/test/multi.test.ts
git commit -m "feat: co-op host creates a realtime room and invites guests by mail"
```

---

## Task 15: Disconnect handling — PvP auto-forfeit and co-op guest drop

**Correction: `DisconnectMonitor` tracks `lastSeen` itself (a `Map<roomId, Map<playerId,
timestamp>>` field on the class, per Step 3's actual code below), not a field on `Room`/
`RoomRegistry`.** The Files list below no longer claims a `registry.ts` modification for this —
earlier plan text said it would, but the real design (Step 3) never touches that file.

**Files:**
- Modify: `realtime/src/transport/server.ts` (accept an optional `DisconnectMonitor`, call `.touch(...)` on every datagram from a peer whose room is known)
- Modify: `realtime/src/main.ts` (construct one `DisconnectMonitor`, pass it into `TransportServer`, poll `checkTimeouts` on an interval)
- Create: `realtime/src/rooms/disconnectMonitor.ts`
- Test: `realtime/test/disconnectMonitor.test.ts`

**Interfaces:**
- Consumes: `Room`/`RoomRegistry` (Task 9), `findRoomForPlayer` (Task 10.5)
- Produces: `class DisconnectMonitor` with `touch(roomId: string, playerId: number, now: number): void` and `checkTimeouts(now: number, gracePeriodMs: number, onForfeit: (room: Room, remainingPlayerId: number, droppedPlayerId: number) => void | Promise<void>): void`

- [ ] **Step 1: Write the failing test**

```typescript
// realtime/test/disconnectMonitor.test.ts
import { describe, expect, it, vi } from "vitest";
import { RoomRegistry } from "../src/rooms/registry.ts";
import { DisconnectMonitor } from "../src/rooms/disconnectMonitor.ts";

describe("DisconnectMonitor", () => {
  it("fires onForfeit once a player has been silent past the grace period", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    registry.joinRoom("room1", 2);
    const monitor = new DisconnectMonitor(registry);
    monitor.touch(room.id, 1, 0);
    monitor.touch(room.id, 2, 0);
    monitor.touch(room.id, 1, 5000); // player 1 kept sending; player 2 went silent

    const onForfeit = vi.fn();
    monitor.checkTimeouts(5000, 3000, onForfeit);
    expect(onForfeit).toHaveBeenCalledWith(room, 1, 2);
  });

  it("does not fire for a room where both players are still active", () => {
    const registry = new RoomRegistry();
    const room = registry.createRoom("room1", 1);
    registry.joinRoom("room1", 2);
    const monitor = new DisconnectMonitor(registry);
    monitor.touch(room.id, 1, 0);
    monitor.touch(room.id, 2, 0);

    const onForfeit = vi.fn();
    monitor.checkTimeouts(1000, 3000, onForfeit);
    expect(onForfeit).not.toHaveBeenCalled();
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run --root realtime realtime/test/disconnectMonitor.test.ts`
Expected: FAIL — `realtime/src/rooms/disconnectMonitor.ts` does not exist.

- [ ] **Step 3: Write `realtime/src/rooms/disconnectMonitor.ts`**

```typescript
// realtime/src/rooms/disconnectMonitor.ts
import type { Room, RoomRegistry } from "./registry.ts";

export class DisconnectMonitor {
  private readonly lastSeen = new Map<string, Map<number, number>>(); // roomId -> playerId -> timestamp
  private readonly forfeited = new Set<string>(); // `${roomId}:${playerId}` already reported

  constructor(private readonly registry: RoomRegistry) {}

  touch(roomId: string, playerId: number, now: number): void {
    let byPlayer = this.lastSeen.get(roomId);
    if (!byPlayer) this.lastSeen.set(roomId, (byPlayer = new Map()));
    byPlayer.set(playerId, now);
  }

  checkTimeouts(now: number, gracePeriodMs: number, onForfeit: (room: Room, remainingPlayerId: number, droppedPlayerId: number) => void): void {
    for (const [roomId, byPlayer] of this.lastSeen) {
      const room = this.registry.getRoom(roomId);
      if (!room || room.playerIds.length !== 2) continue;
      const [a, b] = room.playerIds;
      const seenA = byPlayer.get(a!) ?? 0;
      const seenB = byPlayer.get(b!) ?? 0;
      const key = (droppedId: number) => `${roomId}:${droppedId}`;
      if (now - seenB >= gracePeriodMs && now - seenA < gracePeriodMs && !this.forfeited.has(key(b!))) {
        this.forfeited.add(key(b!));
        onForfeit(room, a!, b!);
      } else if (now - seenA >= gracePeriodMs && now - seenB < gracePeriodMs && !this.forfeited.has(key(a!))) {
        this.forfeited.add(key(a!));
        onForfeit(room, b!, a!);
      }
    }
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run --root realtime realtime/test/disconnectMonitor.test.ts`
Expected: PASS

- [ ] **Step 5: Wire `touch` to every received datagram, and poll `checkTimeouts` on an interval**

`TransportServer`'s constructor currently takes `(sessions?: SessionLookup, registry?: RoomRegistry)` (Task 8/10.5). Add a third, optional parameter: `(sessions?, registry?, disconnectMonitor?: DisconnectMonitor)`. At the top of `handleDatagram`, after resolving `peer` (and whenever `peer.playerId` is defined), call `this.registry.findRoomForPlayer(peer.playerId)` and, if it returns a room, `this.disconnectMonitor?.touch(room.id, peer.playerId, Date.now())` — once per datagram is enough, don't call it per-command inside the datagram's command loop.

In `realtime/src/main.ts`, construct one `DisconnectMonitor` alongside the existing `registry`, pass it as `TransportServer`'s third constructor argument, and poll `checkTimeouts` on a `setInterval`.

**Pre-flight finding (recorded in the ledger before this task was dispatched):** the plan's prose
described the forfeit handler calling "the same internal result-recording path Task 13 built" —
but Task 13's HTTP endpoint is player-session-authenticated (`auth: "session"`, needs a live
`ssid`), which a server-to-server forfeit call has no way to present. Ruling: the realtime process
already depends on `@nb/server` directly (Task 2's package.json) and both processes already share
the same Postgres-backed `Stores`, so the forfeit handler imports and calls `recordResult` (Task
13's plain function, `server/src/game/rtpvpResult.ts`) directly against that shared `Stores` —
no HTTP round-trip, no new internal endpoint, same function the HTTP handler itself calls.

In `realtime/src/main.ts`, add a `setInterval` (every 1 second) calling `checkTimeouts`. Its
`onForfeit` callback:

```typescript
// in realtime/src/main.ts, after wiring the UDP socket and room API
import { recordResult } from "@nb/server/src/game/rtpvpResult.ts";
import { postgresStores } from "@nb/server/src/store/postgres.ts";
// Reuses the same Stores construction pattern server/src/main.ts already uses -- read that
// file's own postgresStores(...) call before writing this one, to match its exact options
// (databaseUrl, redisUrl, sessionTtlSeconds) and env var names verbatim, not by guessing them here.
const stores = postgresStores({
  databaseUrl: process.env.DATABASE_URL!,
  redisUrl: process.env.REDIS_URL!,
  sessionTtlSeconds: Number(process.env.SESSION_TTL_SECONDS ?? 86400),
});

disconnectMonitor.checkTimeouts(Date.now(), 10_000, async (room, remainingPlayerId, droppedPlayerId) => {
  if (room.kind !== "pvp") return; // co-op: the host's existing solo mission flow continues untouched
  const dropped = await stores.players.findById(droppedPlayerId);
  const remaining = await stores.players.findById(remainingPlayerId);
  if (dropped) await stores.players.mutate(dropped.id, (p) => { recordResult(p.state, false); });
  if (remaining) await stores.players.mutate(remaining.id, (p) => { recordResult(p.state, true); });
});
```

`room.kind` (Task 9's `Room` interface) is already set correctly at creation time — Task 12's
matchmaker tick and Task 14's co-op host flow each pass their own `kind` (`"pvp"` / `"coop"`)
through `POST /internal/rooms`'s body, which `startRoomApi` forwards into `registry.createRoom`.

- [ ] **Step 6: Run the full realtime suite**

Run: `npx vitest run --root realtime`
Expected: all PASS

- [ ] **Step 7: Commit**

```bash
git add realtime/src/rooms/disconnectMonitor.ts realtime/src/transport/server.ts realtime/src/main.ts realtime/test/disconnectMonitor.test.ts
git commit -m "feat: auto-forfeit a PvP match after a disconnected player's grace period"
```

---

## Task 16: Extend `repoint_ipa.py` to repoint the Photon address literals

**Files:**
- Modify: `tools/repoint_ipa.py`
- Test: existing script has no automated test harness (confirm by reading the file) — this task's verification is a manual run against a copy of the IPA, documented below, not a new pytest file.

**Interfaces:**
- Consumes: nothing new
- Produces: a new CLI flag, `--photon-host <ip>`, that appends the new name-server/master-server address strings into `global-metadata.dat` and repoints the two literals' offset/length to them

- [ ] **Step 1: Read the existing literal-patching code**

Run: `grep -n "def patch\|global-metadata\|offset" tools/repoint_ipa.py` and read the surrounding function(s) in full. The existing HTTP-URL patch works in-place because the new string is the same length or shorter; this task's strings are longer, so it needs the append-and-repoint approach described in the spec (Client-side change required) — confirm the current code's string-table layout before writing the new function, since the new function must not corrupt offsets used by other, unrelated strings.

- [ ] **Step 2: Add the append-and-repoint function**

Add a function (name and signature following the file's existing conventions, e.g. `def repoint_long_string(metadata: bytearray, old_offset: int, new_value: str) -> int:`) that: appends `new_value`'s UTF-16 (or whatever encoding the existing string table uses — confirm from Step 1, do not assume) bytes to the end of the metadata blob, records the new offset and length, and returns the new offset so the caller can patch the literal's reference (the 4- or 8-byte offset field pointing at the string, located via the same lookup mechanism the existing patch function uses).

- [ ] **Step 3: Add the `--photon-host` CLI flag**

In the script's argument parser, add `--photon-host` (default: unset, meaning don't touch Photon literals). When provided, locate the two known literals (`127.0.0.1:15058`, `127.0.0.1:15055` — the two addresses documented in `docs/online.md`), compute the new values (`f"{photon_host}:5058"`, `f"{photon_host}:5055"`), and call the new repoint function for each.

- [ ] **Step 4: Verify manually against a copy of the IPA**

Run: `python tools/repoint_ipa.py Blazing-Rebirth-iOS-offline.ipa build/Blazing-Private-LAN-photon-test.ipa --host 192.168.1.137 --photon-host 192.168.1.137`

Then use a hex/strings tool (e.g. `strings build/Blazing-Private-LAN-photon-test.ipa | grep "192.168.1.137:505"`) to confirm both new addresses appear in the output IPA. This is the task's done-criterion — full on-device/Photon-client verification happens in Task 17.

- [ ] **Step 5: Document the new flag**

Add a line to `docs/running.md`'s existing IPA-repointing section describing `--photon-host` and when to use it (only once the realtime server from Tasks 1-15 is actually running and validated per Task 17 — otherwise the client will try to connect to a Photon server that refuses it).

- [ ] **Step 6: Commit**

```bash
git add tools/repoint_ipa.py docs/running.md
git commit -m "feat: repoint_ipa.py can repoint the Photon server address literals"
```

---

## Task 17: Validate against Photon's own SDK, then against the real client

**Files:**
- Create: `realtime/scratch/pc-sdk-client/` (a throwaway PC test client using Photon's published Unity/PC SDK — not shipped, not under test coverage, explicitly a validation harness)
- Create: `docs/realtime-validation.md` (the checklist and results)

**Interfaces:**
- Consumes: the full realtime server from Tasks 1-15, running via `npm run dev -w realtime`
- Produces: a documented pass/fail record for each operation in scope

- [ ] **Step 1: Set up Photon's PC SDK test client**

Download Photon's publicly available PC/Unity SDK (per the spec's De-risking plan — this is Exit Games' own distributed SDK, not anything extracted from the game client). Create a minimal project under `realtime/scratch/pc-sdk-client/` that connects to `127.0.0.1:5058` (point it at your dev machine running the realtime server from Task 2 onward), and exercises: connect, `Authenticate` (Task 8), `JoinLobby`, `CreateGame` (Task 9), `JoinGame` (Task 9), `RaiseEvent` (Task 10), `Leave` (Task 10).

- [ ] **Step 2: Run the realtime server locally**

Run: `npm run dev -w realtime`

- [ ] **Step 3: Run the PC SDK client against it and record results**

For each operation in Step 1's list, record in `docs/realtime-validation.md` under a "PC SDK validation" heading: whether the real Photon client considered the exchange successful, and if not, the exact point of failure (which lets you go back to the specific task's file and fix it with a real signal, rather than guessing).

- [ ] **Step 4: Only once Step 3 is fully green, repoint and test the real IPA**

Run Task 16's `--photon-host` flag against the real Rebirth-derived IPA, install it, and manually exercise: joining the Ninja World Ultimate Showdown queue from the home screen, getting matched (you'll need a second device or account queued at the same time — reuse the multi-account setup from `docs/running.md`'s "Adding more players" section), fighting a live match, and separately, hosting a co-op mission and having a second account join via the mail invite. Record each step's outcome in `docs/realtime-validation.md` under an "On-device validation" heading.

- [ ] **Step 5: Update `CLAUDE.md` and `docs/online.md` with the outcome**

Whatever the result — full success, partial success with specific gaps, or failure at a specific step — write it down per this project's "write findings down as you go" rule. If something fails only at Step 4 despite Step 3 being green, that is itself a useful, specific finding (likely something about the real client's exact handshake that the published SDK doesn't exercise the same way) — document it precisely rather than as a vague "doesn't work."

- [ ] **Step 6: Commit**

```bash
git add docs/realtime-validation.md CLAUDE.md docs/online.md
git commit -m "docs: record realtime PvP/co-op validation results"
```

---

## Self-Review Notes

- **Spec coverage:** Architecture (Task 2), Protocol layer + de-risking (Tasks 1, 17), Matchmaking/ranking (Tasks 11-13), Co-op rooms (Task 14), Disconnect handling (Task 15), Client-side change (Task 16). Non-goals (battle simulation, seasons/tiers, real regions) are not implemented anywhere in this plan, matching the spec.
- **Placeholder scan:** Task 1's constants are explicitly marked as values-to-replace from cited research, not left as an unexplained placeholder; Task 10 includes a deliberately-wrong line to make a refactor requirement concrete rather than hand-waving it, immediately corrected in the same step.
- **Type consistency:** `Room.playerIds: number[]` (Task 9) is used consistently through Tasks 10, 12, 15. `PlayerState.rtpvp` (Task 11) is used with the same field names (`points`, `winCount`, `lossCount`, `queuedAt`) through Tasks 12-13. `SessionLookup` (Task 8) matches the shape `Stores.sessions` already has in `server/src/store/types.ts` — confirm this during Task 8 itself, since this plan was written without re-reading that file's exact current shape.
