param(
    [Parameter(Mandatory = $true)][string]$HostRoot,
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$s6Repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$s6Host = [System.IO.Path]::GetFullPath($HostRoot)
$s6Plugin = Join-Path $s6Repository 'ue\MimirComposite'
$s6Template = Join-Path $s6Repository 'tools\ue_s6_host'
if (-not [System.IO.Path]::IsPathRooted($HostRoot) -or
    $s6Host.TrimEnd('\', '/') -eq [System.IO.Path]::GetPathRoot($s6Host).TrimEnd('\', '/') -or
    (Test-Path -LiteralPath $s6Host)) {
    throw 'HostRoot must be an explicit, fresh absolute directory; existing directories are never overwritten.'
}
if (-not (Test-Path -LiteralPath (Join-Path $s6Plugin 'MimirComposite.uplugin'))) {
    throw 'RepositoryRoot does not contain the MimirComposite plugin.'
}
New-Item -ItemType Directory -Path $s6Host | Out-Null
Get-ChildItem -LiteralPath $s6Template | Copy-Item -Destination $s6Host -Recurse
$s6Plugins = Join-Path $s6Host 'Plugins'
New-Item -ItemType Directory -Path $s6Plugins | Out-Null
New-Item -ItemType Junction -Path (Join-Path $s6Plugins 'MimirComposite') -Target $s6Plugin | Out-Null
Write-Output (Join-Path $s6Host 'MimirCompositeV5S6.uproject')
