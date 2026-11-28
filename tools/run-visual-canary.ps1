$ErrorActionPreference = 'Stop'
py -3.11 "$PSScriptRoot\visual_canary.py" @args
exit $LASTEXITCODE
