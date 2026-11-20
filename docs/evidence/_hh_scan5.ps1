Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("D:\Projects\Task Manager Pro\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
Write-Output "--- per-y nonwhite count in each column's header cell (x ranges) ---"
$ranges=@(@(0,235,"name"),@(237,355,"status"),@(357,428,"cpu"),@(430,501,"mem"))
foreach($y in 92..134){
  $line = "y={0,3} " -f $y
  foreach($r in $ranges){
    $n=0; $mn=9999;$mx=-1
    for($x=$r[0];$x -le $r[1];$x++){ $c=$img.GetPixel($x,$y); if(-not ($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255)){ $n++; if($x -lt $mn){$mn=$x}; if($x -gt $mx){$mx=$x} } }
    $line += ("{0}:n={1}({2}..{3}) " -f $r[2],$n,$mn,$mx)
  }
  Write-Output $line
}
$img.Dispose()
