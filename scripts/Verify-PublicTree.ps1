[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$forbiddenNames = @('decor.json')
$forbiddenExtensions = @('.m2', '.mdx', '.wmo', '.blp', '.dbc', '.db2', '.mpq')
$violations = Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
    Where-Object {
        $_.Name -in $forbiddenNames -or $_.Extension.ToLowerInvariant() -in $forbiddenExtensions
    }

if ($violations) {
    $violations | ForEach-Object { Write-Error "Public release must not include game/custom asset: $($_.FullName)" }
    throw 'Public-tree asset audit failed.'
}

Write-Host 'PUBLIC_TREE_OK'
