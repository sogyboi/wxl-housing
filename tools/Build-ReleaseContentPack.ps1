[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PatchRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory,

[string]$Version = '0.8.2'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-ChildPath([string]$Root, [string]$Path) {
    $rootWithSeparator = $Root.TrimEnd([char[]]@([char]'\', [char]'/' )) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $Path.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside source root: $Path"
    }
}

function Invoke-ZipArchive([string]$SourceDirectory, [string]$ArchivePath) {
    $sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
    if (Test-Path -LiteralPath $sevenZip -PathType Leaf) {
        Push-Location $SourceDirectory
        try {
            & $sevenZip a -tzip -mx=5 $ArchivePath 'Patch-Housing.MPQ' | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE" }
        }
        finally {
            Pop-Location
        }
        return
    }

    Compress-Archive -LiteralPath (Join-Path $SourceDirectory 'Patch-Housing.MPQ') -DestinationPath $ArchivePath -CompressionLevel Optimal
}

$sourceRoot = Get-FullPath $PatchRoot
$outRoot = Get-FullPath $OutputDirectory
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "Patch root does not exist: $sourceRoot"
}
if ((Split-Path -Leaf $sourceRoot) -ne 'Patch-Housing.MPQ') {
    throw "Patch root must be the directory-backed Patch-Housing.MPQ folder: $sourceRoot"
}

$forbiddenSegments = @('custom-assets', 'phase1-furniture')
$forbiddenExtensions = @('.wmo')
$files = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force | Where-Object {
    $relative = $_.FullName.Substring($sourceRoot.Length).TrimStart([char[]]@([char]'\', [char]'/' ))
    $segments = $relative -split '[\\/]'
    (-not ($segments | Where-Object { $forbiddenSegments -contains $_ })) -and
    ($forbiddenExtensions -notcontains $_.Extension.ToLowerInvariant()) -and
    $_.Name -ne 'wxl-housing-official-content.json'
}

if ($files.Count -eq 0) { throw 'No eligible content files found.' }

New-Item -ItemType Directory -Path $outRoot -Force | Out-Null
$stageRoot = Join-Path $outRoot ('.stage-' + [guid]::NewGuid().ToString('N'))
$coreRoot = Join-Path $stageRoot 'core\Patch-Housing.MPQ'
$doodadRoot = Join-Path $stageRoot 'doodads\Patch-Housing.MPQ'
New-Item -ItemType Directory -Path $coreRoot, $doodadRoot -Force | Out-Null

try {
    foreach ($file in $files) {
        Assert-ChildPath $sourceRoot $file.FullName
        $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart([char[]]@([char]'\', [char]'/' ))
        $destinationRoot = if ($relative.StartsWith('world\expansion11\doodads\', [System.StringComparison]::OrdinalIgnoreCase)) { $doodadRoot } else { $coreRoot }
        $destination = Join-Path $destinationRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }

    # The local-builder manifest contains the machine-specific source and client
    # paths.  Never copy it into a published archive; supply a portable manifest
    # instead.
    $portableContentManifest = [ordered]@{
        schema = 'wxl-housing-release-content/1'
        version = $Version
        patchDirectory = 'Patch-Housing.MPQ'
        exclusions = [ordered]@{
            sourceRoots = $forbiddenSegments
            extensions = $forbiddenExtensions
        }
    }
    $portableContentManifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $coreRoot 'wxl-housing-content-manifest.json') -Encoding UTF8

    $manifest = [ordered]@{
        schema = 'wxl-housing-release-content/1'
        version = $Version
        extractTo = 'Data'
        patchDirectory = 'Patch-Housing.MPQ'
        archives = @()
        exclusions = [ordered]@{
            sourceRoots = $forbiddenSegments
            extensions = $forbiddenExtensions
        }
    }
    $archiveDefinitions = @(
        [pscustomobject]@{ Name = "wxl-housing-content-$Version-core.zip"; Root = (Join-Path $stageRoot 'core') },
        [pscustomobject]@{ Name = "wxl-housing-content-$Version-doodads.zip"; Root = (Join-Path $stageRoot 'doodads') }
    )
    foreach ($definition in $archiveDefinitions) {
        $target = Join-Path $outRoot $definition.Name
        if (Test-Path -LiteralPath $target) {
            throw "Refusing to overwrite existing release archive: $target"
        }
        Invoke-ZipArchive $definition.Root $target
        $archiveFiles = Get-ChildItem -LiteralPath $definition.Root -Recurse -File | Measure-Object -Property Length -Sum
        $entryCount = $archiveFiles.Count
        $entryBytes = [int64]$archiveFiles.Sum
        $manifest.archives += [ordered]@{
            file = $definition.Name
            sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            compressedBytes = (Get-Item -LiteralPath $target).Length
            extractedFiles = $entryCount
            extractedBytes = $entryBytes
            destination = 'Data\\Patch-Housing.MPQ'
        }
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outRoot "wxl-housing-content-$Version-SHA256.json") -Encoding UTF8
    Write-Host "CONTENT_RELEASE_PACK_OK: $outRoot"
}
finally {
    if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
}

