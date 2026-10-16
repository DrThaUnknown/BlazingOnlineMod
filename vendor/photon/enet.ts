// Photon's ENet-style reliable UDP, server side. Layout and client behaviour traced from the
// 2.28.0 binary: docs/photon-client-trace.md "Transport facts".

export const CT = {
  Ack: 1,
  Connect: 2,
  VerifyConnect: 3,
  Disconnect: 4,
  Ping: 5,
  SendReliable: 6,
  SendUnreliable: 7,
  SendFragment: 8,
} as const;

const DATAGRAM_HEADER = 12;
const CMD_HEADER = 12;
const FRAGMENT_HEADER = 32;

interface ParsedCommand {
  type: number;
  channel: number;
  flags: number;
  seq: number;
  body: Buffer;
}

interface PendingReliable {
  bytes: Buffer;
  channel: number;
  seq: number;
  firstSent: number;
  lastSent: number;
  rto: number;
}

interface FragmentSet {
  count: number;
  total: number;
  got: number;
  data: Buffer;
}

export interface ConnectionEvents {
  /** A complete application message (Photon payload, starting with 0xF3). */
  onMessage(payload: Buffer, channel: number): void;
  onDisconnect(reason: string): void;
}

export class EnetConnection {
  readonly createdAt = Date.now();
  peerId = 0;
  challenge = 0;
  mtu = 1200;
  connected = false;
  closed = false;
  lastReceive = Date.now();

  private incomingNext = new Map<number, number>(); // channel -> next expected reliable seq
  private incomingBuffer = new Map<number, Map<number, ParsedCommand>>();
  private fragments = new Map<string, FragmentSet>();
  private outgoingSeq = new Map<number, number>();
  private pending = new Map<string, PendingReliable>();
  private queue: Buffer[] = []; // encoded commands waiting for the next flush

  private readonly send: (datagram: Buffer) => void;
  private readonly events: ConnectionEvents;
  private readonly log: (msg: string) => void;

  constructor(send: (datagram: Buffer) => void, events: ConnectionEvents, log: (msg: string) => void = () => {}) {
    this.send = send;
    this.events = events;
    this.log = log;
  }

  private now(): number {
    return (Date.now() - this.createdAt) | 0;
  }

  /** Handles one raw datagram from the client. */
  receive(msg: Buffer, assignPeerId: () => number): void {
    if (this.closed || msg.length < DATAGRAM_HEADER) return;
    this.lastReceive = Date.now();
    const count = msg[3]!;
    const sentTime = msg.readInt32BE(4);
    const challenge = msg.readInt32BE(8);
    let off = DATAGRAM_HEADER;
    for (let i = 0; i < count && off + CMD_HEADER <= msg.length; i++) {
      const len = msg.readInt32BE(off + 4);
      if (len < CMD_HEADER || off + len > msg.length) break;
      const cmd: ParsedCommand = {
        type: msg[off]!,
        channel: msg[off + 1]!,
        flags: msg[off + 2]!,
        seq: msg.readInt32BE(off + 8),
        body: Buffer.from(msg.subarray(off + CMD_HEADER, off + len)),
      };
      off += len;
      if (cmd.type === CT.Connect) {
        this.handleConnect(cmd, challenge, assignPeerId);
        continue;
      }
      if (cmd.flags & 1 && cmd.type !== CT.Ack) this.queueAck(cmd, sentTime);
      this.handleCommand(cmd);
    }
    this.flush();
  }

  private handleConnect(cmd: ParsedCommand, challenge: number, assignPeerId: () => number): void {
    if (!this.connected) {
      this.peerId = assignPeerId();
      this.challenge = challenge;
      if (cmd.body.length >= 4) this.mtu = Math.max(576, Math.min(1500, cmd.body.readUInt16BE(2)));
      this.connected = true;
      this.log(`connect peerId=${this.peerId} mtu=${this.mtu}`);
    }
    // Ack the reliable Connect, then VerifyConnect echoing the proposal with our peer id.
    this.queueAck(cmd, 0);
    const body = Buffer.from(cmd.body);
    if (body.length >= 2) body.writeUInt16BE(this.peerId, 0);
    this.queue.push(encodeCommand(CT.VerifyConnect, cmd.channel, 0, 0, body));
  }

  private queueAck(cmd: ParsedCommand, sentTime: number): void {
    const body = Buffer.alloc(8);
    body.writeInt32BE(cmd.seq, 0);
    body.writeInt32BE(sentTime, 4);
    this.queue.push(encodeCommand(CT.Ack, cmd.channel, 0, 0, body));
  }

  private handleCommand(cmd: ParsedCommand): void {
    switch (cmd.type) {
      case CT.Ack: {
        if (cmd.body.length >= 4) this.pending.delete(`${cmd.channel}:${cmd.body.readInt32BE(0)}`);
        return;
      }
      case CT.Disconnect:
        this.close("client disconnect");
        return;
      case CT.Ping:
        return;
      case CT.SendReliable:
      case CT.SendFragment:
        if (cmd.flags & 1) this.receiveReliable(cmd);
        else this.dispatch(cmd);
        return;
      case CT.SendUnreliable:
        this.dispatch(cmd);
        return;
      default:
        return;
    }
  }

  private receiveReliable(cmd: ParsedCommand): void {
    const next = this.incomingNext.get(cmd.channel) ?? 1;
    if (cmd.seq < next) return; // duplicate (retransmit of something we already handled)
    let buf = this.incomingBuffer.get(cmd.channel);
    if (!buf) this.incomingBuffer.set(cmd.channel, (buf = new Map()));
    buf.set(cmd.seq, cmd);
    let expected = next;
    while (buf.has(expected)) {
      const c = buf.get(expected)!;
      buf.delete(expected);
      expected++;
      this.incomingNext.set(cmd.channel, expected);
      this.dispatch(c);
    }
  }

  private dispatch(cmd: ParsedCommand): void {
    if (cmd.type === CT.SendUnreliable) {
      if (cmd.body.length >= 4) this.events.onMessage(cmd.body.subarray(4), cmd.channel);
      return;
    }
    if (cmd.type === CT.SendReliable) {
      this.events.onMessage(cmd.body, cmd.channel);
      return;
    }
    if (cmd.type === CT.SendFragment && cmd.body.length >= 20) {
      const start = cmd.body.readInt32BE(0);
      const count = cmd.body.readInt32BE(4);
      const total = cmd.body.readInt32BE(12);
      const offset = cmd.body.readInt32BE(16);
      const chunk = cmd.body.subarray(20);
      if (total <= 0 || total > 1 << 22 || offset < 0 || offset + chunk.length > total) return;
      const key = `${cmd.channel}:${start}`;
      let set = this.fragments.get(key);
      if (!set) this.fragments.set(key, (set = { count, total, got: 0, data: Buffer.alloc(total) }));
      chunk.copy(set.data, offset);
      set.got++;
      if (set.got >= set.count) {
        this.fragments.delete(key);
        this.events.onMessage(set.data, cmd.channel);
      }
    }
  }

  /** Queues an application message as reliable (fragmented if it does not fit the MTU). */
  sendReliable(payload: Buffer, channel = 0): void {
    if (this.closed) return;
    const maxSingle = this.mtu - DATAGRAM_HEADER - CMD_HEADER;
    if (payload.length <= maxSingle) {
      const seq = this.nextSeq(channel);
      this.queueReliable(encodeCommand(CT.SendReliable, channel, 1, seq, payload), channel, seq);
      return;
    }
    const chunkSize = this.mtu - DATAGRAM_HEADER - FRAGMENT_HEADER;
    const count = Math.ceil(payload.length / chunkSize);
    let start = -1;
    for (let i = 0; i < count; i++) {
      const seq = this.nextSeq(channel);
      if (start < 0) start = seq;
      const chunk = payload.subarray(i * chunkSize, (i + 1) * chunkSize);
      const fh = Buffer.alloc(20);
      fh.writeInt32BE(start, 0);
      fh.writeInt32BE(count, 4);
      fh.writeInt32BE(i, 8);
      fh.writeInt32BE(payload.length, 12);
      fh.writeInt32BE(i * chunkSize, 16);
      this.queueReliable(encodeCommand(CT.SendFragment, channel, 1, seq, Buffer.concat([fh, chunk])), channel, seq);
    }
  }

  private nextSeq(channel: number): number {
    const seq = (this.outgoingSeq.get(channel) ?? 0) + 1;
    this.outgoingSeq.set(channel, seq);
    return seq;
  }

  private queueReliable(bytes: Buffer, channel: number, seq: number): void {
    const now = Date.now();
    this.pending.set(`${channel}:${seq}`, { bytes, channel, seq, firstSent: now, lastSent: now, rto: 300 });
    this.queue.push(bytes);
  }

  /** Retransmits unacked reliable commands; drops the connection when the client is gone. */
  tick(now = Date.now()): void {
    if (this.closed) return;
    if (now - this.lastReceive > 20_000) {
      this.close("timeout");
      return;
    }
    for (const p of this.pending.values()) {
      if (now - p.firstSent > 15_000) {
        this.close("reliable command never acked");
        return;
      }
      if (now - p.lastSent >= p.rto) {
        p.lastSent = now;
        p.rto = Math.min(p.rto * 2, 2000);
        this.queue.push(p.bytes);
      }
    }
    this.flush();
  }

  /** Packs queued commands into datagrams no larger than the client's MTU. */
  flush(): void {
    if (!this.queue.length || this.closed) return;
    let batch: Buffer[] = [];
    let size = DATAGRAM_HEADER;
    const emit = () => {
      if (!batch.length) return;
      const header = Buffer.alloc(DATAGRAM_HEADER);
      header.writeUInt16BE(this.peerId, 0);
      header[2] = 0;
      header[3] = batch.length;
      header.writeInt32BE(this.now(), 4);
      header.writeInt32BE(this.challenge, 8);
      this.send(Buffer.concat([header, ...batch]));
      batch = [];
      size = DATAGRAM_HEADER;
    };
    for (const cmd of this.queue) {
      if (size + cmd.length > this.mtu || batch.length >= 255) emit();
      batch.push(cmd);
      size += cmd.length;
    }
    emit();
    this.queue = [];
  }

  /** Server-initiated disconnect. */
  disconnect(): void {
    if (this.closed) return;
    this.queue.push(encodeCommand(CT.Disconnect, 0xff, 0, 0, Buffer.alloc(0)));
    this.flush();
    this.close("server disconnect");
  }

  close(reason: string): void {
    if (this.closed) return;
    this.closed = true;
    this.pending.clear();
    this.log(`closed: ${reason}`);
    this.events.onDisconnect(reason);
  }
}

export function encodeCommand(type: number, channel: number, flags: number, seq: number, body: Buffer): Buffer {
  const h = Buffer.alloc(CMD_HEADER);
  h[0] = type;
  h[1] = channel;
  h[2] = flags;
  h[3] = 0;
  h.writeInt32BE(CMD_HEADER + body.length, 4);
  h.writeInt32BE(seq, 8);
  return Buffer.concat([h, body]);
}
