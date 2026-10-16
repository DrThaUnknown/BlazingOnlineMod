// A small Photon LoadBalancing server (NameServer + Master + GameServer in one process) that
// behaves the way the 2.28.0 client's LoadBalancingClient expects. Every parameter code and type
// here is traced in docs/photon-client-trace.md. The server never interprets game data: it relays
// RaiseEvent payloads byte-for-byte and keeps room/actor properties.
import crypto from "node:crypto";
import { EnetConnection } from "./enet.ts";
import { MSG, PhotonCodes, PhotonCrypto, eventMessage, initResponse, operationResponse, parseMessage } from "./messages.ts";
import {
  P,
  asBool,
  asBytes,
  asEntries,
  asInts,
  asNumber,
  asString,
  asStrings,
  describe,
  keyId,
  type PV,
  type ParamTable,
} from "./protocol16.ts";

export type Role = "name" | "master" | "game";

export const Op = {
  GetRegions: 220,
  FindFriends: 222,
  JoinRandomGame: 225,
  JoinGame: 226,
  CreateGame: 227,
  LeaveLobby: 228,
  JoinLobby: 229,
  Authenticate: 230,
  ChangeGroups: 248,
  SetProperties: 252,
  RaiseEvent: 253,
  Leave: 254,
} as const;

export const Ev = {
  GameListUpdate: 229,
  GameList: 230,
  AppStats: 226,
  PropertiesChanged: 253,
  Leave: 254,
  Join: 255,
} as const;

export const Err = {
  Ok: 0,
  OperationNotAllowed: -3,
  InvalidOperation: -2,
  InvalidAuthentication: 0x7fff,
  GameIdAlreadyExists: 0x7ffe,
  GameFull: 0x7ffd,
  GameClosed: 0x7ffc,
  NoRandomMatchFound: 0x7ff8,
  GameDoesNotExist: 0x7ff6,
} as const;

// Game property keys (byte).
const GP = { MaxPlayers: 255, IsVisible: 254, IsOpen: 253, PlayerCount: 252, Removed: 251, PropsListedInLobby: 250 } as const;

/** Region codes answered by GetRegions; all point at our one master. */
const REGIONS = ["asia", "au", "cae", "eu", "in", "jp", "kr", "ru", "rue", "sa", "us", "usw"];

type Entries = Map<string, [PV, PV]>;

function toEntries(v: PV | undefined): Entries {
  const m: Entries = new Map();
  for (const [k, x] of asEntries(v) ?? []) m.set(keyId(k), [k, x]);
  return m;
}

function mergeEntries(target: Entries, changes: Entries, deleteOnNull: boolean): void {
  for (const [id, [k, v]] of changes) {
    if (deleteOnNull && v.t === "null" && k.t === "string") target.delete(id);
    else target.set(id, [k, v]);
  }
}

function hashtable(e: Entries): PV {
  return P.hashtable([...e.values()]);
}

interface Actor {
  nr: number;
  client: Client;
  props: Entries;
  groups: Set<number>;
}

interface CachedEvent {
  sender: number;
  code: number;
  payload: Buffer;
}

interface Room {
  name: string;
  lobby: string;
  props: Entries;
  actors: Map<number, Actor>;
  nextActor: number;
  createdAt: number;
  cache: CachedEvent[];
}

export interface Client {
  id: number;
  role: Role;
  conn: EnetConnection;
  crypto: PhotonCrypto | null;
  userId: string;
  authenticated: boolean;
  lobby: string | null;
  room: Room | null;
  actorNr: number;
}

export interface ActorJoinInfo {
  room: string;
  actorNr: number;
  userId: string;
  /** Actor properties keyed by keyId (e.g. "s:ParamsJson", which carries the player's PvP team). */
  props: Map<string, PV>;
}

export interface LoadBalancingOptions {
  masterAddress: string;
  gameAddress: string;
  log?: (msg: string) => void;
  /** Called whenever an actor enters a room on the game server (the mod's host learns profiles here). */
  onActorJoin?: (info: ActorJoinInfo) => void;
}

export class LoadBalancingServer {
  readonly rooms = new Map<string, Room>();
  readonly lobbies = new Map<string, Set<Client>>();
  readonly clients = new Set<Client>();
  private nextClientId = 1;
  private readonly log: (msg: string) => void;

  private readonly opts: LoadBalancingOptions;

  constructor(opts: LoadBalancingOptions) {
    this.opts = opts;
    this.log = opts.log ?? (() => {});
  }

  attach(role: Role, conn: EnetConnection): Client {
    const client: Client = {
      id: this.nextClientId++,
      role,
      conn,
      crypto: null,
      userId: "",
      authenticated: false,
      lobby: null,
      room: null,
      actorNr: 0,
    };
    this.clients.add(client);
    return client;
  }

  onMessage(client: Client, payload: Buffer): void {
    let msg;
    try {
      msg = parseMessage(payload, client.crypto?.key ? (b) => client.crypto!.decrypt(b) : null);
    } catch (err) {
      this.log(`#${client.id} ${client.role}: unreadable message (${(err as Error).message})`);
      return;
    }
    if (msg.kind === "init") {
      client.conn.sendReliable(initResponse());
      return;
    }
    if (msg.kind !== "op") return;
    if (msg.internal) {
      this.handleInternal(client, msg.code, msg.params);
      return;
    }
    this.log(`#${client.id} ${client.role} op ${msg.code}${msg.encrypted ? " (enc)" : ""} ${JSON.stringify(Object.fromEntries([...msg.params].map(([k, v]) => [k, describe(v)])))}`);
    try {
      this.handleOperation(client, msg.code, msg.params);
    } catch (err) {
      this.log(`#${client.id} op ${msg.code} failed: ${(err as Error).stack}`);
      this.respond(client, msg.code, Err.InvalidOperation);
    }
  }

  onDisconnect(client: Client): void {
    this.leaveLobby(client);
    if (client.room) this.removeActor(client, false);
    this.clients.delete(client);
  }

  /** Expires rooms that were reserved on the master but never entered on the game server. */
  tick(now = Date.now()): void {
    for (const room of this.rooms.values()) {
      if (room.actors.size === 0 && now - room.createdAt > 30_000) this.rooms.delete(room.name);
    }
  }

  // ---- plumbing ----

  private respond(client: Client, code: number, rc: number, params: ParamTable = new Map(), debug: string | null = null): void {
    client.conn.sendReliable(operationResponse(code, rc, params, false, debug));
  }

  private sendEvent(client: Client, code: number, params: ParamTable): void {
    client.conn.sendReliable(eventMessage(code, params));
  }

  private handleInternal(client: Client, code: number, params: ParamTable): void {
    if (code !== PhotonCodes.InitEncryption) return;
    const key = params.get(PhotonCodes.ClientKey);
    if (key?.t !== "bytes" || key.v.length === 0) {
      client.conn.sendReliable(operationResponse(code, Err.InvalidOperation, new Map(), true));
      return;
    }
    client.crypto = new PhotonCrypto();
    client.crypto.derive(key.v);
    client.conn.sendReliable(operationResponse(code, 0, new Map([[PhotonCodes.ServerKey, P.bytes(client.crypto.publicKey)]]), true));
    this.log(`#${client.id} ${client.role}: encryption established`);
  }

  private handleOperation(client: Client, code: number, params: ParamTable): void {
    switch (code) {
      case Op.Authenticate:
        return this.authenticate(client, params);
      case Op.GetRegions:
        return this.respond(
          client,
          code,
          0,
          new Map([
            [210, P.strings(REGIONS)],
            [230, P.strings(REGIONS.map(() => this.opts.masterAddress))],
          ]),
        );
      case Op.FindFriends:
        return this.findFriends(client, params);
      case Op.JoinLobby:
        return this.joinLobby(client, params);
      case Op.LeaveLobby:
        this.leaveLobby(client);
        return this.respond(client, code, 0);
      case Op.CreateGame:
      case Op.JoinGame:
      case Op.JoinRandomGame:
        return client.role === "game" ? this.enterOnGame(client, code, params) : this.enterOnMaster(client, code, params);
      case Op.RaiseEvent:
        return this.raiseEvent(client, params);
      case Op.SetProperties:
        return this.setProperties(client, params);
      case Op.ChangeGroups:
        return this.changeGroups(client, params);
      case Op.Leave:
        this.respond(client, code, 0);
        if (client.room) this.removeActor(client, true);
        return;
      default:
        this.log(`#${client.id}: unhandled operation ${code}`);
        return this.respond(client, code, Err.InvalidOperation, new Map(), `operation ${code} not supported`);
    }
  }

  // ---- authentication ----

  private authenticate(client: Client, params: ParamTable): void {
    const secret = asString(params.get(221));
    if (secret) client.userId = decodeSecret(secret) ?? client.userId;
    const userId = asString(params.get(225));
    if (userId) client.userId = userId;
    if (!client.userId) client.userId = `anon-${crypto.randomUUID()}`;
    client.authenticated = true;
    const out: ParamTable = new Map([[221, P.str(encodeSecret(client.userId))]]);
    if (client.role === "name") out.set(230, P.str(this.opts.masterAddress));
    this.respond(client, Op.Authenticate, 0, out);
    this.log(`#${client.id} ${client.role}: authenticated as ${client.userId}`);
    if (client.role === "master") this.sendAppStats(client);
  }

  private sendAppStats(client: Client): void {
    let inRooms = 0;
    for (const r of this.rooms.values()) inRooms += r.actors.size;
    const onMaster = [...this.clients].filter((c) => c.role === "master").length;
    this.sendEvent(
      client,
      Ev.AppStats,
      new Map([
        [229, P.int(inRooms)],
        [228, P.int([...this.rooms.values()].filter((r) => r.actors.size > 0).length)],
        [227, P.int(onMaster)],
      ]),
    );
  }

  private findFriends(client: Client, params: ParamTable): void {
    const ids = asStrings(params.get(1)) ?? [];
    const online: boolean[] = [];
    const rooms: string[] = [];
    for (const id of ids) {
      const c = [...this.clients].find((x) => x.userId === id);
      online.push(!!c);
      rooms.push(c?.room?.name ?? "");
    }
    this.respond(
      client,
      Op.FindFriends,
      0,
      new Map([
        [1, P.bools(online)],
        [2, P.strings(rooms)],
      ]),
    );
  }

  // ---- lobbies ----

  private lobbyKey(params: ParamTable): string {
    return `${asNumber(params.get(212)) ?? 0}|${asString(params.get(213)) ?? ""}`;
  }

  private joinLobby(client: Client, params: ParamTable): void {
    this.leaveLobby(client);
    const key = this.lobbyKey(params);
    client.lobby = key;
    let set = this.lobbies.get(key);
    if (!set) this.lobbies.set(key, (set = new Set()));
    set.add(client);
    this.respond(client, Op.JoinLobby, 0);
    const list: [PV, PV][] = [];
    for (const room of this.rooms.values()) {
      if (room.lobby === key && this.listed(room)) list.push([P.str(room.name), this.lobbyEntry(room)]);
    }
    this.sendEvent(client, Ev.GameList, new Map([[222, P.hashtable(list)]]));
    this.log(`#${client.id}: joined lobby "${key}" (${list.length} rooms listed)`);
  }

  private leaveLobby(client: Client): void {
    if (client.lobby) this.lobbies.get(client.lobby)?.delete(client);
    client.lobby = null;
  }

  private listed(room: Room): boolean {
    return room.actors.size > 0 && (asBool(room.props.get(`b:${GP.IsVisible}`)?.[1]) ?? true);
  }

  private lobbyEntry(room: Room): PV {
    const e: [PV, PV][] = [
      [P.byte(GP.MaxPlayers), P.byte(maxPlayers(room))],
      [P.byte(GP.IsOpen), P.bool(isOpen(room))],
      [P.byte(GP.PlayerCount), P.byte(room.actors.size)],
    ];
    const listed = new Set(asStrings(room.props.get(`b:${GP.PropsListedInLobby}`)?.[1]) ?? []);
    for (const [k, v] of room.props.values()) {
      if (k.t === "string" && listed.has(k.v)) e.push([k, v]);
    }
    return P.hashtable(e);
  }

  /** Pushes a room's current lobby entry (or its removal) to everyone in its lobby. */
  private roomChanged(room: Room, removed = false): void {
    const set = this.lobbies.get(room.lobby);
    if (!set?.size) return;
    const entry = removed || !this.listed(room) ? P.hashtable([[P.byte(GP.Removed), P.bool(true)]]) : this.lobbyEntry(room);
    const params: ParamTable = new Map([[222, P.hashtable([[P.str(room.name), entry]])]]);
    for (const c of set) this.sendEvent(c, Ev.GameListUpdate, params);
  }

  // ---- rooms on the master: reserve and point the client at the game server ----

  private newRoom(name: string, lobby: string, props: Entries): Room {
    const room: Room = { name, lobby, props, actors: new Map(), nextActor: 1, createdAt: Date.now(), cache: [] };
    this.rooms.set(name, room);
    return room;
  }

  private enterOnMaster(client: Client, code: number, params: ParamTable): void {
    let room: Room | undefined;
    if (code === Op.CreateGame) {
      const name = asString(params.get(255)) || crypto.randomUUID();
      if (this.rooms.has(name)) return this.respond(client, code, Err.GameIdAlreadyExists, new Map(), "game already exists");
      room = this.newRoom(name, client.lobby ?? this.lobbyKey(params), toEntries(params.get(248)));
    } else if (code === Op.JoinGame) {
      const name = asString(params.get(255)) ?? "";
      room = this.rooms.get(name);
      if (!room) {
        if (!truthy(params.get(215))) return this.respond(client, code, Err.GameDoesNotExist, new Map(), "game does not exist");
        room = this.newRoom(name, client.lobby ?? this.lobbyKey(params), toEntries(params.get(248)));
      } else {
        const rc = joinBlocked(room);
        if (rc) return this.respond(client, code, rc, new Map(), "game full or closed");
      }
    } else {
      room = this.findRandom(client.lobby ?? this.lobbyKey(params), toEntries(params.get(248)));
      if (!room) return this.respond(client, code, Err.NoRandomMatchFound, new Map(), "no match found");
    }
    this.respond(
      client,
      code,
      0,
      new Map([
        [230, P.str(this.opts.gameAddress)],
        [255, P.str(room.name)],
      ]),
    );
  }

  private findRandom(lobby: string, expected: Entries): Room | undefined {
    const wantMax = asNumber(expected.get(`b:${GP.MaxPlayers}`)?.[1]) ?? 0;
    for (const room of this.rooms.values()) {
      if (room.lobby !== lobby || !this.listed(room) || joinBlocked(room)) continue;
      if (wantMax && maxPlayers(room) !== wantMax) continue;
      let ok = true;
      for (const [id, [k, v]] of expected) {
        if (k.t !== "string") continue;
        const have = room.props.get(id)?.[1];
        if (!have || JSON.stringify(describe(have)) !== JSON.stringify(describe(v))) ok = false;
      }
      if (ok) return room;
    }
    return undefined;
  }

  // ---- rooms on the game server ----

  private enterOnGame(client: Client, code: number, params: ParamTable): void {
    if (client.room) return this.respond(client, code, Err.OperationNotAllowed, new Map(), "already in a room");
    const name = asString(params.get(255)) ?? "";
    let room = this.rooms.get(name);
    if (code === Op.CreateGame) {
      if (room && room.actors.size > 0) return this.respond(client, code, Err.GameIdAlreadyExists, new Map(), "game already exists");
      if (!room) room = this.newRoom(name || crypto.randomUUID(), this.lobbyKey(params), new Map());
      mergeEntries(room.props, toEntries(params.get(248)), false);
    } else {
      if (!room) {
        if (!truthy(params.get(215))) return this.respond(client, code, Err.GameDoesNotExist, new Map(), "game does not exist");
        room = this.newRoom(name, this.lobbyKey(params), toEntries(params.get(248)));
      }
      const rc = joinBlocked(room);
      if (rc) return this.respond(client, code, rc, new Map(), "game full or closed");
    }
    this.addActor(client, room, code, toEntries(params.get(249)));
  }

  private addActor(client: Client, room: Room, code: number, props: Entries): void {
    const nr = room.nextActor++;
    const actor: Actor = { nr, client, props, groups: new Set() };
    room.actors.set(nr, actor);
    client.room = room;
    client.actorNr = nr;
    const actorList = P.ints([...room.actors.keys()]);
    const allProps = P.hashtable([...room.actors.values()].map((a) => [P.int(a.nr), hashtable(a.props)] as [PV, PV]));
    this.respond(
      client,
      code,
      0,
      new Map([
        [254, P.int(nr)],
        [249, allProps],
        [248, hashtable(room.props)],
        [252, actorList],
      ]),
    );
    const join: ParamTable = new Map([
      [254, P.int(nr)],
      [249, hashtable(props)],
      [252, actorList],
    ]);
    for (const a of room.actors.values()) this.sendEvent(a.client, Ev.Join, join);
    for (const ev of room.cache) client.conn.sendReliable(ev.payload);
    this.log(`#${client.id}: entered room "${room.name}" as actor ${nr} (${room.actors.size} in room)`);
    if (this.opts.onActorJoin) {
      const flat = new Map<string, PV>([...props].map(([id, [, v]]) => [id, v]));
      try {
        this.opts.onActorJoin({ room: room.name, actorNr: nr, userId: client.userId, props: flat });
      } catch (err) {
        this.log(`onActorJoin failed: ${(err as Error).message}`);
      }
    }
    this.roomChanged(room);
  }

  private removeActor(client: Client, voluntary: boolean): void {
    const room = client.room!;
    const nr = client.actorNr;
    room.actors.delete(nr);
    client.room = null;
    client.actorNr = 0;
    room.cache = room.cache.filter((e) => e.sender !== nr);
    this.log(`#${client.id}: left room "${room.name}" (${voluntary ? "leave" : "disconnect"}), ${room.actors.size} remain`);
    const leave: ParamTable = new Map([
      [254, P.int(nr)],
      [252, P.ints([...room.actors.keys()])],
    ]);
    for (const a of room.actors.values()) this.sendEvent(a.client, Ev.Leave, leave);
    if (room.actors.size === 0) {
      this.rooms.delete(room.name);
      this.roomChanged(room, true);
    } else {
      this.roomChanged(room);
    }
  }

  private raiseEvent(client: Client, params: ParamTable): void {
    const room = client.room;
    if (!room) return;
    const code = asNumber(params.get(244)) ?? 0;
    const data = params.get(245);
    const out: ParamTable = new Map();
    if (data) out.set(245, data);
    out.set(254, P.int(client.actorNr));
    const payload = eventMessage(code, out);

    const cache = asNumber(params.get(247)) ?? 0;
    if (cache === 4 || cache === 5 || cache === 1 || cache === 2) {
      if (cache === 2) room.cache = room.cache.filter((e) => !(e.sender === client.actorNr && e.code === code));
      room.cache.push({ sender: cache === 5 ? 0 : client.actorNr, code, payload });
    } else if (cache === 6 || cache === 3) {
      room.cache = room.cache.filter((e) => !(e.code === code && (e.sender === client.actorNr || cache === 6)));
      return;
    } else if (cache === 7) {
      room.cache = room.cache.filter((e) => e.sender === 0 || room.actors.has(e.sender));
      return;
    }

    const targets = asInts(params.get(252));
    const group = asNumber(params.get(240)) ?? 0;
    const receivers = asNumber(params.get(246)) ?? 0;
    let recipients: Actor[];
    if (targets) {
      recipients = targets.map((t) => room.actors.get(t)).filter((a): a is Actor => !!a);
    } else if (receivers === 2) {
      const master = Math.min(...room.actors.keys());
      recipients = [room.actors.get(master)!].filter(Boolean);
    } else {
      recipients = [...room.actors.values()].filter((a) => receivers === 1 || a.nr !== client.actorNr);
    }
    if (group) recipients = recipients.filter((a) => a.groups.has(group));
    for (const a of recipients) a.client.conn.sendReliable(payload);
  }

  private setProperties(client: Client, params: ParamTable): void {
    const room = client.room;
    if (!room) return this.respond(client, Op.SetProperties, Err.OperationNotAllowed);
    const changes = toEntries(params.get(251));
    const target = asNumber(params.get(254)) ?? 0;
    if (target > 0) {
      const actor = room.actors.get(target);
      if (!actor) return this.respond(client, Op.SetProperties, Err.InvalidOperation, new Map(), "no such actor");
      mergeEntries(actor.props, changes, true);
    } else {
      mergeEntries(room.props, changes, true);
    }
    this.respond(client, Op.SetProperties, 0);
    if (asBool(params.get(250)) ?? true) {
      const ev: ParamTable = new Map([
        [253, P.int(target)],
        [251, hashtable(changes)],
        [254, P.int(client.actorNr)],
      ]);
      for (const a of room.actors.values()) if (a.nr !== client.actorNr) this.sendEvent(a.client, Ev.PropertiesChanged, ev);
    }
    if (target === 0) this.roomChanged(room);
  }

  private changeGroups(client: Client, params: ParamTable): void {
    const actor = client.room?.actors.get(client.actorNr);
    if (!actor) return;
    const remove = params.get(239);
    if (remove) {
      const r = asBytes(remove) ?? [];
      if (r.length === 0) actor.groups.clear();
      for (const g of r) actor.groups.delete(g);
    }
    for (const g of asBytes(params.get(238)) ?? []) actor.groups.add(g);
  }
}

function maxPlayers(room: Room): number {
  return asNumber(room.props.get(`b:${GP.MaxPlayers}`)?.[1]) ?? 0;
}

function isOpen(room: Room): boolean {
  return asBool(room.props.get(`b:${GP.IsOpen}`)?.[1]) ?? true;
}

function joinBlocked(room: Room): number {
  if (!isOpen(room)) return Err.GameClosed;
  const max = maxPlayers(room);
  if (max > 0 && room.actors.size >= max) return Err.GameFull;
  return 0;
}

function truthy(v: PV | undefined): boolean {
  if (!v) return false;
  if (v.t === "bool") return v.v;
  return (asNumber(v) ?? 0) !== 0;
}

function encodeSecret(userId: string): string {
  return Buffer.from(JSON.stringify({ u: userId }), "utf8").toString("base64");
}

function decodeSecret(secret: string): string | undefined {
  try {
    const v = JSON.parse(Buffer.from(secret, "base64").toString("utf8")) as { u?: unknown };
    return typeof v.u === "string" ? v.u : undefined;
  } catch {
    return undefined;
  }
}

export { MSG };
