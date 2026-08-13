# Official housing content pack

The public Housing Editor release contains the extension and this builder, not
Blizzard game data. The icon, model, texture, skin, and DB2 files must come from
an installation or extract that the person running the builder is entitled to use.
The builder never downloads, uploads, or includes those files in this repository.

It installs a directory-backed WXL patch at `Data/Patch-Housing.MPQ/`. WarcraftXL
scans that patch name, so it does not modify Blizzard's stock `Patch-*.MPQ` files.

## What is included

Given the prepared official input roots under `wow-housing-tools`, the builder
includes:

- `retail-decor-raw`: stock decor M2s and their dependencies;
- `retail-db2`: HouseDecor plus model/texture path DB2 tables; and
- `retail-housing-ui/retail-art`: official housing icons and panels.

It deliberately excludes `custom-assets` and `phase1-furniture`. Those folders
contain the Blender/Minecraft-derived content that must not be installed or bundled.
It also skips WMO root files: WMO placement is disabled in this extension and modern
WMO handling remains experimental. All supported furniture/prop M2 content is kept.

The older `phase1` fireplace experiment is deliberately not read: its root M2 files
overlap the complete decor export at the same canonical paths but have different
bytes. The complete `retail-decor-raw` export is authoritative, so the builder never
mixes revisions of a model or its dependencies.

## Build and install for this machine

Close the target WoW client first. From the Housing Editor install folder, run a
dry run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-OfficialHousingContent.ps1 `
  -AssetRoot C:\path\to\wow-housing-tools `
  -ClientRoot "C:\path\to\WoW-3.3.5a"
```

Review the generated `wxl-housing-content-plan-*.json`, then add `-Install` to
create `Data\Patch-Housing.MPQ`. The builder stages into a temporary sibling first,
then moves it into place only after all files copy successfully.

If a prior housing patch already exists, the builder refuses to overwrite it. Add
`-Replace` only when you want it moved into the client `Backups` directory.

## Hub limitation

WXL Hub can extract the extension ZIP but does not run arbitrary installers. That
is why the builder is shipped beside the DLL rather than being automatically run at
install time. A public GitHub release also must not redistribute the proprietary
game-content output. The resulting local `Patch-Housing.MPQ` is the complete
runtime pack for this owned client.
