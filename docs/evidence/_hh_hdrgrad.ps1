Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("D:\Projects\Task Manager Pro\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
foreach($x in 236,356,429,502){
  Write-Output ("x={0}: y92={1} y100={2} y113={3} y127={4} y134={5}" -f $x,(Hex $img.GetPixel($x,92)),(Hex $img.GetPixel($x,100)),(Hex $img.GetPixel($x,113)),(Hex $img.GetPixel($x,127)),(Hex $img.GetPixel($x,134)))
}
Write-Output "--- is the gradient linear? deltas at x=236 ---"
$vals=@()
for($y=92;$y -le 134;$y++){ $c=$img.GetPixel(236,$y); $vals += $c.R }
Write-Output ("first=" + $vals[0] + " last=" + $vals[$vals.Count-1] + " span=" + ($vals[0]-$vals[$vals.Count-1]) + " steps=" + ($vals.Count-1))
$diffs=@(); for($i=1;$i -lt $vals.Count;$i++){ $diffs += ($vals[$i-1]-$vals[$i]) }
Write-Output ("diffs: " + ($diffs -join ","))
$img.Dispose()
