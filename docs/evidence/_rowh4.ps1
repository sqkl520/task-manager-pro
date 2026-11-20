# 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对脚本自身
# 目录定位，便于在任何位置重跑。原路径记录在下方注释里。
# 原始：D:\Projects\Task Manager Pro\
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile("$PSScriptRoot\..\..\shots\rt3_proc.png")
# CPU column text: around x 700-760 (from screenshot, CPU values right-aligned near 745)
$rows=@()
for($y=160;$y -lt 620;$y++){
  $c=0
  for($x=700;$x -lt 760;$x++){ $px=$b.GetPixel($x,$y); if(($px.R+$px.G+$px.B) -lt 420){$c++} }
  if($c -ge 2){ $rows += $y }
}
$starts=@(); if($rows.Count){ $s=$rows[0]; $p=$rows[0]
for($i=1;$i -lt $rows.Count;$i++){ if($rows[$i]-$p -gt 2){ $starts+=$s; $s=$rows[$i] }; $p=$rows[$i] }
$starts+=$s }
"starts: " + ($starts -join ',')
$d=@(); for($i=1;$i -lt $starts.Count;$i++){ $d += ($starts[$i]-$starts[$i-1]) }
"deltas: " + ($d -join ',')
$b.Dispose()
