// The host's session state: who connected, their PvP standing, matches in progress. Persisted as
// one JSON file through the platform so a host restart keeps the session leaderboard.
import type { Platform } from "./platform.ts";

export interface CharaLite {
  chara_id?: number;
  chara_level?: number;
  skill_level?: number;
  ability_count?: number;
  luck?: number;
  plus_s?: number;
  plus_a?: number;
  plus_d?: number;
  plus_sp?: number;
  exlimit_count?: number;
  grade?: number;
}

export interface HostPlayer {
  id: number;
  name: string;
  rank: number;
  team: CharaLite[];
  lastSeen: number;
  ladder: { points: number; win: number; loss: number; draw?: number; titlePt?: number; winStreak?: number; loseStreak?: number };
  practice: { win: number; loss: number; draw: number };
}

export interface Match {
  hash: string;
  host: number;
  guest: number;
  startedAt: number;
  practice: boolean;
}

export class HostState {
  players: Record<string, HostPlayer> = {};
  // By player id. Persisted: the host runs inside a phone's game, so a relaunch mid-battle must not
  // forget the match (both players' clients resend their result at the next login).
  matches = new Map<number, Match>();
  private dirty = false;
  private readonly platform: Platform;

  constructor(platform: Platform) {
    this.platform = platform;
    const raw = platform.readFile("host-state.json");
    if (raw) {
      try {
        const saved = JSON.parse(raw) as { players?: Record<string, HostPlayer>; matches?: Record<string, Match> };
        this.players = saved.players ?? {};
        for (const [id, m] of Object.entries(saved.matches ?? {})) this.matches.set(Number(id), m);
      } catch {
        platform.log("host: host-state.json unreadable, starting a fresh session");
      }
    }
  }

  player(id: number): HostPlayer {
    const key = String(id);
    return (this.players[key] ??= {
      id,
      name: "",
      rank: 1,
      team: [],
      lastSeen: this.platform.now(),
      ladder: { points: 0, win: 0, loss: 0 },
      practice: { win: 0, loss: 0, draw: 0 },
    });
  }

  touch(): void {
    this.dirty = true;
  }

  flush(): void {
    if (!this.dirty) return;
    this.dirty = false;
    this.platform.writeFile("host-state.json", JSON.stringify({ players: this.players, matches: Object.fromEntries(this.matches) }));
  }
}
