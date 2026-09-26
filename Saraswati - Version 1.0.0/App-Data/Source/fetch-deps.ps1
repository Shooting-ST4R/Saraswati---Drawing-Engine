# Shallow-fetches every library listed in deps.txt into third_party\ at its pinned commit.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force third_party | Out-Null
foreach ($line in Get-Content deps.txt) {
  if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
  $dir, $url, $commit = $line -split '\s+'
  $dest = "third_party\$dir"
  if (Test-Path "$dest\.git") {
    $head = git -C $dest rev-parse HEAD
    if ($head -eq $commit) { Write-Host "${dir}: already at $commit"; continue }
  }
  if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
  New-Item -ItemType Directory -Force $dest | Out-Null
  git -C $dest init -q
  git -C $dest remote add origin $url
  git -C $dest fetch -q --depth 1 origin $commit
  git -C $dest checkout -q FETCH_HEAD
  Write-Host "${dir}: fetched $commit"
}
