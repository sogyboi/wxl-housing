# WXL Housing Editor

Housing Editor is a WarcraftXL ABI 1.1 extension for WoW 3.3.5a build 12340.
It presents the compatible HouseDecor catalog, provides local M2 placement,
selection and transform editing, a bounded free build camera, and experimental
client-local terrain deformation.

## Install from WXL Hub

1. Install **wxl-db2** from WXL Hub first. Housing Editor requires its `wxl.db2`
   and `wxl.fdid` interfaces.
2. Install **Housing Editor** and restart the client when prompted.
3. Press **Insert** or click the round building launcher in game.

The Hub ZIP contains `wxl-housing.dll`, its manifest, documentation, and the
`tools/Build-OfficialHousingContent.ps1` local content builder. The optional
**Official Housing Content Pack** is published as [separate GitHub Release
downloads](https://github.com/sogyboi/wxl-housing/releases/latest) because it is too large for the Hub extension ZIP. Download every
`wxl-housing-content-0.8.2-*.zip` asset from the matching release, then extract
them into the WoW `Data` folder while the client is closed. The combined result
must be `Data/Patch-Housing.MPQ/...`, not a nested `Patch-Housing.MPQ` folder.

The content archives contain the official decor, UI art, preview PNGs, and DB2
sidecars required by this extension. They intentionally exclude
`custom-assets`, `phase1-furniture`, custom Blender/Minecraft-derived files,
and WMO root models.

## What it uses from the local client

Housing Editor refers to compatible data already installed by the user:

- `HouseDecor.db2` is opened through `wxl-db2`.
- `DBFilesClient/HouseDecorNames.tsv`, if locally available, gives catalog rows
  readable names. Missing names safely fall back to `Decor <row id>` labels.
- The official-content builder places retail UI art and preview PNGs in
  `Data/Patch-Housing.MPQ`; the editor reads them from there. It falls back to
  built-in placeholders only when that local patch is absent or incomplete.

Use the included [official content builder](docs/OfficialContent.md) instead if
you want to create the same pack from local source material. Both routes stage
the content in `Data/Patch-Housing.MPQ`. The published catalog contains no
custom M2/WMO object, no sample `decor.json`, and no hard-coded third-party
model row.

The source retains the optional local `decor.json` discovery path for people
who independently install their own licensed packages. Such packages are not
part of this project, are not needed for normal WoW decor use, and are their
installer's responsibility.

## Local behavior and limits

Placements are saved only on the client in
`Extensions/wxl-housing/placements.tsv`; `placements.tsv.bak` is used for
recovery. Nearby saved objects restore as terrain chunks load.

The catalog opens on placeable Furnishings and hides technical `[DNT]`/WMO rows.
Use **Filter** if you need to inspect every catalog row.

The free build camera is local and bounded around its activation point. Terrain
deformation modifies only the rendered loaded terrain: it does not rewrite MPQ
or ADT sources, server collision, navigation, or multiplayer state. The 65 WMO
catalog rows remain visible but disabled because arbitrary WMO placement needs a
different WarcraftXL spawn path.

## Build and release

The GitHub Actions workflow checks the source tests, builds against
`WarcraftXL/wxl-core` tag `v1.1`, and publishes a versioned ZIP release that
WXL Hub can install. To build locally, place this source tree in
`wxl-core/extensions/wxl-housing/` and configure wxl-core as Win32.

```powershell
cmake -S C:/path/to/wxl-core -B C:/path/to/wxl-core/build -A Win32
cmake --build C:/path/to/wxl-core/build --config Release --target wxl-housing
```

## Layout

| Path | Purpose |
|---|---|
| `src/` | Extension, catalog, placement, input, camera, and terrain code |
| `third_party/imguizmo/` | ImGuizmo source (MIT) |
| `tests/` | Source and manifest regression checks |
| `tools/Build-OfficialHousingContent.ps1` | Safe local builder for official content |
| `tools/Build-ReleaseContentPack.ps1` | Splits a completed content patch into release archives |
| `docs/OfficialContent.md` | Official content source, install, and limits |
| `store/description.md` | WXL Hub listing description |
| `.github/workflows/release.yml` | Win32 build and ZIP release automation |

License: GPL-3.0-or-later. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
