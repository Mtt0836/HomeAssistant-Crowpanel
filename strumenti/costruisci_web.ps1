<#
  Comprime la pagina dell'editor del pannello (<progetto>/web/panel.html) e la mette
  dove serve:
    - <progetto>/components/apps/home_dashboard/assets/panel.html.gz
      copia di riserva compilata nel firmware (serve un idf.py build per averla
      sul pannello)
    - <progetto>/web/panel.html.gz
      copia da mettere sulla SD, nella cartella "web": il pannello preferisce
      questa, cosi' si aggiorna la pagina senza riflashare

  Uso:
    doppio clic su costruisci_web.bat
    oppure: powershell -ExecutionPolicy Bypass -File costruisci_web.ps1 [E:]
  Passando la lettera della SD, la copia ci finisce direttamente dentro.
#>
param([string] $Sd)

$radice  = Split-Path (Split-Path $PSCommandPath)      # D:\HA_DISPLAY
# Il progetto attivo in un posto solo: prima 'V0.2' era scritto tre volte qui
# dentro, e dopo il passaggio a V0.3 lo script comprimeva ancora la pagina
# vecchia senza dire niente.
$progetto = 'V0.3'
$sorgente = Join-Path $radice "$progetto\web\panel.html"
$uscite = @(
    (Join-Path $radice "$progetto\components\apps\home_dashboard\assets\panel.html.gz"),
    (Join-Path $radice "$progetto\web\panel.html.gz")
)

if (-not (Test-Path $sorgente)) {
    Write-Host "Non trovo $sorgente" -ForegroundColor Red
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($sorgente)
foreach ($u in $uscite) {
    New-Item -ItemType Directory -Force (Split-Path $u) | Out-Null
    $out = [System.IO.File]::Create($u)
    $gz  = New-Object System.IO.Compression.GZipStream($out, [System.IO.Compression.CompressionLevel]::Optimal)
    $gz.Write($bytes, 0, $bytes.Length)
    $gz.Dispose(); $out.Dispose()
    $kb = [math]::Round((Get-Item $u).Length / 1KB, 1)
    Write-Host ("{0}  ({1} KB)" -f $u, $kb) -ForegroundColor Green
}

if ($Sd) {
    $dest = Join-Path $Sd 'web'
    try {
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item $uscite[1] (Join-Path $dest 'panel.html.gz') -Force
        Write-Host "Copiata anche su $dest" -ForegroundColor Green
    } catch {
        Write-Host ("Copia sulla SD non riuscita: {0}" -f $_.Exception.Message) -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "Sulla SD va in una cartella 'web' (SD:\web\panel.html.gz)."
Write-Host "Senza SD il pannello usa la copia dentro il firmware: in quel caso"
Write-Host "serve ricompilare con idf.py build e riflashare."
