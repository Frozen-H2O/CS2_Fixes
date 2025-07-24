# About GFLBans's cs2fixes Fork
This is a fork of cs2fixes that adds GFLBans web integration to the admin system. Features that this fork adds include, but are not limited to, the following:
- Sync punishments across multiple servers within a community.
- Allow admins to view, create, edit, or remove the info of punishments through GFLBans without joining the game server.
- Punish clients that are not currently connected to the server by using GFLBans.
- Game timed punishments that only decrease while the punished player is connected to the game server.
- In game reporting features for players to notify disconnected admins through Discord.
- Admin commands to block player usage of in game admin chat and reporting features.
- Check a player's punishment history, including removed and/or expired punishments.
- Viewing the current map and player list of the server on the GFLBans web page.

# Setup
1. Host [GFLBans](https://github.com/gflze/GFLBans) by following the setup in its ``README.md``.
2. Create a new server listing in GFLBan's server management page.
3. Since it is expected that most servers using this fork also run other private changes, there is no public release for this plugin provided. As such, you must compile this repository yourself in the same way you would normally compile base cs2fixes.
4. Setup the following mandatory ConVars:
- ``gflbans_api_url <url>`` - URL to interact with GFLBans API. Should end in ``"api/"``
- ``gflbans_server_id <ID>`` - GFLBans ID for the server. This should be given when you create a server listing on GFLBans.
- ``gflbans_server_key <key>`` - GFLBans KEY for the server. DO NOT LEAK THIS. This should be given when you create a server listing on GFLBans.
5. Setup admin permissions on both GFLBans's admin management page and ``admins.cfg`` in cs2fixes (these will NOT auto sync).
6. Configure any other optional ConVars the plugin adds in a way that suits your community.

# ConVars
| ConVar | Parameter | Default Value | Description |
|:-------:|:---------|:-------------:|:-----------|
|``gflbans_api_url``|``<string>``|``"https://bans.gflclan.com/api/"``|URL to interact with GFLBans API. Should end in "api/"|
|``gflbans_server_id``|``<string>``|``"999"``|GFLBans ID for the server.|
|``gflbans_server_key``|``<string>``|``"1337"``|GFLBans KEY for the server. DO NOT LEAK THIS|
|``gflbans_hostname``|``<string>``|``"CS2 ZE Test"``|Name of the server (if a name is already set in GFLBan's server listing, this does nothing)|
|``gflbans_filter_no_punish_regex``|``<regex>``|``"nigga"``|The basic_regex (case insensitive) to delete any chat messages containing a match. USING AN INVALID REGEX WILL CRASH THE SERVER.|
|``gflbans_filter_regex``|``<regex>``|``"n+i+g+e+r+"``|The basic_regex (case insensitive) to delete any chat messages containing a match and punish the player that typed them. USING AN INVALID REGEX WILL CRASH THE SERVER.|
|``gflbans_filtered_gag_duration``|``<integer>``|``60`` (minutes)|Minutes to gag a player if they type a filtered message. Gags will only be issued with non-negative values|
|``gflbans_global``|``<boolean>``|``true``|Makes the server use global GFLBans punishments. This being enabled requires all admins to have ``PERMISSION_SCOPE_GLOBAL`` in order to issue punishments on the server.|
|``gflbans_min_real_world_timed``|``<integer>``|``61`` (minutes)|Minimum amount of minutes for a mute/gag duration to be real world timed. 0 or negative values force all punishments to be game timed.|
|``gflbans_report_cooldown``|``<integer>``|``600`` (seconds)|Minimum amount of seconds between ``c_report``/``c_calladmin`` usages. Minimum of 1 second.|

# Commands
Square brackets [] indicate a parameter is optional. Angled brackets <> indicate a parameter is required.
## Adding infractions
| Command | Parameters | Admin Flags | Description |
|--------:|:-----------|:-----------:|:------------|
|``ban``|``<name> <duration> [reason]``|``ADMFLAG_BAN``|Ban a player|
|``gag``|``<name> [duration] [reason]``|``ADMFLAG_CHAT``|Gag a player|
|``ungag``|``<name> [reason]``|``ADMFLAG_CHAT``|Ungag a player|
|``mute``|``<name> [duration] [reason]``|``ADMFLAG_CHAT``|Mute a player|
|``unmute``|``<name> [reason]``|``ADMFLAG_CHAT``|Unmute a player|
|``silence``|``<name> [duration] [reason]``|``ADMFLAG_CHAT``|Mute and gag a player|
|``unsilence``|``<name> [reason]``|``ADMFLAG_CHAT``|Unmute and ungag a player|
|``agag``|``<name> [duration] [reason]``|``ADMFLAG_CHAT``|Gag a player from using adminchat|
|``unagag``|``<name> [reason]``|``ADMFLAG_CHAT``|Ungag a player from using adminchat|
|``aungag``|``<name> [reason]``|``ADMFLAG_CHAT``|Alias for ``unagag``|
|``callban``|``<name> [duration] [reason]``|``ADMFLAG_CHAT``|Ban a player from using report and calladmin|
|``uncallban``|``<name> [reason]``|``ADMFLAG_CHAT``|Unban a player from using report and calladmin|
|``callunban``|``<name> [reason]``|``ADMFLAG_CHAT``|Alias for ``uncallban`` |

### Duration Types:
- **Session**: Lasts until the end of the current map.
  - No duration provided. Ex. ``!mute Ice``
  - Invalid duration provided. Ex. ``!mute Ice one``
  - Negative duration provided. Ex. ``!mute Ice -1``
- **Game Timed**: Time left on infraction only decreases while client is connected to the server.
  - Duration is less than ``gflbans_min_real_world_timed``. Ex. ``!mute Ice 5``
  - Duration is prefaced with a ``+``. Ex. ``!mute Ice +1d``
- **Real World Timed**: Time left on infraction decreases in real time, even while client is disconnected.
  - Duration is equal to or greater than ``gflbans_min_real_world_timed``. Ex. ``!mute Ice 1w``

## Misc
| Command | Parameters | Admin Flags | Description |
|--------:|:-----------|:-----------:|:------------|
|``calladmin``|``<reason>``||Request for an admin to join the server|
|``claim``||``ADMFLAG_GENERIC``|Claim the most recent GFLBans report/calladmin query|
|``confirm``|||Send a report or admin call queued within the last 30 seconds|
|``history``|``<name>``|``ADMFLAG_GENERIC``|Check a player's infraction history|
|``report``|``<name> <reason>``||Report a player|
|``status``|``[name]``||List a player's active punishments. Players without ``ADMFLAG_GENERIC`` may only check their own punishments.|

## _DEBUG compilations only
| Command | Description |
|:-------:|:------------|
|``dumpinf``|Dump server's infractions table to the server console.|

# CS2Fixes

CS2Fixes is a Metamod plugin with fixes and features aimed but not limited to zombie escape. This project also serves as a good example and help for source2mod and other developers.

## Installation

- Install [Metamod](https://cs2.poggu.me/metamod/installation/)
- Download the [latest release package](https://github.com/Source2ZE/CS2Fixes/releases) for your OS
- Extract the package contents into `game/csgo` on your server
- Configure the plugin cvars as desired in `cfg/cs2fixes/cs2fixes.cfg`, many features are disabled by default
- OPTIONAL: If you want to setup admins, rename `admins.cfg.example` to `admins.cfg` which can be found in `addons/cs2fixes/configs` and follow the instructions within to add admins

## Fixes and Features
You can find the documentation of the fixes and features [here](../../wiki/Home).

## Why is this all one plugin? Why "CS2Fixes"?

Reimplementing all these features as standalone plugins would duplicate quite a lot of code between each. Metamod is not much more than a loader & hook manager, so many common modding features need a fair bit of boilerplate to work with. And since our primary goal is developing CS2Fixes for all zombie escape servers, there is not necessarily a drawback to distributing our work in this form at the moment.

The CS2Fixes name comes from the CSSFixes and CSGOFixes projects, which were primarily aimed at low-level bug fixes and improvements for their respective games. Long term, we see this plugin slimming down and becoming more similar to them. Since as the CS2 modding scene matures, common things like an admin system and RTV become more feasible in source2mod or a similar modding platform.

## Compilation

```
git clone https://github.com/Source2ZE/CS2Fixes/ && cd CS2Fixes
git submodule update --init --recursive
```
### Docker (easiest)

Requires Docker to be installed. Produces Linux builds only.

```
docker compose up
```

Copy the contents of `dockerbuild/package/cs2/` to your server's `game/csgo/` directory.

### Manual

#### Requirements
- [Metamod:Source](https://github.com/alliedmodders/metamod-source)
- [AMBuild](https://wiki.alliedmods.net/Ambuild)

#### Linux
```bash
export MMSOURCE_DEV=/path/to/metamod
export HL2SDKCS2=/path/to/sdk/submodule

mkdir build && cd build
python3 ../configure.py --enable-optimize --sdks cs2
ambuild
```

#### Windows

Make sure to run in "x64 Native Tools Command Prompt for VS"

```bash
set MMSOURCE_DEV=\path\to\metamod
set HL2SDKCS2=\path\to\sdk\submodule

mkdir build && cd build
py ../configure.py --enable-optimize --sdks cs2
ambuild
```

Copy the contents of `build/package/cs2/` to your server's `game/csgo/` directory.
