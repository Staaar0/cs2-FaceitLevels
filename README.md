# CS2FaceitLevels 

CS2 plugin that shows a player's real FACEIT level in the scoreboard.

## Requirements

- [CounterStrikeSharp](https://github.com/roflmuffin/CounterStrikeSharp)
- [Faceit API Key](https://developers.faceit.com/)

## Commands

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

### languages:

```text
ar, en, lv, pl, pt-BR, pt-PT, ru, tr, ua, zh-ch
```
Default: `en`

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
  "AutoUpdateSignatures": true,
  "ConfigVersion": 2
}
```
