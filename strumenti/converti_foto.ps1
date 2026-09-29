<#
  Riduce le foto alla dimensione dello schermo del pannello (1024x600) e le
  salva come JPEG, pronte per la SD.

  Perche' serve: il P4 decodifica il JPEG in hardware, ma deve decomprimere la
  foto a piena risoluzione in PSRAM. Una foto da 12 Mpixel (4032x3024) vuole
  23 MB e non ci sta. Ridotte, si aprono all'istante.

  Uso:
    powershell -ExecutionPolicy Bypass -File converti_foto.ps1 <file o cartella>
  Doppio clic sul .bat: chiede la cartella con una finestra.
  Oppure trascina file/cartelle su converti_foto.bat.
  Le foto convertite finiscono nella sottocartella "per_pannello": copia quelle
  sulla SD (nella radice oppure in una cartella "foto").
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

Add-Type -AssemblyName System.Drawing

$Larghezza = 1024
$Altezza   = 600
$Qualita   = 88L

$estensioni = '.jpg', '.jpeg', '.png', '.bmp', '.heic', '.webp'
$file = foreach ($i in $Ingressi) {
    if (Test-Path $i -PathType Container) {
        Get-ChildItem $i -File | Where-Object { $estensioni -contains $_.Extension.ToLower() }
    } elseif (Test-Path $i) {
        Get-Item $i
    }
}
if (-not $file) { Write-Host "Nessuna foto da convertire."; exit 1 }

$uscita = Join-Path (Split-Path $file[0].FullName) "per_pannello"
New-Item -ItemType Directory -Force $uscita | Out-Null

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$par = New-Object System.Drawing.Imaging.EncoderParameters 1
$par.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), $Qualita

$ok = 0; $ko = 0
foreach ($f in $file) {
    try {
        $img = [System.Drawing.Image]::FromFile($f.FullName)

        # Orientamento EXIF (foto scattate col telefono ruotato)
        if ($img.PropertyIdList -contains 274) {
            switch ($img.GetPropertyItem(274).Value[0]) {
                3 { $img.RotateFlip([System.Drawing.RotateFlipType]::Rotate180FlipNone) }
                6 { $img.RotateFlip([System.Drawing.RotateFlipType]::Rotate90FlipNone) }
                8 { $img.RotateFlip([System.Drawing.RotateFlipType]::Rotate270FlipNone) }
            }
        }

        # Riduzione proporzionale: la foto entra nello schermo senza deformarsi
        $scala = [Math]::Min($Larghezza / $img.Width, $Altezza / $img.Height)
        if ($scala -gt 1) { $scala = 1 }          # le foto piccole restano come sono
        $w = [int][Math]::Round($img.Width * $scala / 2) * 2
        $h = [int][Math]::Round($img.Height * $scala / 2) * 2

        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($img, 0, 0, $w, $h)
        $g.Dispose()

        $dest = Join-Path $uscita ($f.BaseName + ".jpg")
        $bmp.Save($dest, $codec, $par)
        $bmp.Dispose(); $img.Dispose()

        $kb = [math]::Round((Get-Item $dest).Length / 1KB)
        Write-Host ("{0,-40} {1}x{2}  {3} KB" -f $f.Name, $w, $h, $kb) -ForegroundColor Green
        $ok++
    } catch {
        Write-Host ("{0,-40} errore: {1}" -f $f.Name, $_.Exception.Message) -ForegroundColor Red
        $ko++
    }
}
Write-Host ""
Write-Host "Convertite $ok foto (errori: $ko) in: $uscita"
Write-Host "Copiale sulla SD, nella radice o in una cartella 'foto'."
