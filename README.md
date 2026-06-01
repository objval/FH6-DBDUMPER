# FH6-DBDUMPER

Forza Horizon 6 database dumper and mod tool. Attaches to a running FH6 process, resolves the internal SQLite database via AOB pattern scanning, and provides a TUI for dumping, modifying, and persisting game data.

## Features

**Database**
- Dump the full game database (215 tables, 135K+ rows) to a local SQLite file
- Load a modified database back into the running game

**Mods (toggleable)**
- All Cars in Autoshow -- unlocks every car in the autoshow *(reversible)*
- Free Cars -- sets all car prices to 0 CR *(reversible)*
- Free Upgrades -- all performance and visual upgrade parts cost 0
- Unlock Hidden Cars -- makes all hidden/traffic/unobtainable cars visible, purchasable, and tradeable *(reversible)*
- Unlock Barn Finds -- reveals all barn find locations
- Clear New Tags -- removes the "new" badge from all garage cars

**Info**
- Show Garage Stats -- full garage overview with PI, class, distance, wins, skills
- Export Garage to JSON -- dumps all garage data including full tuning parameters

## Building

Requires Visual Studio 2022 (v143 toolset) and Windows SDK 10.0.

```
msbuild FH6-DBDUMPER.slnx -p:Configuration=Release -p:Platform=x64
```

Output: `bin/FH6-DBDUMPER.exe`

## Usage

1. Launch Forza Horizon 6
2. Run `FH6-DBDUMPER.exe` as administrator
3. The tool attaches to the game, resolves the database, and shows the menu
4. Type a number `[1-6]` to toggle a mod, or a letter `[D/L/S/E]` for an action
5. Reversible mods create backup tables and can be toggled back off

## How it works

The tool uses AOB (Array of Bytes) pattern scanning to locate the `CDatabase` structure in the game's memory. It then calls the game's own `ExecuteQuery` function via a remote thread to run SQL queries against the live in-memory database. All mod operations are SQL-level -- no game files are modified on disk.

## Detection mitigations

- Minimal process handle rights (no PROCESS_ALL_ACCESS)
- Shellcode allocated as RW, flipped to RX after write (no RWX allocations)
- AOB scan targets PE .text sections only (no bulk memory reads)

## Fork

This is a fork of [matkhl/FH6-DBDUMPER](https://github.com/matkhl/FH6-DBDUMPER) with a redesigned TUI, additional mods, and detection mitigations.

## Disclaimer

For educational purposes only. Use at your own risk.
