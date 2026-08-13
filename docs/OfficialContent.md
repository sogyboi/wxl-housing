# Official housing content pack

Housing Editor ships the extension through WXL Hub and the official content as
separate [GitHub Release downloads](https://github.com/sogyboi/wxl-housing/releases/latest). The content is deliberately kept out of the
source repository and the Hub ZIP because it is large. Download every
`wxl-housing-content-<version>-*.zip` asset from the matching GitHub Release,
close WoW, and extract all archives into the target client's `Data` directory.

After extraction, the target must look like this:

```text
<WoW>\\Data\\Patch-Housing.MPQ\\world\\...
<WoW>\\Data\\Patch-Housing.MPQ\\interface\\housing\\...
<WoW>\\Data\\Patch-Housing.MPQ\\DBFilesClient\\...
```

Do not extract into `Extensions`, and do not leave an extra nested
`Patch-Housing.MPQ` directory. The release includes a SHA-256 manifest to verify
each downloaded archive before extraction.

It installs a directory-backed WXL patch at `Data/Patch-Housing.MPQ/`. WarcraftXL
scans that patch name, so it does not modify Blizzard's stock `Patch-*.MPQ` files.
Housing Editor reads its official category atlas, catalog panel, and preview PNGs
directly from that local patch; no separate manual `Extensions/wxl-housing/ui` or
`thumbnails` copy is required.

## What is included

Given the prepared official input roots under `wow-housing-tools`, the builder
includes:

- `retail-decor-raw`: stock decor M2s and their dependencies;
- `retail-db2`: HouseDecor plus model/texture path DB2 tables; and
- `retail-housing-ui/retail-art`: official housing icons and panels.

It deliberately excludes `custom-assets` and `phase1-furniture`, including the
Blender/Minecraft-derived files from those roots. It also skips WMO root files:
WMO placement is disabled in this extension and modern WMO handling remains
experimental. All supported furniture/prop M2 content is kept.

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

WXL Hub installs only the extension ZIP under `Extensions/wxl-housing`; it
does not run arbitrary installers and does not extract release content into
`Data`. That is why
the content pack is a separate download with explicit `Data`-folder instructions.
The Hub ZIP itself must not redistribute the proprietary content pack. The
resulting local `Patch-Housing.MPQ` is the complete runtime pack.
