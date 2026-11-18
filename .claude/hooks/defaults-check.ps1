# PostToolUse hook (Edit|Write): after a settings-defaults fragment changes, check that
# resource\VegasShaders.defaults.toml still matches the fragments. Exit 2 sends the message back to Claude.
$ErrorActionPreference = 'Stop'
try { $evt = [Console]::In.ReadToEnd() | ConvertFrom-Json } catch { exit 0 }
$path = [string]$evt.tool_input.file_path
if (-not $path) { exit 0 }
$p = $path -replace '/', '\'
if ($p -notmatch '\\src\\effects\\[^\\]+\\defaults\.toml$' -and $p -notmatch '\\resource\\defaults\\[^\\]+\.toml$' -and
    $p -notmatch '\\resource\\defaults\\ORDER\.txt$') { exit 0 }
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repo
try { $out = & py -3.10 -I tools\defaults_tool.py check . 2>&1; $code = $LASTEXITCODE } finally { Pop-Location }
if ($code -ne 0) {
    [Console]::Error.WriteLine("Settings defaults are out of date after editing $path. Run: py -3.10 -I tools\defaults_tool.py merge .  (check said: $($out -join ' '))")
    exit 2
}
exit 0
