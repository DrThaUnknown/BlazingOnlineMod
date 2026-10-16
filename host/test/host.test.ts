// End to end: the Blazing Online host under Node, driven like two phones would drive it through
// their mod proxies (online player ids, the game's body codec) and Photon (realtime/test/client.ts).
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { decodeBody, encodeBody } from "../../vendor/shared/codec.ts";
import { P, asString, type PV } from "../../vendor/photon/protocol16.ts";
import { TestClient } from "../../vendor/test/client.ts";
import { startHost, type Host } from "../src/host.ts";
import { nodePlatform } from "../src/node.ts";

let host: Host;
let dir: string;
const open: TestClient[] = [];

beforeEach(async () => {
  dir = mkdtempSync(path.join(tmpdir(), "bo-host-"));
  host = await startHost(nodePlatform(dir, "127.0.0.1", true), { publicHost: "127.0.0.1", httpPort: 0, namePort: 0, masterPort: 0, gamePort: 0 });
});
afterEach(() => {
  for (const c of open.splice(0)) c.close();
  host.stop();
});

async function api(p: string, body: object): Promise<Record<string, unknown>> {
  const res = await fetch(`http://127.0.0.1:${host.ports.http}/${p}`, { method: "POST", body: encodeBody(body) });
  return decodeBody(await res.text());
}

async function handshake(port: number): Promise<TestClient> {
  const c = new TestClient(port);
  open.push(c);
  await c.connect();
  c.sendInit();
  await c.next("init", 0);
  c.exchangeKeys();
  await c.next("internal", 0);
  return c;
}

/** NameServer -> Master -> GameServer into `room`, with the actor properties the game sends. */
async function enterRoom(id: number, name: string, team: number[], room: string, create: boolean) {
  const ns = await handshake(host.ports.name);
  ns.op(230, new Map<number, PV>([[220, P.str("2.28.0")], [224, P.str("app")], [210, P.str("asia")], [225, P.str(String(id))]]), true);
  const auth = await ns.next("response", 230);
  const secret = asString(auth.params!.get(221))!;
  const master = await handshake(host.ports.master);
  master.op(230, new Map([[221, P.str(secret)]]));
  await master.next("response", 230);
  master.op(create ? 227 : 226, new Map<number, PV>([[255, P.str(room)]]));
  await master.next("response", create ? 227 : 226);
  const game = await handshake(host.ports.game);
  game.op(230, new Map([[221, P.str(secret)]]));
  await game.next("response", 230);
  const params = JSON.stringify({ seasonId: 101, playerID: id, playerName: name, playerRank: 30, teamCharacters: team.map((chara_id) => ({ chara_id, chara_level: 80 })) });
  game.op(create ? 227 : 226, new Map<number, PV>([[255, P.str(room)], [249, P.hashtable([[P.str("UserId"), P.str(String(id))], [P.str("ParamsJson"), P.str(params)]])]]));
  const entered = await game.next("response", create ? 227 : 226);
  expect(entered.rc).toBe(0);
  return game;
}

describe("Blazing Online host", () => {
  it("learns profiles from the room and runs a ranked match through the API", async () => {
    const a = 23732958904, b = 21000000001;
    const hostGame = await enterRoom(a, "Anthony", [10045, 10013], "Rm_test", true);
    await enterRoom(b, "Guest", [10014], "Rm_test", false);
    await hostGame.next("event", 255);
    await hostGame.next("event", 255);

    const detail = await api("api/player/detail.json", { player_id: b, player_id_list: [a] });
    const profile = (detail.player_detail_list as Record<string, { player_name: string; chara_id: number }>)[String(a)];
    expect(profile).toMatchObject({ player_name: "Anthony", chara_id: 10045 });

    expect((await api("api/rtpvp2/starthost.json", { player_id: a, opponent_player_id: b, hash: "h1" })).status_code).toBe(200);
    expect((await api("api/rtpvp2/startguest.json", { player_id: b })).status_code).toBe(200);
    const won = await api("api/rtpvp2/result.json", { player_id: a, hash: "h1", result: 1 });
    expect(won.status_code).toBe(200);
    expect(won.u_status).toBeUndefined(); // the host never owns account state (the joiner's proxy adds it)
    expect(won.win_result_info).toMatchObject({ title_pt: { cattack_b: 100 }, ranking_pt: { cattack_b: 100 } });
    expect((await api("api/rtpvp2/result.json", { player_id: b, hash: "h1", result: 2 })).status_code).toBe(200);
    // A repeat (or a result for a match the host never saw) is answered 200 but scores nothing: the
    // client resends an unanswered result at every login, and a refusal there blocks the login.
    const again = await api("api/rtpvp2/result.json", { player_id: a, hash: "h1", result: 1 });
    expect(again.status_code).toBe(200);
    expect(again.win_result_info).toMatchObject({ title_pt: { cattack_b: 0 }, ranking_pt: { cattack_b: 0 } });

    const top = await api("api/rtpvp2/top.json", { player_id: a });
    // Local time with an offset, never a UTC "Z" date (a UTC evening date loops the game's day change).
    expect(String(top.date)).toMatch(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d$/);
    // Players start at league D (title 9, 6600 points): leagues F/E are the client's Academy.
    expect((top.u_rtpvp2 as Record<string, object>)["1"]).toMatchObject({ win_count: 1, wins_count: 1, title_pt: 6700, title_id: 9, ranking_pt: 100, ranking: 1 });
    // Five more wins reach 7200 points, still title 9 until the next threshold (titles.ts).
    for (let i = 2; i <= 6; i++) {
      await api("api/rtpvp2/starthost.json", { player_id: a, opponent_player_id: b, hash: `h${i}` });
      await api("api/rtpvp2/result.json", { player_id: a, hash: `h${i}`, result: 1 });
    }
    const top2 = await api("api/rtpvp2/top.json", { player_id: a });
    expect((top2.u_rtpvp2 as Record<string, object>)["1"]).toMatchObject({ title_pt: 7200, wins_count: 6 });
    expect(((top2.u_rtpvp2 as Record<string, { title_id: number }>)["1"]!).title_id).toBeGreaterThanOrEqual(9);
    const light = await api("api/rtpvp2/toplight.json", { player_id: b });
    expect((light.u_rtpvp2_light as Record<string, object>)["1"]).toMatchObject({ ranking: 2 });
  });

  it("answers the menu's connection test", async () => {
    const res = await fetch(`http://127.0.0.1:${host.ports.http}/healthz`);
    expect(res.status).toBe(200);
    expect(await res.text()).toMatch(/^ok bo=[0-9]+\.[0-9]+\.[0-9]+ proto=[0-9]+$/);
  });

  it("counts practice battles and answers every PvP read the game makes", async () => {
    const id = 21000000002;
    expect((await api("api/rtpvp2/practiceresult.json", { player_id: id, result: 2, giveup_type: 1 })).status_code).toBe(200);
    expect(await api("api/rtpvp2/practicelogcurrent.json", { player_id: id })).toMatchObject({ status_code: 200, lose_count: 1 });
    for (const p of ["matching", "checktitlereward", "toplight", "logpast", "logcurrent", "trankingtotal", "trankingaround", "rrankingtotal", "rrankinglog"]) {
      expect((await api(`api/rtpvp2/${p}.json`, { player_id: id })).status_code).toBe(200);
    }
  });

  it("keeps the session leaderboard across a host restart", async () => {
    await api("api/rtpvp2/practiceresult.json", { player_id: 21000000003, result: 1 });
    host.stop(); // flushes host-state.json
    host = await startHost(nodePlatform(dir, "127.0.0.1", true), { publicHost: "127.0.0.1", httpPort: 0, namePort: 0, masterPort: 0, gamePort: 0 });
    expect(await api("api/rtpvp2/practicelogcurrent.json", { player_id: 21000000003 })).toMatchObject({ win_count: 1 });
  });

  it("keeps a match across a host restart so the result still counts", async () => {
    const a = 21000000011, b = 21000000012;
    await api("api/rtpvp2/starthost.json", { player_id: a, opponent_player_id: b, hash: "hr" });
    host.stop(); // the host phone's game is closed mid-battle
    host = await startHost(nodePlatform(dir, "127.0.0.1", true), { publicHost: "127.0.0.1", httpPort: 0, namePort: 0, masterPort: 0, gamePort: 0 });
    const res = await api("api/rtpvp2/result.json", { player_id: b, hash: "hr", result: 1 });
    expect(res.status_code).toBe(200);
    expect(((res.u_rtpvp2 as Record<string, { win_count: number }>)["1"]!).win_count).toBe(1);
  });
});
