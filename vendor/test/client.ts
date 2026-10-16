// A test-only Photon client that speaks exactly what the traced 2.28.0 client speaks
// (docs/photon-client-trace.md): ENet commands with the reliable flag, [F3][type] framing with no
// Protocol16 tags, DH generator 22 + AES-256-CBC (zero IV) for encrypted operations.
import crypto from "node:crypto";
import dgram from "node:dgram";
import { CT, encodeCommand } from "../photon/enet.ts";
import { minimalBigEndian, oakleyPrime768 } from "../photon/messages.ts";
import { Reader, Writer, readParamTable, readTyped, writeParamTable, type PV, type ParamTable } from "../photon/protocol16.ts";

/** The Connect datagram body captured from the phone (mtu 1200, 2 channels). */
const CONNECT_BODY = Buffer.from("000004b000008000000000020000000000000000000013880000000200000002", "hex");
/** The Init payload captured from the phone. */
export const INIT_NAMESERVER = Buffer.concat([Buffer.from("f30001060104000007", "hex"), Buffer.from("NameServer"), Buffer.alloc(22)]);

export interface Received {
  kind: "init" | "response" | "event" | "internal";
  code: number;
  rc?: number;
  params?: ParamTable;
}

export class TestClient {
  private sock = dgram.createSocket("udp4");
  private peerId = 0xffff;
  private challenge = crypto.randomInt(1, 0x7fffffff);
  private seq = new Map<number, number>();
  private start = Date.now();
  private inbox: Received[] = [];
  private waiters: (() => void)[] = [];
  private fragments = new Map<number, { count: number; got: number; data: Buffer }>();
  private nextIncoming = new Map<number, number>();
  private held = new Map<string, { type: number; body: Buffer }>();
  readonly rawDatagrams: Buffer[] = [];
  private dh = crypto.createDiffieHellman(oakleyPrime768(), Buffer.from([22]));
  key: Buffer | null = null;
  unackedReliable = 0;

  private port: number;
  private mtu: number;
  private host: string;

  constructor(port: number, mtu = 1200, host = "127.0.0.1") {
    this.port = port;
    this.mtu = mtu;
    this.host = host;
    this.dh.generateKeys();
    this.sock.on("message", (m) => this.onDatagram(m));
  }

  async connect(): Promise<void> {
    await new Promise<void>((r) => this.sock.bind(0, this.host === "127.0.0.1" ? "127.0.0.1" : "0.0.0.0", () => r()));
    const body = Buffer.from(CONNECT_BODY);
    body.writeUInt16BE(this.mtu, 2);
    this.sendCommands([encodeCommand(CT.Connect, 0xff, 1, this.nextSeq(0xff), body)]);
    await this.waitFor(() => this.peerId !== 0xffff);
  }

  private nextSeq(ch: number): number {
    const s = (this.seq.get(ch) ?? 0) + 1;
    this.seq.set(ch, s);
    return s;
  }

  private sendCommands(cmds: Buffer[]): void {
    const h = Buffer.alloc(12);
    h.writeUInt16BE(this.peerId, 0);
    h[3] = cmds.length;
    h.writeInt32BE(Date.now() - this.start, 4);
    h.writeInt32BE(this.challenge, 8);
    this.sock.send(Buffer.concat([h, ...cmds]), this.port, this.host);
  }

  sendRaw(payload: Buffer): void {
    this.sendCommands([encodeCommand(CT.SendReliable, 0, 1, this.nextSeq(0), payload)]);
  }

  sendInit(): void {
    this.sendRaw(INIT_NAMESERVER);
  }

  exchangeKeys(): void {
    const w = new Writer();
    w.u8(0xf3);
    w.u8(6);
    w.u8(0); // InitEncryption
    writeParamTable(w, new Map([[1, { t: "bytes", v: minimalBigEndian(this.dh.getPublicKey()) } as PV]]));
    this.sendRaw(w.done());
  }

  op(code: number, params: ParamTable, encrypt = false): void {
    const body = new Writer();
    body.u8(code);
    writeParamTable(body, params);
    let b = body.done();
    if (encrypt) {
      const c = crypto.createCipheriv("aes-256-cbc", this.key!, Buffer.alloc(16));
      b = Buffer.concat([c.update(b), c.final()]);
    }
    this.sendRaw(Buffer.concat([Buffer.from([0xf3, encrypt ? 0x82 : 0x02]), b]));
  }

  private onDatagram(m: Buffer): void {
    this.rawDatagrams.push(m);
    const count = m[3]!;
    const sentTime = m.readInt32BE(4);
    if (m.readInt32BE(8) !== this.challenge) throw new Error("server echoed the wrong challenge");
    if (m.length > this.mtu) throw new Error(`datagram ${m.length} exceeds mtu ${this.mtu}`);
    let off = 12;
    const acks: Buffer[] = [];
    for (let i = 0; i < count; i++) {
      const type = m[off]!;
      const ch = m[off + 1]!;
      const flags = m[off + 2]!;
      const len = m.readInt32BE(off + 4);
      const seq = m.readInt32BE(off + 8);
      const body = m.subarray(off + 12, off + len);
      off += len;
      if (type === CT.VerifyConnect) {
        this.peerId = body.readUInt16BE(0);
        continue;
      }
      if (type === CT.SendReliable || type === CT.SendFragment) {
        // Mirrors QueueIncomingCommand: a flags-0 command is never dispatched by the real client.
        if (!(flags & 1)) {
          this.unackedReliable++;
          continue;
        }
        const ack = Buffer.alloc(8);
        ack.writeInt32BE(seq, 0);
        ack.writeInt32BE(sentTime, 4);
        acks.push(encodeCommand(CT.Ack, ch, 0, 0, ack));
        const next = this.nextIncoming.get(ch) ?? 1;
        if (seq < next) continue;
        this.held.set(`${ch}:${seq}`, { type, body: Buffer.from(body) });
        let e = next;
        while (this.held.has(`${ch}:${e}`)) {
          const c = this.held.get(`${ch}:${e}`)!;
          this.held.delete(`${ch}:${e}`);
          e++;
          this.nextIncoming.set(ch, e);
          if (c.type === CT.SendReliable) this.onMessage(c.body);
          else this.onFragment(c.body);
        }
      }
    }
    if (acks.length) this.sendCommands(acks);
  }

  private onFragment(body: Buffer): void {
    const start = body.readInt32BE(0);
    const count = body.readInt32BE(4);
    const total = body.readInt32BE(12);
    const offset = body.readInt32BE(16);
    let f = this.fragments.get(start);
    if (!f) this.fragments.set(start, (f = { count, got: 0, data: Buffer.alloc(total) }));
    body.subarray(20).copy(f.data, offset);
    if (++f.got === f.count) {
      this.fragments.delete(start);
      this.onMessage(f.data);
    }
  }

  private onMessage(p: Buffer): void {
    if (p[0] !== 0xf3) throw new Error("missing 0xF3 magic");
    const type = p[1]! & 0x7f;
    const r = new Reader(p, 2);
    if (type === 1) this.inbox.push({ kind: "init", code: 0 });
    else if (type === 3 || type === 7) {
      const code = r.u8();
      const rc = r.i16();
      readTyped(r);
      const params = readParamTable(r);
      if (type === 7 && code === 0 && rc === 0) {
        const serverKey = params.get(1) as Extract<PV, { t: "bytes" }>;
        const secret = minimalBigEndian(this.dh.computeSecret(serverKey.v));
        this.key = crypto.createHash("sha256").update(secret).digest();
      }
      this.inbox.push({ kind: type === 3 ? "response" : "internal", code, rc, params });
    } else if (type === 4) this.inbox.push({ kind: "event", code: r.u8(), params: readParamTable(r) });
    for (const w of this.waiters.splice(0)) w();
  }

  async waitFor<T>(pred: () => T | undefined | false, ms = 2000): Promise<T> {
    const deadline = Date.now() + ms;
    for (;;) {
      const v = pred();
      if (v) return v;
      if (Date.now() > deadline) throw new Error("timed out waiting");
      await new Promise<void>((r) => {
        this.waiters.push(r);
        setTimeout(r, 20);
      });
    }
  }

  async next(kind: Received["kind"], code: number): Promise<Received> {
    return this.waitFor(() => {
      const i = this.inbox.findIndex((m) => m.kind === kind && m.code === code);
      return i >= 0 ? this.inbox.splice(i, 1)[0] : undefined;
    });
  }

  close(): void {
    this.sock.close();
  }
}
