[CmdletBinding()]
param(
    [string]$Root
)

if (-not $Root) {
    $Root = Split-Path -Parent $PSScriptRoot
}

$ErrorActionPreference = 'Stop'
$forbiddenNames = @('decor.json')
$forbiddenExtensions = @('.m2', '.mdx', '.wmo', '.blp', '.dbc', '.db2', '.mpq', '.zip')
$textExtensions = @('.md', '.ps1', '.py', '.json', '.yml', '.yaml', '.cmake', '.cpp', '.hpp', '.h', '.txt')
$violations = Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
    Where-Object {
        $_.FullName -notmatch '\\(\.git|artifacts|build|dist|stage)\\' -and
        ($_.Name -in $forbiddenNames -or $_.Extension.ToLowerInvariant() -in $forbiddenExtensions)
    }

if ($violations) {
    $violations | ForEach-Object { Write-Error "Public release must not include game/custom asset: $($_.FullName)" }
    throw 'Public-tree asset audit failed.'
}

$localPathHits = Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
    Where-Object {
        $_.FullName -notmatch '\\(\.git|artifacts|build|dist|stage)\\' -and
        $textExtensions -contains $_.Extension.ToLowerInvariant()
    } |
    Select-String -Pattern 'C:\\Users\\' -ErrorAction Stop
if ($localPathHits) {
    $localPathHits | ForEach-Object {
        Write-Error "Public release must not include a local user path: $($_.Path):$($_.LineNumber)"
    }
    throw 'Public-tree local-path audit failed.'
}

Write-Host 'PUBLIC_TREE_OK'
