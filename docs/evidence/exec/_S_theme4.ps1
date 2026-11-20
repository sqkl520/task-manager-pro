$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.Encoding]::UTF8
$code=@'
using System;using System.Runtime.InteropServices;
public class T4 {
 [DllImport("uxtheme.dll",CharSet=CharSet.Unicode)] public static extern IntPtr OpenThemeData(IntPtr h,string c);
 [DllImport("uxtheme.dll")] public static extern int CloseThemeData(IntPtr h);
 [DllImport("uxtheme.dll")] public static extern int GetThemeColor(IntPtr h,int p,int s,int pr,out uint c);
 [DllImport("uxtheme.dll")] public static extern int GetThemeInt(IntPtr h,int p,int s,int pr,out int v);
 [DllImport("uxtheme.dll",CharSet=CharSet.Unicode)] public static extern int GetThemeFont(IntPtr h,IntPtr dc,int p,int s,int pr,out LF lf);
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct LF{public int h,w,e,o,wt;public byte it,ul,so,cs,op,cp,q,pf;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string face;}
}
'@
Add-Type -TypeDefinition $code
function F([uint32]$c){ $r=$c -band 0xFF; $g=($c -shr 8) -band 0xFF; $b=($c -shr 16) -band 0xFF; return ("#{0:X2}{1:X2}{2:X2}" -f $r,$g,$b) }
$jobs=@(
 @('Button',1,4),@('Button',1,2),@('Button',1,3),@('Button',1,1),
 @('TaskManager',43,1),@('TaskManager',43,2),@('TaskManager',2,0),@('TaskManager',13,0),
 @('CONTROLPANELSTYLE',10,1),@('CONTROLPANELSTYLE',10,2),@('CONTROLPANELSTYLE',10,4),
 @('TEXTSTYLE',4,0),@('TEXTSTYLE',1,0),
 @('TaskDialog',13,0),@('ItemsView::ListView',1,3),@('ItemsView::ListView',1,2),@('ItemsView::ListView',1,5),@('ItemsView::ListView',1,6),
 @('Explorer::TreeView',2,1),@('Explorer::TreeView',4,1),@('Explorer::TreeView',2,2),@('Explorer::TreeView',4,2),
 @('CommandModule',3,1),@('CommandModule',3,2)
)
$h=@{}
foreach($j in $jobs){
 $cls=$j[0]; if(-not $h.ContainsKey($cls)){ $h[$cls]=[T4]::OpenThemeData([IntPtr]::Zero,$cls) }
 $hd=$h[$cls]
 $line = ("{0}({1},{2})" -f $cls,$j[1],$j[2])
 if($hd -eq [IntPtr]::Zero){ Write-Output ($line + " <no theme>"); continue }
 foreach($pr in 0x3801,0x3802,0x3803,0x3804,0x3805,0x3809,0x3821){
   $c=[uint32]0
   if([T4]::GetThemeColor($hd,$j[1],$j[2],$pr,[ref]$c) -eq 0){ $line += (" 0x{0:X}={1}" -f $pr,(F $c)) }
 }
 $lf=New-Object T4+LF
 if([T4]::GetThemeFont($hd,[IntPtr]::Zero,$j[1],$j[2],210,[ref]$lf) -eq 0 -and $lf.face){ $line += (" FONT={0} h={1} wt={2}" -f $lf.face,$lf.h,$lf.wt) }
 foreach($ip in 2401,2409,2411,2415){ $v=0; if([T4]::GetThemeInt($hd,$j[1],$j[2],$ip,[ref]$v) -eq 0){ $line += (" int{0}={1}" -f $ip,$v) } }
 Write-Output $line
}
