// The Blazing Online host: the Photon NameServer/Master/GameServer (realtime/src/photon) and the
// multiplayer HTTP API (api.ts), on whatever Platform it is given (node.ts on a PC, the dylib's
// JavaScriptCore bindings on the host phone).
import { EnetConnection } from "../../vendor/photon/enet.ts";
import { LoadBalancingServer, type ActorJoinInfo, type Client, type Role } from "../../vendor/photon/loadbalancing.ts";
import { asString } from "../../vendor/photon/protocol16.ts";
import { createApi } from "./api.ts";
import type { Platform, UdpSocket } from "./platform.ts";
import { HostState, type CharaLite } from "./state.ts";

export const HOST_VERSION = "0.3.0";

export interface HostOptions {
  /** The address joiners use to reach this host (handed out as the Photon master/game address). */
  publicHost: string;
  /**
   * Added to the master/game ports handed out. On a phone the host hands out 127.0.0.1 with +10000
   * (15055/15056): every joiner's mod redirects those loopback ports to the host address that
   * player typed (src/net.c), so the same host works over Wi-Fi, a VPN such as Tailscale, or a
   * forwarded router port, without knowing which address each player uses.
   */
  advertisedPortOffset?: number;
  httpPort?: number;
  namePort?: number;
  masterPort?: number;
  gamePort?: number;
  bindHost?: string;
}

export interface Host {
  ports: { http: number; name: number; master: number; game: number };
  state: HostState;
  lb: LoadBalancingServer;
  stop(): void;
}

/** Records who joined a PvP room: name, rank and team come from the actor's ParamsJson. */
function learnProfile(state: HostState, info: ActorJoinInfo, log: (m: string) => void): void {
  const raw = asString(info.props.get("s:ParamsJson"));
  if (!raw) return;
  try {
    const p = JSON.parse(raw) as { playerID?: number; playerName?: string; playerRank?: number; teamCharacters?: CharaLite[] };
    const id = Number(p.playerID ?? info.userId);
    if (!id) return;
    const player = state.player(id);
    player.name = String(p.playerName ?? player.name);
    player.rank = Number(p.playerRank ?? player.rank);
    if (Array.isArray(p.teamCharacters)) player.team = p.teamCharacters;
    state.touch();
    log(`host: ${player.name || id} joined room ${info.room} with ${player.team.length} characters`);
  } catch {
    log(`host: unreadable ParamsJson from ${info.userId}`);
  }
}

export async function startHost(platform: Platform, opts: HostOptions): Promise<Host> {
  const log = (m: string) => platform.log(m);
  const state = new HostState(platform);
  const conns = new Map<string, { conn: EnetConnection; client: Client }>();
  let nextPeerId = 1;
  let lb: LoadBalancingServer | undefined;

  const sockets: Partial<Record<Role, UdpSocket>> = {};
  const ports = { http: 0, name: 0, master: 0, game: 0 };
  const bind = async (role: Role, port: number) => {
    const { socket, port: bound } = await platform.udpBind(port, (msg, address, rport) => {
      if (!lb) return; // a datagram during startup, before the Photon server exists
      const key = `${role}|${address}:${rport}`;
      let entry = conns.get(key);
      if (entry?.conn.closed) {
        conns.delete(key);
        entry = undefined;
      }
      if (!entry) {
        if (msg.length < 13 || msg[12] !== 2) return; // only a Connect starts a connection
        let client: Client;
        const conn = new EnetConnection(
          (buf) => socket.send(buf, rport, address),
          {
            onMessage: (payload) => lb!.onMessage(client, payload),
            onDisconnect: () => {
              lb!.onDisconnect(client);
              conns.delete(key);
            },
          },
          (m) => log(`${role} ${address}:${rport}: ${m}`),
        );
        client = lb!.attach(role, conn);
        entry = { conn, client };
        conns.set(key, entry);
      }
      try {
        entry.conn.receive(msg, () => (nextPeerId = (nextPeerId % 0x7ffe) + 1));
      } catch (err) {
        log(`${key}: dropping malformed datagram: ${(err as Error).message}`);
      }
    });
    sockets[role] = socket;
    return bound;
  };
  ports.name = await bind("name", opts.namePort ?? 5058);
  ports.master = await bind("master", opts.masterPort ?? 5055);
  ports.game = await bind("game", opts.gamePort ?? 5056);

  const server = new LoadBalancingServer({
    masterAddress: `${opts.publicHost}:${ports.master + (opts.advertisedPortOffset ?? 0)}`,
    gameAddress: `${opts.publicHost}:${ports.game + (opts.advertisedPortOffset ?? 0)}`,
    log,
    onActorJoin: (info) => learnProfile(state, info, log),
  });
  lb = server;

  const http = await platform.httpListen(opts.httpPort ?? 8180, createApi(state, () => platform.now(), log));
  ports.http = http.port;

  const tick = platform.setInterval(() => {
    for (const { conn } of conns.values()) conn.tick();
    server.tick();
  }, 50);
  const save = platform.setInterval(() => state.flush(), 5000);

  log(`host ${HOST_VERSION}: http ${ports.http}, photon ${ports.name}/${ports.master}/${ports.game}, public ${opts.publicHost}`);
  return {
    ports,
    state,
    lb: server,
    stop() {
      platform.clearInterval(tick);
      platform.clearInterval(save);
      state.flush();
      http.close();
      for (const s of Object.values(sockets)) s?.close();
    },
  };
}
