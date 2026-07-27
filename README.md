> ## KarasuCSAC — a fork of [CS2AC](https://github.com/karola3vax/CS2AC)
>
> This is **not** the upstream project. It is Karasu's fork of `karola3vax/CS2AC`,
> modified so that a detection can result in an automatic ban on the
> [Karasu](https://karasu.live) platform rather than only a server-local punishment
> command.
>
> **All detection logic is upstream's work, and all credit for it belongs to
> [karola3vax](https://github.com/karola3vax).** CS2AC is licensed AGPL-3.0 and so is
> this fork; see [LICENSE](LICENSE) and [Third-party notices](THIRD_PARTY_NOTICES.md).
>
> What this fork changes, and nothing else:
>
> - Adds `src/karasu/` — a per-detector enforcement tier table, a corroboration
>   ledger, and a console relay that hands detections to the Karasu CS2 plugin.
> - Classifies all 17 detectors into three tiers so that a single noisy heuristic
>   cannot ban a legitimate player. Five detectors can **never** produce a ban.
> - Replaces the admin-plugin punishment command with an engine `kickid`, because on
>   Karasu the authoritative ban is the platform account ban, not a server-local one.
> - Implements the previously-ignored `validated` argument of `Player::GetSteamId64`,
>   so an unauthenticated SteamID can never be the basis of a ban.
>
> Read [docs/KARASU.md](docs/KARASU.md) before deploying it. Upstream's own
> documentation follows below and still describes how the detectors work.
>
> If you run a community server and just want an anti-cheat, use
> **[upstream CS2AC](https://github.com/karola3vax/CS2AC)** — this fork is wired to
> one specific platform and is less useful to you.

<div align="center">

<img src="docs/cs2ac-logo.png" width="760" alt="CS2AC">

### Open-source server-side anti-cheat for Counter-Strike 2.

[![Build](https://img.shields.io/github/actions/workflow/status/karola3vax/CS2AC/build.yml?branch=main&style=for-the-badge&label=build)](https://github.com/karola3vax/CS2AC/actions/workflows/build.yml)
[![Version](https://img.shields.io/badge/version-1.0.3-blue?style=for-the-badge)](https://github.com/karola3vax/CS2AC)
[![Detections](https://img.shields.io/badge/detections-17-red?style=for-the-badge)](#the-seventeen-detection-modules)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-5c7cfa?style=for-the-badge)](#quickstart)
[![License](https://img.shields.io/badge/license-AGPL--3.0-2ea44f?style=for-the-badge)](LICENSE)

**Counter-Strike is at its best when every shot, clutch, and win is earned.**

CS2AC helps community servers keep it that way.

[Install](#quickstart) · [See every detection](#the-seventeen-detection-modules) · [Pair it with CS2FOW](#want-wallhack-protection-too)

</div>

## See it catch

<table>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/aimbot.gif" width="100%" alt="CS2AC detecting a blatant snap-hit aimbot">
<br><strong>AIMBOT</strong><br>
<sub>A blatant snap lands on target.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/aimlock.gif" width="100%" alt="CS2AC detecting inhumanly precise target tracking">
<br><strong>AIMLOCK</strong><br>
<sub>The crosshair follows a moving target with inhuman precision.</sub>
</td>
</tr>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/antiaim.gif" width="100%" alt="CS2AC detecting impossible anti-aim angles">
<br><strong>ANTIAIM</strong><br>
<sub>Impossible angles, attack-return, jitter, and spin patterns.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/bhop.gif" width="100%" alt="CS2AC detecting automated bunny hopping">
<br><strong>BHOP</strong><br>
<sub>Repeated frame-perfect hops and machine-like jump patterns.</sub>
</td>
</tr>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/irregular-behavior.gif" width="100%" alt="CS2AC detecting repeated irregular airborne and no-scope results">
<br><strong>IRREGULAR BEHAVIOR</strong><br>
<sub>Too many rage-level airborne and no-scope results.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/silentaim.gif" width="100%" alt="CS2AC detecting bullets that disagree with visible aim">
<br><strong>SILENTAIM</strong><br>
<sub>The bullets hit somewhere the visible aim never pointed.</sub>
</td>
</tr>
</table>

<div align="center">

### CS2AC + CS2FOW

<a href="https://github.com/karola3vax/CS2FOW">
<img src="docs/showcase/cs2fow-wallhack.gif" width="100%" alt="CS2FOW operating across Dust II long sightlines">
</a>

**Catch the cheat. Starve the wallhack.**

<sub>CS2AC catches cheating behavior. CS2FOW stops hidden enemies from being sent to the cheater in the first place.</sub>

</div>

## The Seventeen Detection Modules

### Aim and accuracy

**Aimbot.** The player's aim moves sharply onto an enemy before a damaging shot. CS2AC checks whether this pattern occurs across separate shots.

**Aimlock.** The player's aim closely follows a moving enemy for an extended period. CS2AC measures how closely the aim follows the target over time, including when the target is behind a wall.

**Silentaim.** A damaging bullet lands away from the direction shown by the player's aim. CS2AC compares the aim at the moment of the shot with the bullet impact and resulting damage.

**Inhuman Accuracy.** The player maintains an unusually high hit rate across a longer series of aimed shots. CS2AC tracks those shots and how many of them cause damage.

**Irregular Behavior.** The player repeatedly lands difficult shots while airborne or without using a sniper scope. CS2AC counts both successful and missed attempts over time.

### Movement

**Autostrafe.** The player repeatedly gains or preserves speed through highly consistent movement while airborne. CS2AC compares the player's movement, speed, and timing across each jump.

**Bhop.** The player repeatedly jumps again as soon as they touch the ground. CS2AC measures the time between landing and the next jump across consecutive hops.

**Hyperscroll.** The player sends unusually rapid bursts of jump inputs while landing. CS2AC checks those inputs together with the timing of the resulting jumps.

**Nulls.** The player switches between opposite movement directions with highly consistent timing while airborne. CS2AC compares the movement keys with the direction changes sent by the player.

### Exploits and client behavior

**Antiaim.** The player's view spins, jitters, returns after an attack, or reaches angles outside normal play. CS2AC checks the view angles and their order across consecutive commands.

**DLL Injection.** The player's game subscribes to a group of events associated with injected client code. CS2AC checks those event subscriptions after the player joins and again while they remain connected.

**Desubticking.** The player's movement inputs repeatedly arrive without their normal timing between ticks. CS2AC checks the timing attached to each movement change.

**Doubletap.** The same weapon fires twice before its normal delay has passed. CS2AC compares the weapon and server tick of each consecutive shot.

**Invalid CVar.** The player's game reports a protected or monitored setting outside its accepted value. CS2AC requests these settings from the client and checks each reply.

**Invalid Input.** The player's button state does not match the recorded order of button presses and releases. CS2AC compares both parts of the command sent by the client.

**Namechanger.** The player changes their visible name repeatedly within a short period. CS2AC counts name changes for each connected player.

**Subtick Spam.** The player repeatedly sends many movement or aim changes at the same point within a tick. CS2AC checks how often these same-time input bursts occur.

## One detection. Everywhere.

When CS2AC acts, it can do all of this at once:

1. Announce the detection in public chat.
2. Hold a clear center-screen alert for five seconds.
3. Write the evidence and punishment result to the server console.
4. Run your configured ban or kick command.
5. Send a detailed Discord webhook report.

```text
[CS2AC] detected AIMBOT on Player and punished.
```

<div align="center">

<img src="docs/showcase/announcement-chat.png" width="600" alt="CS2AC test announcement in public chat">

<table>
<tr>
<td width="33%" align="center">
<img src="docs/showcase/announcement-center.png" width="100%" alt="CS2AC center-screen test announcement">
<br><strong>Five-second center alert</strong>
</td>
<td width="33%" align="center">
<img src="docs/showcase/detection.png" width="100%" alt="CS2AC center-screen Aimbot detection">
<br><strong>Detection sent</strong>
</td>
<td width="33%" align="center">
<img src="docs/showcase/whitelist.png" width="100%" alt="CS2AC announcing a detection on a whitelisted player">
<br><strong>Whitelist stays visible</strong>
</td>
</tr>
</table>

</div>

Whitelisting does not silence CS2AC. The detection still appears in chat, on screen, in the console, and in Discord; only the punishment command is skipped.

## Quickstart

You need a Windows x64 or Linux x64 CS2 dedicated server running [Metamod:Source](https://www.sourcemm.net/) 2.x.

1. Open this repository's **Releases** tab and choose the matching Windows or Linux package.
2. Extract it into the CS2 server root without rearranging anything. The package begins with the `game` folder.
3. Edit `game/csgo/cfg/cs2ac.cfg`.
4. Start the server.
5. Run `meta list`, then `cs2ac_status`.

That is it. Players install nothing.

The default punishment commands are made for [CS2-SimpleAdmin](https://github.com/daffyyyy/CS2-SimpleAdmin). Using another admin plugin? Replace the two commands in `cs2ac.cfg` with commands that plugin understands.

## Configuration

The included [`cs2ac.cfg`](cfg/cs2ac.cfg) explains every option in plain language.

| Setting | Default | What it does |
| --- | ---: | --- |
| `cs2ac_enabled` | `1` | Master switch for CS2AC. |
| `cs2ac_whitelist` | empty | SteamID64s that may be detected but must never be punished. |
| `cs2ac_*_enabled` | `1` | Enable or disable one detection module. |
| `cs2ac_chat_announcements` | `1` | Show detections in public chat. |
| `cs2ac_center_announcements` | `1` | Show the five-second center alert. |
| `cs2ac_punishment_command` | `css_addban ...` | Command used for permanent bans. |
| `cs2ac_kick_command` | `css_kick ...` | Command used for kick-only detections. |
| `cs2ac_webhook_url` | empty | Discord webhook that receives detection reports. |
| `cs2ac_webhook_role_id` | empty | Discord role to mention on a report. |
| `cs2ac_webhook_server_address` | automatic | Server address shown in Discord. |
| `cs2ac_allow_sv_cheats_testing` | `0` | Allow local detector testing with `sv_cheats 1`. Never enable this on a public server. |

Punishment commands support `{steamid64}`, `{userid}`, and `{detection}`:

```cfg
cs2ac_punishment_command "css_addban {steamid64} 0 CS2AC: {detection}"
cs2ac_kick_command "css_kick #{userid} CS2AC: {detection}"
```

Whitelist one account or a comma-separated list:

```cfg
cs2ac_whitelist "76561198000000001,76561198000000002"
```

### Discord in four steps

1. Create a webhook in the Discord channel that should receive detections.
2. Put its URL in `cs2ac_webhook_url`.
3. Run `cs2ac_reload`.
4. Run `cs2ac_webhook_test`.

Keep the webhook URL private. CS2AC never prints it back to the console.

<div align="center">
<img src="docs/showcase/webhook.png" width="432" alt="CS2AC Discord detection report with player, evidence, punishment, map, and server details">
</div>

<details>
<summary><strong>Server commands</strong></summary>

| Command | What it does |
| --- | --- |
| `cs2ac_status` | Show whether CS2AC and its main features are working. |
| `cs2ac_help` | List the available CS2AC commands. |
| `cs2ac_reload` | Reload `cs2ac.cfg`. |
| `cs2ac_check_config` | Find mistakes in the current configuration. |
| `cs2ac_test_announcement` | Preview the chat and center-screen alert without detecting anyone. |
| `cs2ac_webhook_test` | Send a test detection report to Discord. |

</details>

## FAQ

<details>
<summary><strong>Do players install anything?</strong></summary>

Nope. CS2AC lives entirely on the dedicated server, so players just connect and play as usual.

</details>

<details>
<summary><strong>Does it work in Premier or Valve matchmaking?</strong></summary>

No. CS2AC is made for community and dedicated servers you control. It cannot be added to Premier or Valve matchmaking.

</details>

<details>
<summary><strong>Can it catch every cheat?</strong></summary>

No. No anti-cheat catches everything. CS2AC can only judge the behavior that reaches the server; it does not read a player's files, memory, or desktop.

Despite the name, **DLL Injection does not scan anyone's PC**. It only checks suspicious game-event subscriptions that the client shares with the server.

</details>

<details>
<summary><strong>Which detections ban and which only kick?</strong></summary>

By default, Desubticking, Nulls, and Subtick Spam only kick. Every other detection uses the permanent-ban command.

Want different punishments? Change or empty either command. The detection announcements will keep working.

</details>

<details>
<summary><strong>What happens to whitelisted players?</strong></summary>

They can still trigger a detection, and everyone can still see it, but CS2AC stops before sending any punishment command.

</details>

<details>
<summary><strong>Does it support FFA?</strong></summary>

Yes. When `mp_teammates_are_enemies` is enabled, CS2AC treats other players as enemies just like the game does.

</details>

<details>
<summary><strong>Does CS2AC advertise itself?</strong></summary>

Yes, but it does not spam. Every six completed rounds, CS2AC shows this message once in chat and at the center of the screen:

```text
[CS2AC] This server is protected by karola3vax's anti-cheat.
```

This small project credit is built in and cannot be turned off.

</details>

## Want wallhack protection too?

**CS2AC catches cheating behavior. [CS2FOW](https://github.com/karola3vax/CS2FOW) stops your server from sending live enemy positions through solid walls and smoke.**

They solve different problems, run entirely on the server, and can protect the same CS2 community server together.

## Building from source

Clone the pinned submodules:

```sh
git clone --recursive https://github.com/karola3vax/CS2AC.git
cd CS2AC
```

Windows needs Python 3.8 or newer and Visual Studio 2022 with the C++ workload:

```powershell
./build-windows.ps1
```

Linux needs Python 3.8 or newer and Docker:

```sh
./build-linux.sh
```

Both scripts make a directly installable package under the build folder's `package/game` directory. Linux builds inside the pinned Steam Runtime 3 SDK image.

## Be part of it

Run CS2AC on a real server. Test it. Break it. Send useful reports.

If it earns a place on your server, **star the repository and share your clips**. That is how an open-source anti-cheat gets harder to bypass.

## License

CS2AC is free and open-source software licensed under the [GNU Affero General Public License v3.0](LICENSE). Dependencies keep their own licenses; see [Third-party notices](THIRD_PARTY_NOTICES.md).
