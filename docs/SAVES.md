# Test saves

Named saves for testing in the simulator. **No saves are included in this
repository** (they are game data): make your own (see "Adding a save") and keep
the pristine copies in `<work dir>\psy_saves\<name>\SavedGame` (`PSY_WORK_DIR`,
tools/README.md). Tests never change them. The names below are the ones the
harness knows; `$TestSaves` in `tools\drive.ps1` lists them.

| name | where | level | use it for |
|---|---|---|---|
| `gpc` | Whispering Rock: GPC & Wilderness, by the gate | `ca.CAGP` | the default: smoke test, stereo checks, head sweeps |
| `reception` | Whispering Rock: Reception Area, outdoors among the trees | `ca.CARE` | outdoor rendering, god mode rendering, culling problems |
| `circus` | Meat Circus: Tent City, one enemy near Raz | `mc.MCTC` | targeting (lock-on, aiming) and player actions |
| `lake` | Whispering Rock: Boathouse and Beach, at the lake | `ca.CABH` | water reflections |

The second level is called "circus" in this project, not by its full name.

## Using them

```powershell
. .\tools\drive.ps1
Run-Smoke -Save reception              # PASS/FAIL checks in that save
Quick-Vr -Save circus -Keep            # start there and leave the game running
Run-Vr -Save reception                 # start there and take screenshots
Psy-Sweep basic                        # then e.g. a head-pose sweep
Clean-Game                             # always last
```

`-Continue` without `-Save` means `gpc`.

## How it works

- `Restore-TestSave <name>` copies the save into the **test slot**, profile 2
  slot 5 (`$TestSlot` in drive.ps1). Keep your own saves out of that slot: it
  is overwritten by every test. An autosave during a test only changes slot 5.
- Continue (the Astralathe test mod picks it on the main menu) loads the slot
  that the profile file names, not the newest file. The 57-byte
  `profiles\profile 2\Profile 2- Raz` stores the last slot used at byte 0x12,
  and one in-use flag per slot (0..6) at bytes 0x32..0x38. If a slot is marked
  unused, the game resets that slot to an empty 61,440-byte file.
  `Set-TestProfile` points the profile at slot 5 for the run. Before that, it
  keeps the player's own file in `psy_saves\profile2_pretest`. `Stop-Psy` and
  `Clean-Game` put that file back. Back up `profiles\` yourself before the
  first test run.

## Adding a save

1. Make the save in the game (the mod may be uninstalled meanwhile), in
   profile 2, in any slot except 5.
2. Copy that `SavedGameN` to `psy_saves\<name>\SavedGame`.
3. Add `<name>` and its level to `$TestSaves` in `tools\drive.ps1`. Get the
   level from `astralathe.log` ("LOADING LEVEL xx.XXXX") after a run with
   `-Save <name>`.
4. Add a row to the table above.

The first text in a save file is its title (location and play time), for
example `Meat Circus - Tent City 08:21:08`.
