# 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对脚本自身
# 目录定位，便于在任何位置重跑。原路径记录在下方注释里。
# 原始：D:\Projects\Task Manager Pro\
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("$PSScriptRoot\..\..\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
Write-Output "--- group header row1 y=136..175 : vertical boundaries ---"
Write-Output "--- find group row1 start/end by scanning x=380 for #FFF9E4-ish vs white ---"
foreach($y in 132,133,134,135,136,173,174,175,176){
  Write-Output ("y={0}: x=100:{1} x=380:{2} x=470:{3} x=600:{4}" -f $y,(Hex $img.GetPixel(100,$y)),(Hex $img.GetPixel(380,$y)),(Hex $img.GetPixel(470,$y)),(Hex $img.GetPixel(600,$y)))
}
Write-Output "--- group2 rows: find y where x=380 becomes #FFF9E4 again (scan 460..530) ---"
foreach($y in 470,475,478,480,481,482,519,520,521){
  Write-Output ("y={0}: x=100:{1} x=380:{2} x=470:{3}" -f $y,(Hex $img.GetPixel(100,$y)),(Hex $img.GetPixel(380,$y)),(Hex $img.GetPixel(470,$y)))
}
Write-Output "--- #EAD5A0 vertical lines in data region: check x=236 at y=200/300 ---"
foreach($x in 236,356,429,502){ Write-Output ("x={0} y=200 {1}  y=300 {2}  y=600 {3}" -f $x,(Hex $img.GetPixel($x,200)),(Hex $img.GetPixel($x,300)),(Hex $img.GetPixel($x,600))) }
Write-Output "--- cell fill extents at y=200 ---"
$prev="";$start=0
for($x=350;$x -le 510;$x++){ $h=Hex $img.GetPixel($x,200); if($h -ne $prev){ if($prev){Write-Output ("{0}..{1} {2}" -f $start,($x-1),$prev)}; $prev=$h; $start=$x } }
Write-Output ("{0}..510 {1}" -f $start,$prev)
$img.Dispose()
