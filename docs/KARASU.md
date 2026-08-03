# KarasuCSAC — platform integration

How this fork turns a CS2AC detection into a ban on the Karasu platform, what it
refuses to ban on, and how to turn it off in a hurry.

Read this before putting it on a server that real people play on.

## The short version

Upstream CS2AC punishes by running an admin-plugin command on the game server. That
is the wrong shape for Karasu: the authoritative ban is an account ban on the
platform, it has to outlive the container the match ran in, and it has to be
appealable. So this fork does three things instead:

1. Gives each of the 18 detectors a confidence reflecting how strong its firing bar is.
2. Bans on one detection above a configurable confidence, and requires a repeat or a
   corroborating detector below it — so every detector is bannable, but the weaker ones
   have to earn it.
3. Relays the detection to the platform, which makes the real decision.

```
18 detectors ──▶ CS2ACPlugin::HandleDetection            (src/cs2ac.cpp)
                        │
                        ├─▶ chat / centre alert / Discord webhook   [upstream, unchanged]
                        │
                        └─▶ EvaluateKarasuPolicy                    (src/cs2ac.cpp)
                                 │  tier lookup      (src/karasu/karasu_policy.h)
                                 │  corroboration    (src/karasu/karasu_verdict.cpp)
                                 │
                                 ├─▶ kickid <userid>            — removes them now
                                 └─▶ karasu_anticheat_report …  — console relay
                                              │
                                     Karasu CS2 plugin (CounterStrikeSharp)
                                              │  + match id, server id, report token, HMAC
                                              ▼
                                     POST /internal/match/:id/anticheat/detection
                                              │
                                     the platform decides, records and bans
```

## Why the plugin does not call the API itself

It has no business holding the credentials. `cfg/cs2ac.cfg` is deployed in the clear
to every game server, and a plugin that authenticates to the platform would have to
keep the shared secret there. The Karasu CS2 plugin already holds that secret, already
knows the match id and the per-match report token, and already registers the
`karasu_anticheat_report` command — so this fork hands the detection over and stays
out of it. That also keeps the platform's API contract out of an AGPL repository.

The practical consequence: **auto-ban only works on a Karasu match server.** On any
other server the detectors still run and still announce, but the relay is dropped and
nothing is banned.

## What decides a ban

**Confidence, not tier.** Every detector has a fixed confidence in
`src/karasu/karasu_policy.h` reflecting how strong its firing bar is, and three rules
decide the recommendation:

| Rule | Condition |
|---|---|
| `solo_confidence` | confidence ≥ `cs2ac_karasu_solo_ban_confidence` (default **55**) → bans on one detection |
| `corroborated` | a second detector, different type **and** different family, each ≥ `cs2ac_karasu_min_confidence` (72) |
| `repeat` | the same detector fires again, each time ≥ 85 |

Each repeat fire adds +5 confidence (capped 99), so **every detector reaches a ban
eventually** — NAMECHANGER, the weakest at 30, gets there on its sixth fire. Nothing is
permanently alert-only.

The confidences:

```
95 invalid input      93 desubticking, subtick spam   88 aimlock    86 silentaim
84 antiaim            82 aimbot                       80 nulls, invalid cvar
78 bhop, hyperscroll  76 autostrafe                   55 dll injection
50 doubletap          48 triggerbot                   45 inhuman accuracy,
45 irregular behavior 30 namechanger
```

At the default of 55, everything down to and including DLL INJECTION bans on a single
detection. Set the dial to 30 and every detector does; 76 and only aim/movement/protocol
do; 90 and only the three that are structurally impossible for a real client do.

> **The one to watch is DLL INJECTION.** It is the weakest detector that bans on its own
> at the default, and it fires on any *one* of 117 client event subscriptions with no
> accumulation and no latch — a list that includes `player_jump`, `door_open` and
> `gc_connected`. A legitimate overlay, or a Valve client change, can trip it. Raising
> `cs2ac_karasu_solo_ban_confidence` above 55 is the single change that stops it banning
> anyone by itself, and it needs no rebuild.

**Keep this in step with the platform.** `AnticheatAutoBanService.evaluateTierRules` in
the Karasu monorepo implements the same three rules over its durable cross-session
ledger, and it is the side that actually writes the ban. If you change a rule here and
not there, the plugin will recommend a ban the platform then refuses.

## The tiers

Tier no longer gates enforcement. It sets the severity reported to the platform
(A → `critical`, B → `high`, C → `medium`) and groups detectors into families so that two
detectors measuring the same underlying thing cannot corroborate each other.

### Tier A

The client sent something a conformant client cannot produce.

| Detector | Why |
|---|---|
| `INVALID INPUT` | Button transitions with no matching subtick record. The two fields cannot desync in a real client. |
| `DESUBTICKING` | ≥90% of subtick moves arriving with a timing of exactly zero. Real timings are continuous floats. |
| `SUBTICK SPAM` | Repeated identical `(button, when)` pairs carrying view-angle deltas — angles smuggled through button aliases. |

Upstream ships these three as *kick-only*. That is a legacy choice, not a statement
about confidence: they are the strongest signals in the plugin. They are also the most
fragile — if Valve changes the usercmd or subtick encoding, all three start firing on
honest players at once. That is what the platform's hourly circuit breaker is for.

### Tier B

Very unlikely for a human, but measured from behaviour.

`AIMLOCK` · `SILENTAIM` · `ANTIAIM` · `AIMBOT` · `NULLS` · `INVALID CVAR` · `BHOP` ·
`HYPERSCROLL` · `AUTOSTRAFE`

Families exist so that two detectors measuring the same underlying thing cannot
corroborate each other — bhop and autostrafe are both movement, so tripping both is
one unit, not two.

### Tier C

Real evidence with a worse false-positive profile. These carry the lowest confidences,
so at the default dial only DLL INJECTION (55) bans on its own; the rest need a repeat
or a corroborating detector first.

| Detector | Why its confidence is low |
|---|---|
| `DLL INJECTION` | Fires on any one of 117 client event subscriptions, with no accumulation and no latch. The list includes `player_jump`, `door_open` and `gc_connected` — any legitimate overlay, or a Valve client change, would otherwise ban everyone who has it. Highest false-positive risk in the codebase. |
| `DOUBLETAP` | Its incident counter has no time window, so three fire-pairs spread across a whole map still trip it. |
| `TRIGGERBOT` | Each sample is a damaging shot fired 0-2 ticks after the crosshair first touched an enemy, which is far below human reaction time — but the contact trace is not lag compensated, so steady latency skews the count, and pre-firing a held angle on a sound cue lands in the same bucket. Well plumbed (damage confirmed, smoke excluded, network vetoed, ordinary duels subtract) but softly measured. Needs three fires to reach the ban bar. |
| `INHUMAN ACCURACY` | A strong human on close-range SMG duels reaches the threshold, and the weapon list includes p90 and mac10. |
| `IRREGULAR BEHAVIOR` | Airborne and no-scope kills are legitimate, if rare, human plays. |
| `NAMECHANGER` | Detects annoyance, not cheating. Belongs in conduct moderation. |

Upstream runs its permanent-ban command for all of these on a single detection. Here
they still ban, but the weaker ones have to earn it by repeating or being corroborated.

## Confidence

Confidence here is a property of the detector, not of the individual event: every
CS2AC module already accumulates 3–15 internal incidents before it fires once, so
"it fired" *is* the signal. Each repeat fire in the same session adds 5, capped at 99.

This is deliberately coarser than a per-event score. The precise version would require
threading each module's raw evidence counter out through the announce callback, which
is a change across nine detector modules — worth doing, but not worth doing blind. See
"Known gaps".

The platform re-derives its own confidence across sessions from its durable ledger, and
its decision is the one that bans.

## Identity

`Player::GetSteamId64(bool validated)` ignored its argument upstream and fell back to
the controller's copy of the SteamID and then to the raw xuid. This fork implements the
argument: pass `true` and you get an authenticated SteamID or nothing.

The relay carries `identitySource`, and the platform refuses to ban on anything but
`authenticated`. Announcements and logging still use the permissive lookup, so nothing
upstream changes.

## Configuration

All keys live in `cfg/cs2ac.cfg`. None of them is a secret.

| Key | Default | Meaning |
|---|---|---|
| `cs2ac_karasu_relay` | `1` | Send detections to the Karasu plugin. |
| `cs2ac_karasu_relay_command` | `karasu_anticheat_report` | Command the Karasu plugin listens on. |
| `cs2ac_karasu_enforce` | `2` | `0` report only · `1` kick on this server · `2` kick and ask for an account ban. |
| `cs2ac_karasu_kick_command` | `kickid {userid} [KarasuAC] {detection}` | How a detected player is removed. |
| `cs2ac_karasu_solo_ban_confidence` | `55` | **The main dial.** Confidence at which one detection bans on its own. |
| `cs2ac_karasu_min_confidence` | `72` | Floor a detection must clear to corroborate. |
| `cs2ac_karasu_corroboration_window` | `1800` | Seconds a detection stays eligible to corroborate. |

`cs2ac_punishment_command` and `cs2ac_kick_command` are set to `""`. When
`cs2ac_karasu_enforce` is above 0 the Karasu path takes over enforcement entirely and
the upstream command path is skipped, so a detection can never be punished twice.
`cs2ac_check_config` warns if you leave either of them set.

Running a **surf, KZ or other movement server**? Turn the movement family off
(`cs2ac_bhop_enabled 0`, `cs2ac_autostrafe_enabled 0`, `cs2ac_hyperscroll_enabled 0`,
`cs2ac_nulls_enabled 0`) rather than whitelisting everyone.

## Turning it off

Three independent switches, fastest first:

1. **One server, immediately** — `rcon cs2ac_karasu_enforce 0`. Detections keep being
   reported; nobody gets removed.
2. **Whole fleet, immediately** — turn Dry run back on in Admin → Moderation
   (`shadowEvaluate`). The platform runs the entire decision path and records what it
   *would* have done, but writes no punishment. No server needs redeploying.
3. **Automatic** — the platform's hourly auto-ban cap. If the fleet issues more than
   `AC_AUTOBAN_HOURLY_CAP` auto-bans in an hour, it stops banning and pages staff. This
   is the defence against a game patch turning a Tier A detector into a mass-ban event.

`cs2ac_status` prints the enforcement level, the policy thresholds and the relay
counters.

## What the platform does with a relay

Roughly, and the authority is `apps/api/src/modules/moderation/anticheat-autoban.service.ts`
in the Karasu monorepo:

1. Record the strike — always, even Tier C. This is the durable, cross-session ledger
   the plugin cannot keep.
2. Drop duplicates on the relay's idempotency key.
3. Refuse to ban on an unauthenticated identity.
4. Refuse to ban staff.
5. Stop if the hourly circuit breaker has tripped.
6. Apply the tier rules over a 30-day window.
7. Honour the fleet-wide dry-run switch.
8. Refuse to report a ban that did not durably persist — never kick a player for a ban
   that evaporated.
9. Write a `TEMP_MATCH_BAN`, flag it for staff review, audit it, notify the player.

Bans are **temporary and flagged for review**, not permanent. A permanent ban stays a
human decision.

## The relay fits in 511 bytes, and that is the binding constraint

`IVEngineServer2::ServerCommand` queues a string that the engine later tokenises into a
`CCommand`, and `CCommand::MaxCommandLength()` is **511** (`COMMAND_MAX_LENGTH = 512`,
minus one, in `hl2sdk-cs2/public/tier1/convar.h`). Base64 costs 4/3. After the
`karasu_anticheat_report ` prefix that leaves roughly **350 bytes of JSON**.

That is not much, so the payload carries only what the platform cannot reconstruct:

```jsonc
{"steamId":"…","detectionType":"cs2ac:AIMLOCK","severity":"high","detectorTier":"B",
 "detectorFamily":"aim","confidence":88,"recommendedAction":"alert","action":"alert",
 "identitySource":"unauthenticated","idempotencyKey":"…","evidence":"…"}
```

Deliberately not sent: `source` and `pluginVersion` (the C# relay substitutes both when
absent), `playerName` and `userid` (it resolves the player from `steamId`),
`deterministic` (implied by tier A), and `rule` / `corroboratingUnits` /
`corroboratingType` / `serverTick` (diagnostics — the platform re-derives its own verdict
from its durable ledger and wins anyway).

`evidence` is the flexible field and gets trimmed to fit. **If you ever add a field here,
take the budget out of the evidence prose, not out of the others** — and check
`cs2ac_karasu_test_report`, which prints the JSON with its encoded size against the cap.

This was found the hard way: the first version of the payload was ~450 bytes with the
evidence already emptied, so the length guard dropped **100 % of reports** and auto-ban
would have been silently dead in production. The drop path now prints the actual sizes.

## What has been verified, and what has not

Verified on a real CS2 dedicated server (Metamod:Source 2, `de_dust2`):

- Builds clean under clang++, `-Wall -Werror`, C++17, via `./build-linux.sh`.
- Loads: `meta list` shows `KarasuCSAC (1.0.12)`.
- `cs2ac.cfg` parses, including every `cs2ac_karasu_*` key; `cs2ac_check_config` passes.
- The policy is correct end to end, observed via `cs2ac_karasu_test_report`:
  `INVALID INPUT` (95), `AIMLOCK` (88), `SILENTAIM` (86) and `DLL INJECTION` (55) all
  reach rule `solo_confidence` and recommend **ban** at the default dial of 55, while
  `DOUBLETAP` (50) and `NAMECHANGER` (30) sit at `below_threshold` and recommend
  **alert** on a first fire.
- Repeat escalation works: firing `NAMECHANGER` six times walked its confidence
  30 → 35 → 40 → 45 → 50 → 55 and flipped the recommendation to **ban** on the sixth,
  which is the proof that no detector is permanently alert-only.
- The relay emits, the JSON parses, `confidence` is a number, the idempotency key is 24
  hex and increments on a stable per-boot nonce, and the length budget trims evidence
  rather than dropping the report.

**Not verified: a genuine detector firing.** Most detectors read `ProcessUsercmds`, which
only runs for a real network client — bots use a different path. So the seam between a
detector firing and `EvaluateKarasuPolicy` is reasoned about, not observed, and no
end-to-end ban has been issued from a real detection. That needs a human connecting to a
server running both this and the Karasu CS2 plugin.

## Known gaps

Be aware of these before trusting it completely.
- **Confidence is per-detector, not per-event** (see above). Threading real evidence
  values out of the nine detector modules is the obvious next improvement.
- **The relay is fire-and-forget.** There is no acknowledgement from the Karasu plugin,
  so a relay lost in the console buffer is lost silently. `cs2ac_status` reports how
  many were emitted, dropped or truncated, but not how many arrived.
- **The relay is not authenticated.** Anything that can run a server console command
  can forge one. The Karasu plugin rejects client invocation and non-participants, and
  the platform re-validates that the subject is really in the match — but anyone with
  RCON on the box can already ban through the admin API, so this is defence in depth
  rather than a boundary.
- **Detections outside a Karasu match are alert-only** — warmup, scrims, and any
  non-Karasu server. The Karasu plugin drops a relay with no active match.

## Licence

AGPL-3.0, inherited from upstream CS2AC. Modified source must be published, including
to people who interact with a server running it over the network. Keep `LICENSE` and
`THIRD_PARTY_NOTICES.md` intact, and keep this fork's repository public.

All detection logic is [karola3vax](https://github.com/karola3vax)'s work.
