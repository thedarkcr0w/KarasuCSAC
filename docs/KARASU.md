# KarasuCSAC — platform integration

How this fork turns a CS2AC detection into a ban on the Karasu platform, what it
refuses to ban on, and how to turn it off in a hurry.

Read this before putting it on a server that real people play on.

## The short version

Upstream CS2AC punishes by running an admin-plugin command on the game server. That
is the wrong shape for Karasu: the authoritative ban is an account ban on the
platform, it has to outlive the container the match ran in, and it has to be
appealable. So this fork does three things instead:

1. Classifies each of the 17 detectors into an enforcement tier.
2. Requires corroboration before any statistical detector can produce a ban.
3. Relays the detection to the platform, which makes the real decision.

```
17 detectors ──▶ CS2ACPlugin::HandleDetection            (src/cs2ac.cpp)
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

## The tiers

Set in `src/karasu/karasu_policy.h`. The tier is compiled in — there is no config key
that promotes a detector, on purpose.

### Tier A — one detection is enough

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

### Tier B — needs corroboration

Very unlikely for a human, but measured from behaviour. One of these alone never bans.
It needs a second, still-live detection of a **different type** from a **different
family**, each clearing `cs2ac_karasu_min_confidence`.

`AIMLOCK` · `SILENTAIM` · `ANTIAIM` · `AIMBOT` · `NULLS` · `INVALID CVAR` · `BHOP` ·
`HYPERSCROLL` · `AUTOSTRAFE`

Families exist so that two detectors measuring the same underlying thing cannot
corroborate each other — bhop and autostrafe are both movement, so tripping both is
one unit, not two.

### Tier C — never bans

Real evidence, unacceptable false-positive profile. Recorded and relayed for staff
review; it can never contribute to a ban.

| Detector | Why it can never ban |
|---|---|
| `DLL INJECTION` | Fires on any one of 118 client event subscriptions, with no accumulation and no latch. The list includes `player_jump`, `door_open` and `gc_connected` — any legitimate overlay, or a Valve client change, would otherwise ban everyone who has it. Highest false-positive risk in the codebase. |
| `DOUBLETAP` | Its incident counter has no time window, so three fire-pairs spread across a whole map still trip it. |
| `INHUMAN ACCURACY` | A strong human on close-range SMG duels reaches the threshold, and the weapon list includes p90 and mac10. |
| `IRREGULAR BEHAVIOR` | Airborne and no-scope kills are legitimate, if rare, human plays. |
| `NAMECHANGER` | Detects annoyance, not cheating. Belongs in conduct moderation. |

Note this is a **demotion** from upstream, where every one of these except
`NAMECHANGER`'s siblings runs the permanent-ban command.

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
| `cs2ac_karasu_kick_command` | `kickid {userid} Karasu Anti-Cheat` | How a detected player is removed. |
| `cs2ac_karasu_min_confidence` | `72` | Floor a Tier B detection must clear to corroborate. |
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

## Known gaps

Be aware of these before trusting it completely.

- **No detector has fired through this code path yet.** The plugin builds clean
  (clang++, `-Wall -Werror`, C++17) and loads on a real CS2 dedicated server, and
  `cs2ac_karasu_test_report` exercises the policy lookup, corroboration ledger, JSON
  writer, base64url encoder, length budget and console emit. But a *genuine* detection
  needs a real client connected and playing — bots do not drive `ProcessUsercmds`, which
  is where most detectors get their input. Until that happens, the seam between a
  detector firing and `EvaluateKarasuPolicy` is reasoned about, not observed.
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
