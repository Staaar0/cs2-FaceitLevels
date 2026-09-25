# CS2FaceitLevels 

CS2 plugin that shows a player's real FACEIT level in the CS2 scoreboard.

From 1 to Challenger badge

The plugin does this:

1. Reads the player's SteamID64 after authorization.
2. Requests the player's FACEIT CS2 skill level from the FACEIT Data API.
3. Writes the mapped pin ID to the scoreboard pin slot.
4. workshop addon replaces those pin images with FACEIT level images.

## Requirements

- [CounterStrikeSharp](https://github.com/roflmuffin/CounterStrikeSharp)
- [Faceit API Key](https://developers.faceit.com/)

## ELO Chat Commands

Players can privately check FACEIT ELO using chat commands.

### Commands

- elo/elos commands can be disabled via config

| Command | Description |
|---|---|
| `!elo <playername>` | Shows the selected player’s FACEIT ELO |
| `!elos` | Shows all connected players’ FACEIT ELO |


## Install on server

Copy the `addons` folder into your CS2 server `game/csgo/` folder:

Edit this file and add your FACEIT API key:

```text
counterstrikesharp/configs/plugins/CS2FaceitLevels/CS2FaceitLevels.json.
```

Supported languages:

```text
ar, en, lv, pl, pt-BR, pt-PT, ru, tr, ua, zh-ch
```

Default language:

```json
"language": "en"
```

## Default Config
```json
{
  "faceit_api_key": "PUT_YOUR_FACEIT_API_KEY_HERE",
  "language": "en",
  "debug": false,
  "cache_minutes": 30,
  "request_timeout_seconds": 10,
  "clear_pin_when_no_faceit": false,
  "enable_elo_commands": true,
  "builtin_workshop_loader": true,
  "ConfigVersion": 1
}
```
