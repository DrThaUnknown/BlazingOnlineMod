// Runs the JavaScriptCore bundle (build/host.js) in a bare vm context: standard JavaScript only,
// no Buffer/process/setTimeout/require -- the same constraints as JSC on the phone -- plus a fake
// `__bo` built on Node sockets outside the sandbox. Proves the bundle needs nothing but `__bo`.
import { execFileSync } from "node:child_process";
import dgram from "node:dgram";
import fs from "node:fs";
import http from "node:http";
import crypto from "node:crypto";
import vm from "node:vm";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { decodeBody, encodeBody } from "../../vendor/shared/codec.ts";
import { P, asString, type PV } from "../../vendor/photon/protocol16.ts";
import { TestClient } from "../../vendor/test/client.ts";

type Ctx = Record<string, unknown> & {
  __bo_onUdp: (h: number, d: string, a: string, p: number) => void;
  __bo_onHttp: (h: number, id: number, m: string, p: string, b: string) => void;
  __bo_start: (host: string) => void;
  __bo_status: () => string;
  __bo_stop: () => void;
};

let ctx: Ctx;
const udp = new Map<number, dgram.Socket>();
const servers = new Map<number, http.Server>();
const pending = new Map<number, http.ServerResponse>();
const timers = new Map<number, NodeJS.Timeout>();
const files = new Map<string, string>();
const ports = { http: 0, name: 0 };
let next = 1;
const logs: string[] = [];

// Node binds asynchronously but the native binding is synchronous, so sockets are pre-bound.
const preUdp: dgram.Socket[] = [];
let preHttp: http.Server;

beforeAll(async () => {
  execFileSync(process.execPath, ["host/build.mjs"]);
  for (let i = 0; i < 3; i++) {
    const s = dgram.createSocket("udp4");
    await new Promise<void>((r) => s.bind(0, "127.0.0.1", () => r()));
    preUdp.push(s);
  }
  preHttp = http.createServer();
  await new Promise<void>((r) => preHttp.listen(0, "127.0.0.1", () => r()));
  ports.http = (preHttp.address() as { port: number }).port;
  const code = fs.readFileSync("build/host.js", "utf8");
  const bo = {
    log: (m: string) => logs.push(m),
    now: () => Date.now(),
    random: (n: number) => crypto.randomBytes(n).toString("latin1"),
    setInterval: (fn: () => void, ms: number) => {
      const id = next++;
      timers.set(id, setInterval(fn, ms));
      return id;
    },
    clearInterval: (id: number) => clearInterval(timers.get(id)),
    // Port 0 = any free port; the test maps the well-known ports so the bundle can still ask for them.
    udpBind: (port: number) => {
      const h = next++;
      const s = preUdp.shift()!;
      s.on("message", (m, r) => ctx.__bo_onUdp(h, m.toString("latin1"), r.address, r.port));
      udp.set(h, s);
      if (port === 5058) ports.name = s.address().port;
      return h;
    },
    udpPort: (h: number) => udp.get(h)!.address().port,
    udpSend: (h: number, d: string, p: number, a: string) => udp.get(h)!.send(Buffer.from(d, "latin1"), p, a),
    udpClose: (h: number) => udp.get(h)?.close(),
    httpListen: () => {
      const h = next++;
      const server = preHttp;
      server.on("request", (req, res) => {
        let body = "";
        req.on("data", (c) => (body += c));
        req.on("end", () => {
          const id = next++;
          pending.set(id, res);
          ctx.__bo_onHttp(h, id, req.method ?? "GET", req.url ?? "/", body);
        });
      });
      servers.set(h, server);
      return h;
    },
    httpRespond: (id: number, status: number, type: string, body: string) => {
      pending.get(id)?.writeHead(status, { "content-type": type }).end(body);
      pending.delete(id);
    },
    httpClose: (h: number) => servers.get(h)?.close(),
    readFile: (n: string) => files.get(n) ?? null,
    writeFile: (n: string, d: string) => void files.set(n, d),
  };
  ctx = vm.createContext({ __bo: bo }) as Ctx;
  vm.runInContext(code, ctx);
  ctx.__bo_start("127.0.0.1");
  for (let i = 0; i < 50 && !String(ctx.__bo_status()).startsWith("hosting"); i++) await new Promise((r) => setTimeout(r, 20));
});

afterAll(() => {
  ctx.__bo_stop();
  for (const t of timers.values()) clearInterval(t);
});

describe("host bundle in a JSC-like sandbox", () => {
  it("starts with only __bo available", () => {
    expect(ctx.__bo_status()).toMatch(/^hosting/);
    expect(logs.some((l) => l.startsWith("host 0.3.0"))).toBe(true);
  });

  it("serves the multiplayer API with the game codec", async () => {
    const res = await fetch(`http://127.0.0.1:${ports.http}/api/rtpvp2/practiceresult.json`, { method: "POST", body: encodeBody({ player_id: 21000000009, result: 1 }) });
    expect(decodeBody<{ status_code: number }>(await res.text()).status_code).toBe(200);
  });

  it("answers Photon with working encryption (pure-JS DH + AES inside the sandbox)", async () => {
    const c = new TestClient(ports.name);
    await c.connect();
    c.sendInit();
    await c.next("init", 0);
    c.exchangeKeys();
    expect((await c.next("internal", 0)).rc).toBe(0);
    c.op(230, new Map<number, PV>([[220, P.str("2.28.0")], [224, P.str("app")], [225, P.str("21000000009")]]), true);
    const auth = await c.next("response", 230);
    expect(auth.rc).toBe(0);
    expect(asString(auth.params!.get(230))).toMatch(/^127\.0\.0\.1:\d+$/);
    c.close();
  });
});
