# je2be-core

Core library of data converter for Minecraft Java, Bedrock, Xbox360, and PS3 Edition.

## Override world settings

```sh
b2j -i /path/to/bedrock-world -o /path/to/java-world -O \
  difficulty_settings.difficulty=hard \
  GameType=1 \
  game_rules.minecraft:keep_inventory=true \
  world_gen_settings.seed=123456789 \
  experiments.minecraft:trade_rebalance=true
```

```sh
j2b -i /path/to/java-world -o /path/to/bedrock-world --override \
  Difficulty=3 \
  GameType=1 \
  keepinventory=true \
  Generator=2 \
  experiments.villager_trades_rebalance=true
```

Paths are case-sensitive and use dots to access nested NBT compounds or list
indices. Use `\.` for a literal dot in a key (quote it so the shell preserves
the backslash). Existing fields retain their NBT type. New values use SNBT
syntax, so type suffixes (`1b`, `2s`, `3l`, `4.0f`, `5.0d`), lists, arrays,
and compounds are supported. Quote a whole shell argument when its value
contains spaces or shell metacharacters.

For current Java worlds, an unqualified path targets the `Data` compound in
`level.dat`. The prefixes `game_rules`, `world_gen_settings`, `weather`,
`world_clocks`, and `ender_dragon_fight` target the corresponding files under
the world's data directories. `GameRules` and `WorldGenSettings` are accepted
as aliases. The virtual `experiments.<feature-id>` path updates both
`enabled_features` and the matching data-pack state.

[Java Edition level format](https://minecraft.wiki/w/Java_Edition_level_format)
and [Bedrock Edition level format](https://minecraft.wiki/w/Bedrock_Edition_level_format).

## Repair Java player UUIDs

`j2j-uuid` updates player UUID references in an existing Java world.

```text
8667ba71-b85a-4004-af54-457a9734eed7,bb84e4a8-a756-42ee-8909-2ef9a527064c
```

```sh
j2j-uuid --input /path/to/world --mapping /path/to/player-map.csv
```

Use `--dry-run` to inspect the number of changes without modifying the save.

## To convert playerdatas in multiplayer saves
Call your homies to put anything in their inventory, naming it as:
```
JavaTag=(their Java gamertag)
```
for example,
```
JavaTag=ElvinStarry
```
then do the convert.
For players who do not have such a marker in their inventory their playerdata will be assigned a RANDOM UUID in the save, thus an empty data will be given when they log into the server.
To preserve your friendship, run the SAME bedrock server where the save was in and call them to join the server and make the Java gamertag marker.
And and then use some tools to extract the MsaID of the player in the save, and process the Java save with `j2j-uuid`.
# SAST Tools

[PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static analyzer for C, C++, C#, and Java code.
