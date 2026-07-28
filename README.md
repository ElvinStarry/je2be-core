# je2be-core

Core library of data converter for Minecraft Java, Bedrock, Xbox360, and PS3 Edition.

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
