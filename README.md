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
> - Adds `src/karasu/` — a per-detector confidence table, a corroboration ledger, and a
>   console relay that hands detections to the Karasu CS2 plugin.
> - Replaces "which detections ban" with a single confidence dial
>   (`cs2ac_karasu_solo_ban_confidence`). Every detector can ban, but the weaker ones
>   have to repeat or be corroborated first, and one convar decides where that line sits.
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
[![Detections](https://img.shields.io/badge/detections-17-red?style=for-the-badge)](#detection-modules)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-5c7cfa?style=for-the-badge)](#quickstart)
[![License](https://img.shields.io/badge/license-AGPL--3.0-2ea44f?style=for-the-badge)](LICENSE)

**A good Counter-Strike match should be decided by the players, not by who brought the better cheat.**

CS2AC watches the aim, shots, movement, button presses, and game settings that players send to the server. When it finds enough strong evidence, it reports the player and can ask the server to punish them.

[Watch it work](#showcase) · [Read the detection modules](#detection-modules) · [Pair it with CS2FOW](#cs2ac-and-cs2fow)

</div>

## Showcase

<table>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/aimbot.gif" width="100%" alt="CS2AC detecting a blatant snap-hit aimbot">
<br><strong>AIMBOT</strong><br>
<sub>A damaging shot follows a blatant snap onto the target.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/aimlock.gif" width="100%" alt="CS2AC detecting inhumanly precise target tracking">
<br><strong>AIMLOCK</strong><br>
<sub>The aim follows a moving enemy with machine-like precision.</sub>
</td>
</tr>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/antiaim.gif" width="100%" alt="CS2AC detecting impossible anti-aim angles">
<br><strong>ANTIAIM</strong><br>
<sub>The view produces impossible angles, spin, jitter, or attack-return patterns.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/bhop.gif" width="100%" alt="CS2AC detecting automated bunny hopping">
<br><strong>BHOP</strong><br>
<sub>Landings and jump inputs repeat with automated timing.</sub>
</td>
</tr>
<tr>
<td width="50%" align="center">
<img src="docs/showcase/irregular-behavior.gif" width="100%" alt="CS2AC detecting repeated irregular airborne and no-scope results">
<br><strong>IRREGULAR BEHAVIOR</strong><br>
<sub>Rage-level airborne and no-scope results keep adding up.</sub>
</td>
<td width="50%" align="center">
<img src="docs/showcase/silentaim.gif" width="100%" alt="CS2AC detecting a firing angle that disagrees with visible aim">
<br><strong>SILENTAIM</strong><br>
<sub>The damaging firing angle does not follow the visible aim path.</sub>
</td>
</tr>
</table>

### CS2AC and CS2FOW

<div align="center">

<a href="https://github.com/karola3vax/CS2FOW">
<img src="docs/showcase/cs2fow-wallhack.gif" width="100%" alt="CS2FOW operating across Dust II long sightlines">
</a>

**CS2AC catches cheating behavior. CS2FOW stops hidden enemy positions from reaching the wallhack.**

<sub>They solve different problems, run entirely on the server, and can protect the same CS2 community server together.</sub>

</div>

## Detection output

When a detector finds enough evidence, CS2AC can report the result wherever the server owner needs it:

1. Announce it in public chat.
2. Hold a center-screen alert for five seconds.
3. Write the detector evidence to the server console.
4. Submit the configured ban or kick command to the server.
5. Send a detailed Discord webhook report.

Chat and center-screen announcements can be turned on or off separately. CS2AC sends the chosen ban or kick command to the server. The installed admin plugin must understand and run that command.

```text
[CS2AC] detected AIMBOT on Player and punished.
```

<div align="center">

<img src="docs/showcase/announcement-chat.png" width="600" alt="CS2AC test announcement in public chat">

<table>
<tr>
<td width="33%" align="center">
<img src="docs/showcase/announcement-center.png" width="100%" alt="CS2AC center-screen test announcement">
<br><strong>Center alert</strong>
</td>
<td width="33%" align="center">
<img src="docs/showcase/detection.png" width="100%" alt="CS2AC center-screen Aimbot detection">
<br><strong>Detection announced</strong>
</td>
<td width="33%" align="center">
<img src="docs/showcase/whitelist.png" width="100%" alt="CS2AC announcing a detection on a whitelisted player">
<br><strong>Whitelist visible</strong>
</td>
</tr>
</table>

<img src="docs/showcase/webhook.png" width="432" alt="CS2AC Discord detection report with player, evidence, punishment, map, and server details">

</div>

Whitelisted players can still be detected and reported, but CS2AC stops before submitting a punishment command. Disabled output channels stay quiet.

## Detection modules

All 17 modules are enabled by default, and each one can be turned off. Open **How strict is it?** to see its main rule. These numbers come from the current code and cannot be changed in the config.

### Aim and accuracy

**Aimbot.** An aimbot can quickly move the crosshair onto an enemy just before it shoots. CS2AC checks where the player was aiming before and after a damaging shot, and how close the snap moved to the enemy.

<details>
<summary><strong>How strict is it?</strong></summary>

Three suspicious snap shots within five minutes. The enemy must be at least 100 game units away, and CS2AC checks the half-second before the shot.

</details>

**Aimlock.** An aimlock keeps the crosshair stuck to a moving enemy. CS2AC checks whether the crosshair stays inside a small area around the same enemy while they move, even behind a wall.

<details>
<summary><strong>How strict is it?</strong></summary>

This must happen three times within five minutes. Each time, the lock must last two seconds, stay on the enemy for at least 95% of that time, follow at least 128 game units of movement, and start at least 200 game units away.

</details>

**Silentaim.** Silent aim can send a damaging shot in another direction without moving the player's visible aim there. CS2AC checks where the player looked just before, during, and after the shot. It also allows for the weapon's normal accuracy and bullet spread.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs 10 points within five minutes. A suspicious hit adds 2 points on the ground, 3 in the air, or 4 when it is more than 22.5 degrees past the weapon's normal limit. A headshot adds 1 more, a hit through a wall or smoke adds 2 more, and a no-scope adds 1 more. A normal hit removes 2 points.

</details>

**Inhuman Accuracy.** Nospread and rage cheats can keep hitting far more shots than a normal player. CS2AC counts only shots fired while the crosshair is already close to a real enemy, then checks how many of those shots deal damage.

<details>
<summary><strong>How strict is it?</strong></summary>

At least 40 counted shots within five minutes. The enemy must be at least 100 game units away, and at least 90% of the shots must hit.

</details>

**Irregular Behavior.** Rage cheats can turn jump shots and no-scope sniper shots into easy kills. CS2AC counts both hits and misses. Harder kills, such as long-range headshots and wallbangs, give more points.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs 16 points within five minutes. It also needs at least three hard kills from at least four attempts, with a success rate of 50% or more. Kills below 10 metres do not count as successful hard kills.

</details>

### Movement

**Autostrafe.** An autostrafe cheat moves the player left and right in the air to gain or keep speed. CS2AC compares the turns, speed, and timing across many jumps.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs 15 suspicious jumps in the latest 20. It can detect after five suspicious jumps when at least one has more than 30 strafes per second. It can also detect when more than 90% of recent air turns follow the same perfect pattern.

</details>

**Bhop.** A bhop cheat presses jump at the exact moment the player lands, again and again. CS2AC checks the landing time and repeated jump-button patterns. Failed jumps make the evidence weaker.

<details>
<summary><strong>How strict is it?</strong></summary>

CS2AC starts checking after at least 20 landings. It needs either 10 perfect jumps in a row, or a score of 7 where at least 90% of seven or more jump patterns repeat the same short input pattern.

</details>

**Hyperscroll.** Hyperscroll sends a very large number of jump presses around each landing. CS2AC checks how many presses were sent and how often they still produced a perfect jump.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs at least 20 jump patterns and 20 checked landings. They must average at least 16 jump presses, and more than 60% of the landings must be perfect jumps.

</details>

**Nulls.** A nulls script changes between opposite movement keys, such as A and D, with perfect timing in the air. CS2AC checks when one key is released, when the other is pressed, the player's air speed, and their frame rate.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs at least 128 key changes and a perfect-timing score between 128 and 640 while the player moves at least 100 game units per second in the air. A lower frame rate or a normal delay between the two keys makes the detector require more evidence.

</details>

### Client behavior

**Antiaim.** Anti-aim sends view angles that normal play should not create. It can make the player look in an impossible direction, spin, repeat the same shaking pattern, or quickly turn away and back while shooting. CS2AC gives these actions points, and the points fall over time when the behavior stops.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs 100 points. It can also detect a steady spin of 320–999 degrees per second after 15 seconds, a steady spin of 1,000 degrees per second or more after 10 seconds, or the same shaking pattern repeating for 10 seconds.

</details>

**DLL Injection.** Some injected cheats ask the game to send them events that a normal client does not usually need. The server can see this event list, so CS2AC compares it with a list of suspicious events.

<details>
<summary><strong>How strict is it?</strong></summary>

One match from the 117 checked events is enough. The first check runs 10 seconds after the player joins, and the next checks run every two minutes.

</details>

This detector does not scan the player's files or computer memory. It cannot find every kind of DLL injection. It only reports this one behavior that the server can see.

**Desubticking.** CS2 normally records the small time between movement changes inside each server update. Some cheats remove that time and set it to zero. CS2AC checks how often this happens.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs at least 30 movement commands within 20 seconds. At least 90% of them must have their small timing value set to zero. CS2AC ignores the first 10 seconds after the player joins.

</details>

**Doubletap.** A doubletap cheat makes the same gun fire twice almost at once. CS2AC looks for the same gun firing twice during the same server update or the next one. It also checks the player's connection before sending a punishment.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs two fast-fire pairs. A bad connection does not hide the alert, but CS2AC will not send the punishment command.

</details>

**Invalid CVar.** A CVar is simply a game setting. CS2AC asks the player's game for important settings and checks whether the values are normal. This can find protected settings that were changed or values that should not be possible.

<details>
<summary><strong>How strict is it?</strong></summary>

One invalid setting is enough. CS2AC reports it once and stays quiet until that setting becomes normal again.

</details>

**Invalid Input.** Every movement command tells the server which buttons are held and which buttons were pressed or released. CS2AC checks whether those two parts agree. Cheats can create commands where they do not.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs eight broken commands within five seconds.

</details>

**Namechanger.** A namechanger cheat quickly changes the player's visible name to create spam or confusion. CS2AC counts the name changes for each player separately.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs five visible name changes within one minute.

</details>

**Subtick Spam.** This cheat sends many movement or aim changes at exactly the same moment inside one server update. CS2AC counts commands that repeat this unusual pattern.

<details>
<summary><strong>How strict is it?</strong></summary>

The detector needs 20 suspicious commands within half a second.

</details>

## Quickstart

You need a Windows x64 or Linux x64 CS2 dedicated server running [Metamod:Source](https://www.sourcemm.net/) 2.x.

1. Open this repository's **Releases** tab and choose the matching Windows or Linux package.
2. Extract it into the CS2 server root without rearranging anything. The package begins with the `game` folder.
3. Edit `game/csgo/cfg/cs2ac.cfg`.
4. Start the server.
5. Run `meta list`, then `cs2ac_status`.

That is it. Players install nothing.

The default punishment commands are made for [CS2-SimpleAdmin](https://github.com/daffyyyy/CS2-SimpleAdmin). If your server uses another admin plugin, replace the two commands in `cs2ac.cfg` with commands that plugin understands.

CS2AC checks for stable updates after startup and every six hours. A verified update is prepared in the background and installed on the next full server restart. Existing settings are copied into the new configuration layout, and the previous configuration and plugin binary are kept as backups.

## Configuration

<details>
<summary><strong>Configuration reference</strong></summary>

The included [`cs2ac.cfg`](cfg/cs2ac.cfg) explains every option in plain language.

| Setting | Default | What it does |
| --- | ---: | --- |
| `cs2ac_enabled` | `1` | Master switch for CS2AC. |
| `cs2ac_whitelist` | empty | SteamID64s that may be detected but must never be punished. |
| `cs2ac_*_enabled` | `1` | Enable or disable one detection module. |
| `cs2ac_chat_announcements` | `1` | Show detections in public chat. |
| `cs2ac_center_announcements` | `1` | Show the five-second center alert. |
| `cs2ac_language` | `en` | Language used for public messages and Discord reports. |
| `cs2ac_punishment_command` | `css_addban ...` | Command submitted for permanent-ban detections. |
| `cs2ac_kick_command` | `css_kick ...` | Command submitted for kick-only detections. |
| `cs2ac_webhook_url` | empty | Discord webhook that receives detection reports. |
| `cs2ac_webhook_role_id` | empty | Discord role to mention on a report. |
| `cs2ac_webhook_server_address` | automatic | Server address shown in Discord. |
| `cs2ac_webhook_logo_url` | empty | Override the default CS2AC image shown in Discord reports. |
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

Set `cs2ac_language` to one of the bundled language codes, then run `cs2ac_reload`:

`ar`, `bg`, `cs`, `da`, `de`, `el`, `en`, `es-419`, `es-es`, `et`, `fi`, `fr`, `he`, `hr`, `hu`, `id`, `it`, `ja`, `ko`, `lt`, `lv`, `nl`, `no`, `pl`, `pt-br`, `pt-pt`, `ro`, `ru`, `sk`, `sr`, `sv`, `th`, `tr`, `uk`, `vi`, `zh-cn`, `zh-tw`.

</details>

<details>
<summary><strong>Discord webhooks</strong></summary>

1. Create a webhook in the Discord channel that should receive detections.
2. Put its URL in `cs2ac_webhook_url`.
3. Run `cs2ac_reload`.
4. Run `cs2ac_webhook_test`.

Keep the webhook URL private. CS2AC never prints it back to the console.

</details>

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

No. CS2AC runs on the dedicated server, so players connect and play as usual.

</details>

<details>
<summary><strong>Does it work in Premier or Valve matchmaking?</strong></summary>

No. CS2AC is made for community and dedicated servers you control. It cannot be added to Premier or Valve matchmaking.

</details>

<details>
<summary><strong>Can it catch every cheat?</strong></summary>

No anti-cheat catches everything. CS2AC can only judge the behavior that reaches the server; it does not read a player's files, memory, or desktop.

</details>

<details>
<summary><strong>Which detections ban and which only kick?</strong></summary>

By default, Desubticking, Nulls, and Subtick Spam use the kick command. Invalid CVar also uses it for conditions that should be corrected rather than permanently banned. Other detections use the permanent-ban command.

Server owners can change or empty either command. Detection announcements continue to work.

</details>

<details>
<summary><strong>What happens to whitelisted players?</strong></summary>

They can still trigger a detection and its enabled reports, but CS2AC does not submit a punishment command for them.

</details>

<details>
<summary><strong>Does it support FFA?</strong></summary>

Yes. When `mp_teammates_are_enemies` is enabled, CS2AC treats other players as enemies just like the game does.

</details>

<details>
<summary><strong>Does CS2AC advertise itself?</strong></summary>

Yes. Five seconds after a player fully joins, CS2AC privately shows this message to that player in chat and at the center of their screen. The center message stays for three seconds, and no one else sees it:

```text
[CS2AC] This server is protected by karola3vax's anti-cheat.
```

This small project credit is built in and cannot be turned off.

</details>

## Building from source

<details>
<summary><strong>Windows and Linux build instructions</strong></summary>

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

</details>

## Contributing

Run CS2AC on a real server. Test it, send reproducible reports, and include the detector evidence whenever something looks wrong.

If CS2AC earns a place on your server, star the repository and share your clips. That helps more server owners find it and gives the project better real-world feedback.

## License

CS2AC is free and open-source software licensed under the [GNU Affero General Public License v3.0](LICENSE). Dependencies keep their own licenses; see [Third-party notices](THIRD_PARTY_NOTICES.md).
