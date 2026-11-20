# 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对脚本自身
# 目录定位，便于在任何位置重跑。原路径记录在下方注释里。
# 原始：D:\Projects\Task Manager Pro\
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("$PSScriptRoot\..\..\shots\showcase\f1_processes.png")
Write-Output "--- our header band y=124..157, name col x=3..300, non-white per row ---"
for($y=124;$y -le 157;$y++){
  $n=0;$mn=9999;$mx=-1
  for($x=3;$x -le 300;$x++){ $c=$img.GetPixel($x,$y); if(-not($c.R -eq 255 -and $c.G -eq 255 -and $c.B -eq 255)){ $n++; if($x -lt $mn){$mn=$x}; if($x -gt $mx){$mx=$x} } }
  Write-Output ("y={0,3} n={1,3} x={2}..{3}" -f $y,$n,$mn,$mx)
}
$img.Dispose()
