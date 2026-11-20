# 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对脚本自身
# 目录定位，便于在任何位置重跑。原路径记录在下方注释里。
# 原始：D:\Projects\Task Manager Pro\
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("$PSScriptRoot\..\..\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
Write-Output "--- exact last non-white x for selected row y=410 and group row y=150 ---"
foreach($y in 410,150,200){
  $last=-1
  for($x=0;$x -le 1250;$x++){ $c=$img.GetPixel($x,$y); if(-not($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255)){ $last=$x } }
  Write-Output ("y={0} last nonwhite x={1}" -f $y,$last)
}
Write-Output "--- selected row y=410: x=495..515 ---"
for($x=495;$x -le 515;$x++){ Write-Output ("x={0} {1}" -f $x,(Hex $img.GetPixel($x,410))) }
Write-Output "--- group row y=150: x=495..515 ---"
for($x=495;$x -le 515;$x++){ Write-Output ("x={0} {1}" -f $x,(Hex $img.GetPixel($x,150))) }
Write-Output "--- hover? check a non-selected row's name col bg y=200 x=20 ---"
Write-Output ((Hex $img.GetPixel(20,200)))
$img.Dispose()
