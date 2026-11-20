Add-Type -AssemblyName System.Drawing
$p = "C:\Users\ADMINI~1\AppData\Local\Temp\claude\D--Projects-Task-Manager-Pro\57af6178-5dab-4941-9a4a-b9f30c9cd679\images\4.png"
$bmp = [System.Drawing.Bitmap]::FromFile($p)
$W=$bmp.Width
# profile: for each row, count dark pixels in the name column text area x=60..230 (avoid icons)
$prof = New-Object int[] 704
for ($y=110; $y -lt 640; $y++) {
  $cnt=0
  for ($x=100; $x -lt 230; $x++) {
    $c=$bmp.GetPixel($x,$y)
    if (($c.R+$c.G+$c.B) -lt 520) { $cnt++ }
  }
  $prof[$y]=$cnt
}
# find local peaks
"--- peaks in text profile ---"
$peaks=@()
for ($y=115; $y -lt 635; $y++) {
  if ($prof[$y] -ge 6 -and $prof[$y] -ge $prof[$y-1] -and $prof[$y] -ge $prof[$y+1]) { $peaks += $y }
}
"peaks: " + ($peaks -join ',')
"--- deltas ---"
for ($i=1; $i -lt $peaks.Count; $i++) { $peaks[$i]-$peaks[$i-1] }
$bmp.Dispose()
