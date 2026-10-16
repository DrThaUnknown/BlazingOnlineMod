// Entry point of the host bundle that runs inside JavaScriptCore on the host phone. The dylib
// (src/jsc.c) provides the global `__bo` with native bindings and calls back into the globals
// defined here. Binary data crosses the boundary as "binary strings" (one char per byte, 0-255).
import { Buffer } from "buffer";
import { setRandomSource } from "./shims/crypto.ts";
import { startHost, type Host } from "./host.ts";
import type { HttpRequest, HttpResponse, Platform } from "./platform.ts";

interface Native {
  log(msg: string): void;
  now(): number;
  random(n: number): string;
  setInterval(fn: () => void, ms: number): number;
  clearInterval(id: number): void;
  udpBind(port: number): number; // handle, or -1
  udpSend(handle: number, data: string, port: number, address: string): void;
  udpClose(handle: number): void;
  udpPort(handle: number): number;
  httpListen(port: number): number; // handle, or -1
  httpRespond(requestId: number, status: number, contentType: string, body: string): void;
  httpClose(handle: number): void;
  readFile(name: string): string | null;
  writeFile(name: string, data: string): void;
}

type G = typeof globalThis & {
  __bo: Native;
  __bo_onUdp?: (handle: number, data: string, address: string, port: number) => void;
  __bo_onHttp?: (handle: number, requestId: number, method: string, path: string, body: string) => void;
  __bo_start?: (publicHost: string) => void;
  __bo_stop?: () => void;
  __bo_status?: () => string;
};
const g = globalThis as G;

const udpHandlers = new Map<number, (msg: Buffer, address: string, port: number) => void>();
const httpHandlers = new Map<number, (req: HttpRequest) => Promise<HttpResponse>>();

g.__bo_onUdp = (handle, data, address, port) => udpHandlers.get(handle)?.(Buffer.from(data, "latin1"), address, port);
g.__bo_onHttp = (handle, requestId, method, path, body) => {
  const h = httpHandlers.get(handle);
  if (!h) return g.__bo.httpRespond(requestId, 404, "text/plain", "no handler");
  h({ method, path, body })
    .then((r) => g.__bo.httpRespond(requestId, r.status, r.contentType ?? "text/plain", r.body))
    .catch((e: Error) => g.__bo.httpRespond(requestId, 500, "text/plain", String(e?.message ?? e)));
};

export function nativePlatform(n: Native): Platform {
  setRandomSource((count) => Uint8Array.from(Buffer.from(n.random(count), "latin1")));
  return {
    async udpBind(port, onMessage) {
      const handle = n.udpBind(port);
      if (handle < 0) throw new Error(`cannot bind udp ${port}`);
      udpHandlers.set(handle, onMessage);
      return {
        port: n.udpPort(handle),
        socket: {
          send: (data, p, a) => n.udpSend(handle, Buffer.from(data).toString("latin1"), p, a),
          close: () => {
            udpHandlers.delete(handle);
            n.udpClose(handle);
          },
        },
      };
    },
    async httpListen(port, handler) {
      const handle = n.httpListen(port);
      if (handle < 0) throw new Error(`cannot listen on http ${port}`);
      httpHandlers.set(handle, handler);
      return {
        port,
        close: () => {
          httpHandlers.delete(handle);
          n.httpClose(handle);
        },
      };
    },
    setInterval: (fn, ms) => n.setInterval(fn, ms),
    clearInterval: (h) => n.clearInterval(h as number),
    now: () => n.now(),
    log: (m) => n.log(m),
    readFile: (name) => n.readFile(name),
    writeFile: (name, data) => n.writeFile(name, data),
  };
}

let running: Host | null = null;
let starting = false;

g.__bo_start = (_lanAddress: string) => {
  if (running || starting) return;
  starting = true;
  // Joiners reach the master/game servers through their own redirect (host.ts advertisedPortOffset).
  startHost(nativePlatform(g.__bo), { publicHost: "127.0.0.1", advertisedPortOffset: 10000 })
    .then((h) => {
      running = h;
    })
    .catch((e: Error) => g.__bo.log(`host failed to start: ${e?.message ?? e}`))
    .finally(() => {
      starting = false;
    });
};
g.__bo_stop = () => {
  running?.stop();
  running = null;
};
g.__bo_status = () =>
  running ? `hosting: ${Object.keys(running.state.players).length} players seen, http ${running.ports.http}` : starting ? "starting" : "stopped";
