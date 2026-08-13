[CmdletBinding()]
param(
    # Root that contains the three independently acquired official source folders:
    # retail-decor-raw, retail-db2, and retail-housing-ui.
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$AssetRoot,

    # The WoW 3.3.5a client root.  Content is staged into
    # Data\Patch-Housing.MPQ, which WarcraftXL scans as a directory-backed patch.
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ClientRoot,

# Planning is the default.  This switch is required before any client files
# are created, so the source inventory can be checked first. The plan report is
# written beside this builder unless the caller selects another report path.
    [switch]$Install,

    # Allows an existing Patch-Housing.MPQ directory to be replaced only after it
    # has been moved aside into the client backup folder.
    [switch]$Replace,

    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PatchDirectoryName = 'Patch-Housing.MPQ'
$ExcludedSourceDirectories = @('custom-assets', 'phase1-furniture')
$ExcludedExtensions = @('.wmo')

function Get-NormalizedFullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Get-RelativePath([string]$Root, [string]$Path) {
    $rootWithSeparator = $Root.TrimEnd([char[]]@([char]'\', [char]'/' )) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $Path.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside its declared source root: $Path"
    }
    return $Path.Substring($rootWithSeparator.Length).Replace('/', '\')
}

function Assert-SafeRelativePath([string]$RelativePath) {
    $normalized = $RelativePath.Replace('/', '\').TrimStart([char[]]@([char]'\'))
    if ([string]::IsNullOrWhiteSpace($normalized) -or
        [System.IO.Path]::IsPathRooted($normalized) -or
        $normalized -match '(^|\\)\.\.(\\|$)') {
        throw "Unsafe content destination path: $RelativePath"
    }
    return $normalized
}

function Read-PathTable([string]$Path) {
    $data = [System.IO.File]::ReadAllBytes($Path)
    if ($data.Length -lt 92 -or [System.Text.Encoding]::ASCII.GetString($data, 0, 4) -ne 'WDC1') {
        throw "Expected a WDC1 path table: $Path"
    }

    $recordCount = [System.BitConverter]::ToUInt32($data, 4)
    $fieldCount = [System.BitConverter]::ToUInt32($data, 8)
    $recordSize = [System.BitConverter]::ToUInt32($data, 12)
    $stringSize = [System.BitConverter]::ToUInt32($data, 16)
    if ($fieldCount -ne 2 -or $recordSize -ne 8) {
        throw "Unexpected path-table layout in $Path (fields=$fieldCount recordSize=$recordSize)"
    }

    $recordBase = 92
    $stringBase = $recordBase + [int64]$recordCount * $recordSize
    if ($stringBase + $stringSize -gt $data.Length) {
        throw "Path-table record or string range exceeds file length: $Path"
    }

    $result = @{}
    for ($index = 0; $index -lt $recordCount; ++$index) {
        $offset = $recordBase + $index * $recordSize
        $fileDataId = [System.BitConverter]::ToUInt32($data, $offset)
        $stringOffset = [System.BitConverter]::ToUInt32($data, $offset + 4)
        if ($fileDataId -eq 0 -or $stringOffset -ge $stringSize) { continue }

        $start = [int]($stringBase + $stringOffset)
        $end = [System.Array]::IndexOf($data, [byte]0, $start)
        if ($end -lt $start -or $end -gt $stringBase + $stringSize) { continue }

        $relative = [System.Text.Encoding]::UTF8.GetString($data, $start, $end - $start).Replace('/', '\')
        if (-not [string]::IsNullOrWhiteSpace($relative)) {
            $result[[uint32]$fileDataId] = (Assert-SafeRelativePath $relative)
        }
    }
    return $result
}

function Test-SameBytes([string]$Left, [string]$Right) {
    $leftItem = Get-Item -LiteralPath $Left
    $rightItem = Get-Item -LiteralPath $Right
    if ($leftItem.Length -ne $rightItem.Length) { return $false }
    return (Get-FileHash -LiteralPath $Left -Algorithm SHA256).Hash -eq
           (Get-FileHash -LiteralPath $Right -Algorithm SHA256).Hash
}

function Get-ClientWowProcess([string]$ResolvedClientRoot) {
    Get-CimInstance Win32_Process -Filter "Name = 'Wow.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ExecutablePath -and
            $_.ExecutablePath.StartsWith($ResolvedClientRoot, [System.StringComparison]::OrdinalIgnoreCase)
        }
}

$assetRoot = Get-NormalizedFullPath $AssetRoot
$clientRoot = Get-NormalizedFullPath $ClientRoot
$decorRoot = Join-Path $assetRoot 'retail-decor-raw'
$db2Root = Join-Path $assetRoot 'retail-db2'
$uiRoot = Join-Path $assetRoot 'retail-housing-ui\retail-art'
$missing = @($decorRoot, $db2Root, $uiRoot) | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Container) }
if ($missing) {
    throw "Official source folders are missing: $($missing -join '; ')"
}

# This guard is deliberate: only explicitly named official input roots are read.
# Nothing from custom-assets or phase1-furniture can enter the plan.
foreach ($excluded in $ExcludedSourceDirectories) {
    $candidate = Join-Path $assetRoot $excluded
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        Write-Verbose "Excluded custom source root present but not read: $candidate"
    }
}

$patchRoot = Join-Path (Join-Path $clientRoot 'Data') $PatchDirectoryName
$modelPaths = Read-PathTable (Join-Path $db2Root 'ModelFilePath.db2')
$texturePaths = Read-PathTable (Join-Path $db2Root 'TextureFilePath.db2')
foreach ($entry in $texturePaths.GetEnumerator()) { $modelPaths[$entry.Key] = $entry.Value }

$plan = [System.Collections.Generic.List[object]]::new()
$byDestination = @{}
$duplicateSources = [System.Collections.Generic.List[object]]::new()
$skippedWmos = [System.Collections.Generic.List[string]]::new()

function Add-ContentPlan([string]$Source, [string]$RelativeDestination, [string]$Kind) {
    $relative = Assert-SafeRelativePath $RelativeDestination
    $extension = [System.IO.Path]::GetExtension($relative).ToLowerInvariant()
    if ($ExcludedExtensions -contains $extension) {
        $script:skippedWmos.Add($relative)
        return
    }

    $key = $relative.ToLowerInvariant()
    if ($script:byDestination.ContainsKey($key)) {
        $existing = $script:byDestination[$key]
        if (-not (Test-SameBytes $existing.Source $Source)) {
            throw "Conflicting official sources for ${relative}: $($existing.Source) vs $Source"
        }
        $script:duplicateSources.Add([pscustomobject]@{ destination = $relative; existing = $existing.Source; duplicate = $Source })
        return
    }

    $item = Get-Item -LiteralPath $Source -ErrorAction Stop
    $entry = [pscustomobject]@{
        Source = $item.FullName
        RelativeDestination = $relative
        Kind = $Kind
        Bytes = [int64]$item.Length
    }
    $script:byDestination[$key] = $entry
    $script:plan.Add($entry)
}

# Exported root models in decor/ have friendly filenames.  Their adjacent manifests
# bind each FileDataID back to the canonical ModelFilePath destination.
$friendlyDecorRoot = Join-Path $decorRoot 'decor'
Get-ChildItem -LiteralPath $friendlyDecorRoot -Filter '*.manifest.json' -File | Sort-Object Name | ForEach-Object {
    $manifest = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    $fileDataId = [uint32]$manifest.fileDataID
    if (-not $modelPaths.ContainsKey($fileDataId)) {
        throw "No canonical ModelFilePath for decor FileDataID $fileDataId in $($_.Name)"
    }
    $destination = $modelPaths[$fileDataId]
    if ([System.IO.Path]::GetExtension($destination).ToLowerInvariant() -ne '.m2') {
        # WMO placement is disabled in this extension and a known modern-WMO
        # compatibility issue remains.  It is intentionally excluded from the pack.
        $skippedWmos.Add($destination)
        return
    }
    $stem = $_.Name.Substring(0, $_.Name.Length - '.manifest.json'.Length)
    $source = Join-Path $friendlyDecorRoot ($stem + [System.IO.Path]::GetExtension($destination))
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing exported root model for $($_.Name): $source"
    }
    Add-ContentPlan $source $destination 'decor-root'
}

# Dependencies already use their canonical game-relative paths.  The friendly-name
# decor directory was processed above and is not copied verbatim.
Get-ChildItem -LiteralPath $decorRoot -Recurse -File | ForEach-Object {
    if ($_.FullName.StartsWith($friendlyDecorRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return }
    Add-ContentPlan $_.FullName (Get-RelativePath $decorRoot $_.FullName) 'decor-dependency'
}

# These DB2 tables let wxl-db2 resolve HouseDecor, model, texture and thumbnail IDs.
Get-ChildItem -LiteralPath $db2Root -File -Filter '*.db2' | ForEach-Object {
    Add-ContentPlan $_.FullName (Join-Path 'DBFilesClient' $_.Name) 'db2'
}

# The extracted official housing art contains icons and panels.  It is used as-is;
# retail Lua/XML source is deliberately not copied because the extension uses ImGui.
Get-ChildItem -LiteralPath $uiRoot -Recurse -File | ForEach-Object {
    Add-ContentPlan $_.FullName (Get-RelativePath $uiRoot $_.FullName) 'ui-art'
}

$summary = [ordered]@{
    schema = 'wxl-housing-official-content/1'
    generatedAt = (Get-Date).ToUniversalTime().ToString('o')
    installRequested = [bool]$Install
    assetRoot = $assetRoot
    clientRoot = $clientRoot
    patchRoot = $patchRoot
    plannedFiles = $plan.Count
    plannedBytes = [int64](($plan | Measure-Object Bytes -Sum).Sum)
    byKind = @{}
    excludedSourceDirectories = $ExcludedSourceDirectories
    skippedModernWmoFiles = $skippedWmos.Count
    duplicateOfficialFiles = $duplicateSources.Count
}
foreach ($group in ($plan | Group-Object Kind)) {
    $summary.byKind[$group.Name] = [ordered]@{
        files = $group.Count
        bytes = [int64](($group.Group | Measure-Object Bytes -Sum).Sum)
    }
}

if (-not $ReportPath) {
    $reportRoot = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
    $ReportPath = Join-Path $reportRoot ("wxl-housing-content-plan-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8

Write-Host ("Planned {0:N0} official files ({1:N2} GiB) for {2}" -f $plan.Count, ($summary.plannedBytes / 1GB), $patchRoot)
Write-Host ("Excluded source roots: {0}" -f ($ExcludedSourceDirectories -join ', '))
Write-Host ("Skipped modern WMO files: {0:N0}; report: {1}" -f $skippedWmos.Count, $ReportPath)

if (-not $Install) {
    Write-Host 'Dry run only. Re-run with -Install after reviewing the report.'
    exit 0
}

$runningClient = @(Get-ClientWowProcess $clientRoot)
if ($runningClient.Count -gt 0) {
    throw "Close Wow.exe for this client before installation (PID(s): $($runningClient.ProcessId -join ', '))."
}

$dataRoot = Split-Path -Parent $patchRoot
if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
    throw "Client Data directory not found: $dataRoot"
}

$backupRoot = Join-Path $clientRoot 'Backups'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (Test-Path -LiteralPath $patchRoot) {
    if (-not $Replace) {
        throw "Existing housing patch found at $patchRoot. Use -Replace to move it to a timestamped backup first."
    }
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backupPath = Join-Path $backupRoot ("$PatchDirectoryName.before-official-content-$stamp")
    Move-Item -LiteralPath $patchRoot -Destination $backupPath
    Write-Host "Moved existing patch to $backupPath"
}

$stagingRoot = Join-Path $dataRoot (".$PatchDirectoryName.build-$stamp")
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
try {
    $index = 0
    foreach ($entry in $plan) {
        $destination = Join-Path $stagingRoot $entry.RelativeDestination
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $entry.Source -Destination $destination -Force
        $index++
        if (($index % 500) -eq 0) { Write-Host "Copied $index / $($plan.Count) files" }
    }
    $summary.installedAt = (Get-Date).ToUniversalTime().ToString('o')
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stagingRoot 'wxl-housing-official-content.json') -Encoding UTF8
    Move-Item -LiteralPath $stagingRoot -Destination $patchRoot
    Write-Host "Installed official housing content at $patchRoot"
}
catch {
    if (Test-Path -LiteralPath $stagingRoot) {
        Write-Warning "Incomplete staging was retained for inspection: $stagingRoot"
    }
    throw
}
