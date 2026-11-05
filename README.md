# je2be-core

Core library of data converter for Minecraft Java, Bedrock, Xbox360, and PS3 Edition.

## Prune chunks in place

`prune-chunks` deletes selected chunks from an existing Java or Bedrock world
**in place**. It never converts or rewrites the content of chunks that are
kept.

```sh
prune-chunks -i /path/to/world -c selection.json -d keep
```

- `-i`, `--input`: Java world directory (vanilla, paper server root, or the
  modern `dimensions/minecraft/...` layout) or Bedrock world directory
  containing `db/` and `level.dat`.
- `-c`, `--config`: selection file in the `mbr-chunk-selection` format
  (`schemaVersion` 1, `chunkSize` 16, inclusive signed chunk coordinates,
  `overlapPolicy` `last-wins`). Regions are evaluated in file order and the
  last matching region wins.
- `-d`, `--default`: action for chunks outside every configured region:
  `keep` (default) or `delete`. The JSON `defaultAction` is UI/export metadata;
  this flag overrides it for execution.

`dimension` uses the Java numeric dimension ids: `-1` nether, `0` overworld,
`1` end. Bedrock worlds map these to their internal `1`/`0`/`2` values. Only
the selected dimension is touched; all other dimensions are left untouched, so
`-d delete` cannot remove chunks from a dimension the config did not select.

### Deletion precedence

1. The last matching config region (explicit `keep` or `delete`).
2. Stored map display protection: chunks covered by any valid stored map in
   the target dimension are kept.
3. The CLI `-d` value.

Map protection is deliberately conservative. Every valid map record stored in
the world is protected, including maps that currently have no item, frame, or
chest reference; map copies that share an id share the same stored map. A
config region that explicitly says `delete` overrides map protection. Java
maps use `data/map_<id>.dat` (legacy) or `data/minecraft/maps/<id>.dat`
(current); Bedrock maps are `map_<id>` LevelDB records. Map ranges are
`128 << scale` blocks centered on `xCenter`/`zCenter`.

### What is removed

- Java: the chunk slots in `region/`, `entities/`, and `poi/` of the target
  dimension, plus external `c.<cx>.<cz>.mcc` payloads. Worlds without
  `entities/` or `poi/` (legacy layouts) are supported; those files simply do
  not exist. Kept external chunks and their sidecars are left byte-identical.
- Bedrock: every chunk-keyed record at an existing chunk coordinate of the
  target dimension (terrain, sub-chunks, versions, data 2D/3D, block entities,
  entities, pending ticks, structure bounds, and future chunk tags with the
  same key shape), the per-chunk `digp` entity index, and `actorprefix`
  records whose last referring `digp` was removed. `map_*`, player records,
  `VILLAGE_*`, `portals`, `mobevents`, `PosTrackDB-*`,
  `LevelChunkMetaDataDictionary`, `structuretemplate_*`, and other global
  records are **preserved**: they are not owned by a single chunk. In
  particular, village records span multiple chunks and are updated by the
  game; deleting a village because part of its area was pruned would discard
  surviving village state. Orphaned global entries are a documented boundary
  of this tool.

The tool iterates the chunks/keys that actually exist in the world and tests
each against the last-wins config rectangles and the protected map rectangles.
It does not enumerate the (possibly unbounded) configured rectangles.

### Safety

Stop the world/server first. `prune-chunks` takes Java's `session.lock` with a
non-blocking `fcntl` write lock (`LockFile` on Windows) and fails fast if the
world is in use; Bedrock is opened through live LevelDB, which takes the
database `LOCK`. **Back up the world before running a destructive prune.**

Before writing, the tool validates the whole config, parses every stored map
in the target dimension, and validates every region header/sector range (Java)
or map record (Bedrock). A damaged map, an unknown map field layout, an
overlapping/out-of-range region entry, or an unreadable record causes an error
and exit *before* any chunk data is written.

Atomicity limits: Java region compaction writes a temporary file and renames
it over the original per region file; in-place slot clearing (used when a
region contains external chunks or an unreadable payload) is a sequence of
header and payload writes. Bedrock deletions are batched LevelDB writes.
There is no cross-file transaction: a crash or I/O error partway through can
leave a partially pruned but individually consistent world. Re-running the
same config is idempotent for the chunks that remain.

## Override world settings

```sh
b2j -i /path/to/bedrock-world -o /path/to/java-world -O \
  difficulty_settings.difficulty=hard \
  GameType=1 \
  game_rules.minecraft:keep_inventory=true \
  world_gen_settings.seed=123456789 \
  experiments.minecraft:trade_rebalance=true
```

To sample conversion and Terraform throughput while a conversion is running,
pass an output CSV path:

```sh
b2j -i /path/to/bedrock-world -o /path/to/java-world --profile /tmp/b2j-profile.csv
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
