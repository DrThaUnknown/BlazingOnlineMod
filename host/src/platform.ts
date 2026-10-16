// What the host server needs from where it runs. Two implementations: node.ts (PC, tests) and the
// JavaScriptCore bindings the dylib provides on the host phone (src/jsc.c).
import type { Buffer } from "buffer";

export interface UdpSocket {
  send(data: Uint8Array, port: number, address: string): void;
  close(): void;
}

export interface HttpRequest {
  method: string;
  path: string;
  body: string;
}

export interface HttpResponse {
  status: number;
  body: string;
  contentType?: string;
}

export interface Platform {
  udpBind(port: number, onMessage: (msg: Buffer, address: string, port: number) => void): Promise<{ socket: UdpSocket; port: number }>;
  httpListen(port: number, handler: (req: HttpRequest) => Promise<HttpResponse>): Promise<{ port: number; close(): void }>;
  setInterval(fn: () => void, ms: number): unknown;
  clearInterval(handle: unknown): void;
  now(): number;
  log(msg: string): void;
  readFile(name: string): string | null;
  writeFile(name: string, data: string): void;
}
