# 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对脚本自身
# 目录定位，便于在任何位置重跑。原路径记录在下方注释里。
# 原始：D:\Projects\Task Manager Pro\
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile("$PSScriptRoot\..\..\shots\reference\original_win10.png")
function Hex($c){ return ("#{0:X2}{1:X2}{2:X2}" -f $c.R,$c.G,$c.B) }
foreach($y in 80,85,88,90,91,92,93,95,100,110,120,130,134){
  Write-Output "--- horizontal y=$y non-white runs ---"
  $runs=@()
  $in=$false;$s=0;$last=""
  for($x=0;$x -lt 1251;$x++){
    $c=$img.GetPixel($x,$y); $h=Hex $c
    $nw = ($h -ne "#FFFFFF")
    if($nw -and -not $in){$in=$true;$s=$x;$vals=@()}
    if($nw){ if($vals.Count -lt 4){$vals+=($x.ToString()+":"+$h)} }
    if((-not $nw) -and $in){$in=$false; $runs+= ("["+$s+".."+($x-1)+"] "+($vals -join " "))}
  }
  if($in){$runs+=("["+$s+"..1250] "+($vals -join " "))}
  $runs | Select-Object -First 25 | ForEach-Object { Write-Output $_ }
}
$img.Dispose()
