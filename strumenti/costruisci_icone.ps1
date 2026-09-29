# Rigenera il font delle icone di Home Assistant compilato nel firmware.
#
# Serve solo dopo aver aggiunto un nome a strumenti/icone/elenco.txt: i file
# generati stanno nel repository, quindi per compilare il progetto non serve
# ne' Node ne' questo script.
#
# La prima volta:
#   winget install OpenJS.NodeJS.LTS      (poi riapri il terminale)
#   npm install -g lv_font_conv

$ErrorActionPreference = 'Stop'
$qui = Split-Path -Parent $MyInvocation.MyCommand.Path
$icone = Join-Path $qui 'icone'

# Node non e' sempre nel percorso della sessione appena installato
foreach ($p in @("$env:ProgramFiles\nodejs", "$env:APPDATA\npm")) {
    if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) { $env:Path = "$p;$env:Path" }
}

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Host "Node non c'e'. Installalo con:  winget install OpenJS.NodeJS.LTS"
    exit 1
}
if (-not (Get-Command lv_font_conv -ErrorAction SilentlyContinue)) {
    Write-Host "Manca il convertitore. Installalo con:  npm install -g lv_font_conv"
    exit 1
}

# Il font di Material Design Icons: scaricato qui, non tenuto nel repository
if (-not (Test-Path (Join-Path $icone 'node_modules\@mdi\font\fonts\materialdesignicons-webfont.ttf'))) {
    Write-Host "Scarico Material Design Icons..."
    Push-Location $icone
    npm install @mdi/font --no-save --silent
    Pop-Location
}

python (Join-Path $icone 'costruisci.py')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Fatto. Ora ricompila il firmware con idf.py build."
