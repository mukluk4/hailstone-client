# Imperium fork of Hailstone

This is Hailstone with changes for the Imperium EQEmu server. It lives on the `imperium` branch of
[mukluk4/hailstone-client](https://github.com/mukluk4/hailstone-client). The branch is based on upstream `main`, whose
client code matches the 0.5.0-beta release. Each change is its own commit, so you can review or revert one without
touching the others.

## The changes

| # | Commit | What you get | What to do |
|---|---|---|---|
| 1 | Own HP includes item HP | The player window, inventory, HP bar, and target/group/extended-target windows (when you're in them) show the same HP as `#mystats` | Nothing |
| 2 | Names drop leading '#' | "#The Fabled Vulak`Aerr" shows as "The Fabled Vulak`Aerr" on nameplates, windows, combat text and MQ `CleanName` | Nothing |
| 3 | Potion Belt row | EQ menu > Inventory > **Potion Belt** opens the belt | Drop an item on a slot to set it; right-click the slot to use it |
| 4 | Updater off | The client no longer replaces itself with the official release | `update.check=1` in `hailstone.ini` turns it back on |
| 5 | Allow /stick | **Options > Combat > Allow /stick** (off) | Leave it off to block every `/stick` |
| 6 | Allow Auto Combat | **Options > Combat > Allow Auto Combat** (off) | Leave it off to keep the Auto Combat add-on from running |
| 7 | Face Target On Attack | **Options > Combat > Face Target On Attack** (off) | Leave it off and turning attack on won't turn you |
| 8 | This guide | | |

### 1. Item HP

EQEmu sends your own HP minus the HP from your gear. The original client adds that back, but upstream Hailstone
doesn't, so a character with 393,359 HP showed 131,323. This build adds it back the same way the original client does.
Mana and endurance were already right.

### 2. '#' in names

Imperium marks named NPCs with a leading `#`. Only what you see changes. MQ scripts still get the server's name from
`Spawn.Name`, and `Target.Named` still recognises these mobs.

### 5–7. The automation switches

Upstream Hailstone itself never moves you onto a mob. The 0.5.0 source has no automatic `/stick`. What it does have:

| Source | What it does | Switch |
|---|---|---|
| Turning auto attack on | Turns you to face the target (the original client does this too) | Face Target On Attack |
| The Auto Combat add-on (`autocombat` script) | Sends `/stick`, `/nav`, `/attack`, `/target`, `/assist`, `/cast`, `/useitem`, `/pet attack`, `/sit` and `/stand`, uses abilities, and holds the Ability Book's autoskill | Allow Auto Combat (blocks the whole script) |
| Any other script, hot button social or plugin that types `/stick` | Walks you to your target and keeps you there | Allow /stick |

When a blocked `/stick` is tried, chat shows **"[Stick] sticking is turned off in Options…"**. If you see that line,
something is still sending `/stick`. When a blocked Auto Combat start is tried, chat shows **"[Lua] autocombat is
turned off in Options"**.

The switches are also written to `hailstone.ini`:

```ini
combat.face_on_attack = 0
scripts.allow_autocombat = 0
nav.allow_stick = 0
```

These upstream options are unchanged and still do things for you. They're all in Options > Combat or Display:

| Option | Default | What it does |
|---|---|---|
| Auto Target XTarget Attackers | on | With no target, an NPC that attacks you becomes your target |
| Attack Until XTarget Is Clear | off | Auto attack stays on after a kill while the extended target list has mobs |
| Tab Skips Pets | on | Tab targets the nearest NPC and skips pets |
| `/follow` mesh help (`nav.follow`) | on | `/follow` walks around walls for you |
| Ability Book autoskill | per character | Uses abilities you put on its list while you fight |

## Using it with the ImperiumStats plugin

Change 1 already fixes HP. If you also run `imperiumstats`, its HP correction adds item HP a second time and HP shows
about double. Change 2 also makes the plugin's name stripping unnecessary. The plugin's other job, putting the
`#mystats` AC, ATK, haste, stats and resists in the inventory window, still helps. Until the plugin gets an HP-only
off switch, run it with `/imstats off` to use this build's HP, or don't run it.

## Install

1. Build (below), or use a package made from it.
2. Copy the contents of `build/hailstone/` over a Hailstone 0.5.0 folder, or into a new folder with that release's
   `ui`, `shaders` and other data folders.
3. Start `hailstone.exe`. The login screen corner says "Updates are off".

## Build (Windows)

You need VS 2022 (Build Tools is enough), CMake and Ninja. From an "x64 Native Tools" prompt, in a short path (deep
folders exceed Windows' 260-character limit while dependencies are fetched):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNMSC_BROWSER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DNMSC_BROWSER=OFF` leaves out the in-game browser (a few hundred MB of CEF); pages open in your normal browser.

## Keeping up with upstream

```
git fetch upstream
git rebase upstream/main
```

If a rebase conflicts, the conflict is in one feature's commit, and that commit names the files it touched.
