Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("D:\Projects\Task Manager Pro\shots\reference\original_win10.png")
Write-Output ("size=" + $img.Width + "x" + $img.Height)
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }

# 1. vertical scan of column x=1200 (right blank area) to find structural bands
Write-Output "--- vertical scan x=1200 (transitions) ---"
$prev=""
for($y=0;$y -lt 220;$y++){
  $c=$img.GetPixel(1200,$y); $h=Hex $c
  if($h -ne $prev){ Write-Output ("y=$y  $h"); $prev=$h }
}
Write-Output "--- vertical scan x=8 (left edge, name col) ---"
$prev=""
for($y=0;$y -lt 180;$y++){
  $c=$img.GetPixel(8,$y); $h=Hex $c
  if($h -ne $prev){ Write-Output ("y=$y  $h"); $prev=$h }
}
$img.Dispose()
