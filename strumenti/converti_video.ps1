<#
  Converte video di qualsiasi formato (MP4, MOV, MKV, AVI...) nel formato che il
  pannello riproduce nello slideshow: AVI con video MJPEG a 1024x600.

  Il P4 decodifica il JPEG in hardware, quindi l'MJPEG gira fluido a piena
  risoluzione; l'H.264 degli MP4 andrebbe decodificato in software (~4 fps a 720p).

  Uso:
    powershell -ExecutionPolicy Bypass -File converti_video.ps1 <file o cartella> [altri file...]
  I file convertiti finiscono in una sottocartella 'per_pannello'.
  Doppio clic sul .bat: chiede la cartella con una finestra.
  Oppure trascina file/cartelle su converti_video.bat.

  Il risultato va copiato sulla SD nella cartella delle foto (predefinita: /foto).
  Serve ffmpeg:  winget install Gyan.FFmpeg   (poi riapri la finestra)
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Ingressi
)

# Senza parametri (doppio clic sul .bat): finestra per scegliere la cartella.
if (-not $Ingressi) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Scegli la cartella con i file da convertire per il pannello"
    $dlg.ShowNewFolderButton = $false
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "Annullato."
        exit 1
    }
    $Ingressi = @($dlg.SelectedPath)
}

$Larghezza = 1024
$Altezza   = 600
$FpsMax    = 25      # oltre non serve: lo slideshow non e' un lettore video
$Qualita   = 5       # 2 = migliore/file grandi, 10 = peggiore/file piccoli
$DurataMax = 60      # secondi: le clip piu' lunghe vengono tagliate

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Host "ffmpeg non trovato. Installalo con:" -ForegroundColor Red
    Write-Host "    winget install Gyan.FFmpeg"
    Write-Host "poi chiudi e riapri questa finestra."
    exit 1
}

$estensioni = '.mp4', '.mov', '.mkv', '.avi', '.m4v', '.webm', '.3gp', '.wmv', '.mts'
$file = foreach ($i in $Ingressi) {
    if (Test-Path $i -PathType Container) {
        Get-ChildItem $i -File | Where-Object { $estensioni -contains $_.Extension.ToLower() }
    } elseif (Test-Path $i) {
        Get-Item $i
    }
}
if (-not $file) { Write-Host "Nessun video da convertire."; exit 1 }

$uscita = Join-Path (Split-Path $file[0].FullName) "per_pannello"
New-Item -ItemType Directory -Force $uscita | Out-Null

# Adatta al 1024x600 senza deformare: bande nere dove serve, fotogrammi pari.
$filtro = "scale=${Larghezza}:${Altezza}:force_original_aspect_ratio=decrease," +
          "pad=${Larghezza}:${Altezza}:(ow-iw)/2:(oh-ih)/2:black,fps=$FpsMax,format=yuvj420p"

foreach ($f in $file) {
    $dest = Join-Path $uscita ($f.BaseName + ".avi")
    Write-Host "-> $($f.Name)  =>  $dest"
    ffmpeg -hide_banner -loglevel error -y -i $f.FullName -t $DurataMax -vf $filtro `
           -c:v mjpeg -q:v $Qualita -an $dest
    if ($LASTEXITCODE -eq 0) {
        $mb = [math]::Round((Get-Item $dest).Length / 1MB, 1)
        Write-Host "   ok ($mb MB)" -ForegroundColor Green
    } else {
        Write-Host "   conversione fallita" -ForegroundColor Red
    }
}
Write-Host ""
Write-Host "Fatto. Copia i file di '$uscita' nella cartella 'foto' della SD."
