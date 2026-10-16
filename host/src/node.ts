// Runs the host on a PC (development and tests): node host/src/node.ts [publicHost]
import dgram from "node:dgram";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { pathToFileURL } from "node:url";
import type { Platform } from "./platform.ts";
import { startHost } from "./host.ts";

export function nodePlatform(dataDir: string, bindHost = "0.0.0.0", quiet = false): Platform {
  fs.mkdirSync(dataDir, { recursive: true });
  return {
    udpBind: (port, onMessage) =>
      new Promise((resolve, reject) => {
        const s = dgram.createSocket("udp4");
        s.on("message", (msg, r) => onMessage(msg as never, r.address, r.port));
        s.once("error", reject);
        s.bind(port, bindHost, () => resolve({ port: s.address().port, socket: { send: (d, p, a) => s.send(d, p, a), close: () => s.close() } }));
      }),
    httpListen: (port, handler) =>
      new Promise((resolve) => {
        const server = http.createServer((req, res) => {
          let body = "";
          req.on("data", (c) => (body += c));
          req.on("end", async () => {
            const out = await handler({ method: req.method ?? "GET", path: req.url ?? "/", body });
            res.writeHead(out.status, { "content-type": out.contentType ?? "text/plain" }).end(out.body);
          });
        });
        server.listen(port, bindHost, () => {
          const a = server.address();
          resolve({ port: typeof a === "object" && a ? a.port : port, close: () => server.close() });
        });
      }),
    setInterval: (fn, ms) => setInterval(fn, ms),
    clearInterval: (h) => clearInterval(h as NodeJS.Timeout),
    now: () => Date.now(),
    log: (m) => {
      if (!quiet) console.log(`[host] ${m}`);
    },
    readFile: (name) => {
      try {
        return fs.readFileSync(path.join(dataDir, name), "utf8");
      } catch {
        return null;
      }
    },
    writeFile: (name, data) => fs.writeFileSync(path.join(dataDir, name), data),
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const publicHost = process.argv[2] ?? "127.0.0.1";
  void startHost(nodePlatform(process.env.HOST_DATA ?? "build/host-data"), { publicHost });
}
