Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("D:\Projects\Task Manager Pro\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
Write-Output "--- group1 x=3..12 y=136..173 non-white count (triangle check) ---"
$n=0; $samp=@()
for($y=136;$y -le 173;$y++){ for($x=3;$x -le 12;$x++){ $c=$img.GetPixel($x,$y); if(-not($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255)){ $n++; if($samp.Count -lt 5){$samp+=("$x,$y="+(Hex $c))} } } }
Write-Output ("nonwhite=$n  " + ($samp -join " "))
Write-Output "--- group1 text leftmost x ---"
$mn=9999
for($y=136;$y -le 173;$y++){ for($x=0;$x -le 235;$x++){ $c=$img.GetPixel($x,$y); if(-not($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255) -and -not($c.R -eq 0xEA -and $c.G -eq 0xD5)){ if($x -lt $mn){$mn=$x} } } }
Write-Output ("leftmost non-white (excl heat line) x=$mn")
Write-Output "--- group2 x=3..12 y=482..519 non-white count ---"
$n=0
for($y=482;$y -le 519;$y++){ for($x=3;$x -le 12;$x++){ $c=$img.GetPixel($x,$y); if(-not($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255)){ $n++ } } }
Write-Output ("nonwhite=$n")
Write-Output "--- row band arithmetic: find all y where x=380 is #FFF9E4 (group rows) ---"
$ys=@()
for($y=136;$y -le 800;$y++){ $c=$img.GetPixel(380,$y); if($c.R -eq 0xFF -and $c.G -eq 0xF9 -and $c.B -eq 0xE4){ $ys+=$y } }
# compress to ranges
$s=$ys[0]; $p=$ys[0]
foreach($y in $ys){ if($y -ne $p+1){ Write-Output ("range {0}..{1} (h={2})" -f $s,$p,($p-$s+1)); $s=$y }; $p=$y }
Write-Output ("range {0}..{1} (h={2})" -f $s,$p,($p-$s+1))
$img.Dispose()
