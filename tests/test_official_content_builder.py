"""Static release gates for the legal, source-separated housing content builder."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILDER = (ROOT / "tools" / "Build-OfficialHousingContent.ps1").read_text(encoding="utf-8")
GUIDE = (ROOT / "docs" / "OfficialContent.md").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")


class OfficialContentBuilderTests(unittest.TestCase):
    def test_builder_has_only_explicit_official_inputs(self) -> None:
        for token in (
            "retail-decor-raw",
            "retail-db2",
            "retail-housing-ui\\retail-art",
            "Patch-Housing.MPQ",
        ):
            self.assertIn(token, BUILDER)

    def test_runtime_reads_official_art_from_the_local_housing_patch(self) -> None:
        catalog = (ROOT / "src" / "Catalog.cpp").read_text(encoding="utf-8")
        self.assertIn("Data\\\\Patch-Housing.MPQ\\\\interface\\\\housing", catalog)
        self.assertIn("std::string(kOfficialHousingPatch) + row.thumbPath", catalog)
        self.assertIn("const std::string housingPatch", catalog)

    def test_custom_blender_and_wmo_paths_are_excluded(self) -> None:
        self.assertIn("$ExcludedSourceDirectories = @('custom-assets', 'phase1-furniture')", BUILDER)
        self.assertIn("$ExcludedExtensions = @('.wmo')", BUILDER)
        self.assertIn("WMO placement is disabled", BUILDER)
        self.assertIn("Blender/Minecraft-derived", GUIDE)
        self.assertIn("mixes revisions of a model", GUIDE)

    def test_builder_is_dry_run_by_default_and_client_safe(self) -> None:
        self.assertIn("if (-not $Install)", BUILDER)
        self.assertIn("Close Wow.exe for this client before installation", BUILDER)
        self.assertIn("Existing housing patch found", BUILDER)
        self.assertIn(".$PatchDirectoryName.build-", BUILDER)
        self.assertIn("$reportRoot = if ($PSScriptRoot)", BUILDER)
        self.assertNotIn(
            "$ReportPath = Join-Path $clientRoot (\"wxl-housing-content-plan-",
            BUILDER,
        )

    def test_release_ships_builder_and_guide_but_not_content(self) -> None:
        self.assertIn("Copy-Item 'ext/tools', 'ext/docs' -Destination $stage -Recurse", WORKFLOW)
        self.assertIn("does not run arbitrary installers", GUIDE)
        self.assertIn("must not redistribute the proprietary", GUIDE)


if __name__ == "__main__":
    unittest.main(verbosity=2)
