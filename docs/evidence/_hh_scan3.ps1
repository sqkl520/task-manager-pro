Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("D:\Projects\Task Manager Pro\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
Write-Output "--- rows 79..91: count of non-white pixels in x=3..1246 ---"
for($y=79;$y -le 91;$y++){
  $n=0; $samples=@()
  for($x=3;$x -le 1246;$x++){ $c=$img.GetPixel($x,$y); if($c.R -ne 255 -or $c.G -ne 255 -or $c.B -ne 255){ $n++; if($samples.Count -lt 3){$samples+=($x.ToString()+":"+(Hex $c))} } }
  Write-Output ("y=$y nonwhite=$n  " + ($samples -join " "))
}
Write-Output "--- header band x>503 nonwhite counts y=92..135 ---"
for($y=92;$y -le 135;$y++){
  $n=0; $s=@()
  for($x=503;$x -le 1246;$x++){ $c=$img.GetPixel($x,$y); if($c.R -ne 255 -or $c.G -ne 255 -or $c.B -ne 255){ $n++; if($s.Count -lt 3){$s+=($x.ToString()+":"+(Hex $c))} } }
  if($n -gt 0){ Write-Output ("y=$y nonwhite=$n  " + ($s -join " ")) }
}
Write-Output "(done x>503)"
Write-Output "--- name col text bbox: scan y=110..132, x=3..235 for dark pixels ---"
$minx=9999;$maxx=-1;$miny=9999;$maxy=-1
for($y=110;$y -le 132;$y++){ for($x=3;$x -le 235;$x++){ $c=$img.GetPixel($x,$y); if($c.R -lt 120 -and $c.G -lt 120 -and $c.B -lt 120){ if($x -lt $minx){$minx=$x}; if($x -gt $maxx){$maxx=$x}; if($y -lt $miny){$miny=$y}; if($y -gt $maxy){$maxy=$y} } } }
Write-Output "name text bbox x=$minx..$maxx y=$miny..$maxy"
$img.Dispose()
