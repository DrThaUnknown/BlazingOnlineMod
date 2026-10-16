// The host's multiplayer HTTP API: the calls each phone's mod proxy forwards here (src/
// patch_impl.inc MULTIPLAYER). Bodies use the game's codec (vendor/shared/codec.ts). Players are the
// unique online ids the proxies assign; their profiles come from the Photon room (photon.ts).
// Missions, saves and currencies stay on each phone's own Rebirth engine, so no answer here carries
// u_status or other per-account state the host does not own.
import { decodeBody, encodeBody } from "../../vendor/shared/codec.ts";
import type { HttpRequest, HttpResponse } from "./platform.ts";
import type { HostPlayer, HostState } from "./state.ts";
import { titleFor } from "./titles.ts";

// INFERRED points per battle (the official values were computed on Bandai's servers; the client's
// rtpvpchampresultbonus table holds multipliers we have not decoded). Title points never drop;
// ranking points drop on a loss but never below 0. Titles themselves are real (titles.ts).
const POINTS = {
  win: { title: 100, ranking: 100 },
  draw: { title: 50, ranking: 50 },
  lose: { title: 0, ranking: -30 },
} as const;
const RTPVP2_ID = 1;
// Leagues 1-2 (F, E) are the client's beginner "Academy" (ExternalData.IsAcademyLeague @0xFD6694:
// leagueId < 3), which every new player started in. For a small group of friends that only gets in
// the way, so players start at the first title of league D (title 9, 6600 title points).
export const START_TITLE_PT = 6600;

/** Raises a record below the starting title (older sessions began at 0). */
function floorTitle(p: HostPlayer): void {
  if ((p.ladder.titlePt ?? 0) < START_TITLE_PT) p.ladder.titlePt = START_TITLE_PT;
}

class ApiError extends Error {
  readonly code: number;
  constructor(code: number, message: string) {
    super(message);
    this.code = code;
  }
}

type Body = Record<string, unknown>;
type Handler = (body: Body, me: HostPlayer, state: HostState, now: number, log: (m: string) => void) => object;

/** 1-based position on this host's ranking points (ties share the better place). */
function rankingOf(p: HostPlayer, state: HostState): number {
  return 1 + Object.values(state.players).filter((o) => o.ladder.points > p.ladder.points).length;
}

function ladderView(p: HostPlayer, state: HostState) {
  floorTitle(p);
  const l = p.ladder;
  return {
    [String(RTPVP2_ID)]: {
      rtpvp2_id: RTPVP2_ID,
      win_count: l.win,
      lose_count: l.loss,
      draw_count: l.draw ?? 0,
      wins_count: l.winStreak ?? 0,
      loses_count: l.loseStreak ?? 0,
      title_pt: l.titlePt ?? 0,
      title_id: titleFor(l.titlePt ?? 0).titleId,
      ranking_pt: l.points,
      ranking: rankingOf(p, state),
      total_win_count: l.win,
      final_count: 0,
      final_status: 0,
    },
  };
}

function playerData(p: HostPlayer) {
  const leader = p.team[0] ?? {};
  return {
    player_id: p.id,
    player_name: p.name,
    public_id: p.id,
    player_rank: p.rank,
    last_access_date: new Date(p.lastSeen).toISOString(),
    last_login_date: new Date(p.lastSeen).toISOString(),
    in_date: new Date(p.lastSeen).toISOString(),
    chara_id: leader.chara_id ?? 0,
    chara_level: leader.chara_level ?? 1,
    skill_level: leader.skill_level ?? 1,
    ability_count: leader.ability_count ?? 0,
    luck: leader.luck ?? 1,
    plus_s: leader.plus_s ?? 0,
    plus_a: leader.plus_a ?? 0,
    plus_d: leader.plus_d ?? 0,
    plus_sp: leader.plus_sp ?? 0,
    exlimit_count: leader.exlimit_count ?? 0,
    grade: leader.grade ?? 0,
    rtpvp2_ranking_pt: p.ladder.points,
    rtpvp2_title_id: 0,
  };
}

const empty = (): object => ({});
const rankingList = (): object => ({ ranking_list: {} });

const handlers: Record<string, Handler> = {
  "api/rtpvp2/top.json": (_b, me, state) => ({ u_rtpvp2: ladderView(me, state) }),
  "api/rtpvp2/toplight.json": (_b, me, state) => ({ u_rtpvp2_light: { [String(RTPVP2_ID)]: { rtpvp2_id: RTPVP2_ID, ranking: rankingOf(me, state), final_count: 0 } } }),
  "api/rtpvp2/matching.json": empty,
  "api/rtpvp2/checktitlereward.json": empty,
  "api/rtpvp2/logpast.json": empty,
  "api/rtpvp2/logcurrent.json": () => ({ log_list: {} }),
  "api/rtpvp2/trankingtotal.json": rankingList,
  "api/rtpvp2/trankingaround.json": rankingList,
  "api/rtpvp2/rrankingtotal.json": rankingList,
  "api/rtpvp2/rrankinglog.json": rankingList,

  // The host calls this once both players are in the room (the client pairs players itself).
  "api/rtpvp2/starthost.json": (b, me, state, now) => {
    const opponent = Number(b.opponent_player_id ?? 0);
    if (!opponent || opponent === me.id) throw new ApiError(400, `bad opponent ${opponent}`);
    const match = { hash: String(b.hash ?? ""), host: me.id, guest: opponent, startedAt: now, practice: false };
    state.matches.set(me.id, match);
    state.matches.set(opponent, match);
    state.touch();
    return {};
  },
  "api/rtpvp2/startguest.json": empty,
  "api/rtpvp2/result.json": (b, me, state, _now, log) => {
    const match = state.matches.get(me.id);
    // INFERRED: 1 win, 2 loss, anything else a draw (the practice results use the same codes).
    const r = Number(b.result);
    const kind = r === 1 ? "win" : r === 2 ? "lose" : "draw";
    // A result the host has no match for (a host from before 0.2.6 relaunched mid-battle, or a
    // replay) is still answered 200 with no points: the client resends an unanswered result at
    // every login and a refusal there blocks the login with "error in the requested data".
    const known = match && !match.practice && (!b.hash || !match.hash || String(b.hash) === match.hash);
    if (match) state.matches.delete(me.id);
    const gain = known ? POINTS[kind] : { title: 0, ranking: 0 };
    floorTitle(me);
    if (known) {
      const l = me.ladder;
      l.titlePt = (l.titlePt ?? 0) + gain.title;
      l.points = Math.max(0, l.points + gain.ranking);
      if (kind === "win") {
        l.win++;
        l.winStreak = (l.winStreak ?? 0) + 1;
        l.loseStreak = 0;
      } else if (kind === "lose") {
        l.loss++;
        l.loseStreak = (l.loseStreak ?? 0) + 1;
        l.winStreak = 0;
      } else {
        l.draw = (l.draw ?? 0) + 1;
      }
    } else {
      log(`api: result from ${me.id} (${kind}) has no match on this host; accepted without points`);
    }
    state.touch();
    // The result screen animates these breakdowns; the whole gain is reported as the base part.
    const info =
      kind === "win"
        ? { win_result_info: { title_pt: { cattack_b: gain.title, wins_b: 0, turn_b: 0, opponent_b: 0, camp_b: 0, luck_b: 0 }, ranking_pt: { cattack_b: gain.ranking, wins_b: 0, turn_b: 0, luck_b: 0 }, jewel_special: { league_b: 0, wins_b: 0, opponent_b: 0 } } }
        : kind === "draw"
          ? { draw_result_info: { title_pt: { cattack_b: gain.title, opponent_b: 0, camp_b: 0, luck_b: 0 }, ranking_pt: { cattack_b: gain.ranking, luck_b: 0 }, jewel_special: {} } }
          : { lose_result_info: { ranking_pt: {}, jewel_special: {} } };
    // u_status belongs to the player's own engine; their proxy adds it (src/patch_impl.inc).
    return { ...info, sa_correction_title_pt: 0, sa_correction_ranking_pt: 0, sa_correction_jewel_special: 0, u_rtpvp2: ladderView(me, state) };
  },

  // Training Grounds / friend battles: unranked, counted only.
  "api/rtpvp2/practicestarthost.json": empty,
  "api/rtpvp2/practicestartguest.json": empty,
  "api/rtpvp2/practiceresult.json": (b, me, state) => {
    const r = Number(b.result);
    if (r === 1) me.practice.win++;
    else if (r === 2) me.practice.loss++;
    else me.practice.draw++;
    state.touch();
    return {};
  },
  "api/rtpvp2/practicelogcurrent.json": (_b, me) => ({ win_count: me.practice.win, lose_count: me.practice.loss, draw_count: me.practice.draw, log_list: {} }),

  // Opponent profiles, learned from the Photon room (photon.ts) when players join.
  "api/player/detail.json": (b, _me, state) => {
    const ids = Array.isArray(b.player_id_list) ? b.player_id_list.map(Number) : [];
    const list: Record<string, object> = {};
    for (const id of ids) {
      const p = state.players[String(id)];
      if (p) list[String(id)] = playerData(p);
    }
    return { player_detail_list: list };
  },
};

/**
 * Local time with its UTC offset (e.g. 2026-10-13T21:30:00-04:00). Rebirth's engine dates in local
 * time and the game treats a change of day between responses as a day rollover (back to the title),
 * so a UTC date in the evening loops forever. Each joiner's proxy also replaces this with its own
 * engine's date; this is the fallback.
 */
export function localIso(d = new Date()): string {
  const pad = (n: number) => String(Math.floor(Math.abs(n))).padStart(2, "0");
  const off = -d.getTimezoneOffset();
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}` +
    `${off >= 0 ? "+" : "-"}${pad(off / 60)}:${pad(off % 60)}`
  );
}

function envelope(status: number, payload: object, message = "OK"): object {
  return {
    ...payload,
    date: localIso(),
    status_code: status,
    reason_phrase: message,
    error_message: status === 200 ? "" : message,
    main_revision: 0, // the joiner's proxy puts its own engine's revisions back
    assetbundle_revision: 0,
    achievement_id_list: [],
    achievement_badge_count: 0,
    panel_badge_count: 0,
  };
}

/** Strips whatever precedes `api/` in the request path (the game's base can carry a prefix). */
function apiPath(path: string): string {
  const i = path.indexOf("api/");
  const p = i >= 0 ? path.slice(i) : path;
  const q = p.indexOf("?");
  return q >= 0 ? p.slice(0, q) : p;
}

export function createApi(state: HostState, now: () => number, log: (m: string) => void) {
  return async (req: HttpRequest): Promise<HttpResponse> => {
    const path = apiPath(req.path);
    const reply = (status: number, payload: object, message?: string): HttpResponse => ({
      status: 200,
      body: encodeBody(envelope(status, payload, message)),
      contentType: "text/plain; charset=utf-8",
    });
    if (path.replace(/^\/+/, "") === "healthz") return { status: 200, body: "ok" };
    let body: Body;
    try {
      body = req.body.trim() ? decodeBody<Body>(req.body) : {};
    } catch {
      return reply(400, {}, "unreadable body");
    }
    const id = Number(body.player_id ?? 0);
    const handler = handlers[path];
    if (!handler) {
      log(`api: unhandled ${path} (answered empty)`);
      return reply(200, {});
    }
    if (!id) return reply(400, {}, "player_id missing");
    const me = state.player(id);
    me.lastSeen = now();
    try {
      const payload = handler(body, me, state, now(), log);
      log(`api: ${path} player ${id}`);
      return reply(200, payload);
    } catch (err) {
      const e = err as ApiError;
      log(`api: ${path} player ${id} refused: ${e.message}`);
      return reply(e.code ?? 500, {}, e.message);
    }
  };
}
