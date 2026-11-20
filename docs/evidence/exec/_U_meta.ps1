$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Agent U pass 5: non-color properties (font / image / string / enum) for the parts
# the UIFILE actually references, plus a strict equivalence check between the
# NOMINAL vs UPPERCASE theme class names.

$code = @'
using System; using System.Runtime.InteropServices; using System.Text;
public class UD5 {
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)] public static extern IntPtr OpenThemeData(IntPtr h, string c);
    [DllImport("uxtheme.dll")] public static extern int CloseThemeData(IntPtr h);
    [DllImport("uxtheme.dll")] public static extern int GetThemeColor(IntPtr h, int p, int s, int pr, out uint c);
    [DllImport("uxtheme.dll")] public static extern int GetThemeEnumValue(IntPtr h, int p, int s, int pr, out int v);
    [DllImport("uxtheme.dll")] public static extern int GetThemeInt(IntPtr h, int p, int s, int pr, out int v);
    [DllImport("uxtheme.dll")] public static extern int GetThemeBool(IntPtr h, int p, int s, int pr, out int v);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)] public static extern int GetThemeFont(IntPtr h, IntPtr hdc, int p, int s, int pr, out LOGFONT lf);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)] public static extern int GetThemeString(IntPtr h, int p, int s, int pr, StringBuilder buf, int n);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct LOGFONT {
        public int h,w,e,o,wt; public byte it,un,so,cs,op,cp,q,pf;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string face;
    }
}
'@
Add-Type -TypeDefinition $code

function LF($h,$p,$s) {
    $lf = New-Object UD5+LOGFONT
    if ([UD5]::GetThemeFont($h,[IntPtr]::Zero,$p,$s,210,[ref]$lf) -eq 0) {
        if ($lf.face) { return ("font='{0}' h={1} wt={2}" -f $lf.face,$lf.h,$lf.wt) }
    }
    return ""
}
function Str($h,$p,$s,$pr,$name) {
    $sb = New-Object System.Text.StringBuilder 512
    if ([UD5]::GetThemeString($h,$p,$s,$pr,$sb,512) -eq 0) { if ($sb.Length -gt 0) { return ("{0}='{1}'" -f $name,$sb.ToString()) } }
    return ""
}
function En($h,$p,$s,$pr,$name) {
    $v = 0
    if ([UD5]::GetThemeEnumValue($h,$p,$s,$pr,[ref]$v) -eq 0) { return ("{0}={1}" -f $name,$v) }
    return ""
}

Write-Output "########## FONTS (TMT_FONT=210) for every part the UIFILE uses ##########"
$th = [UD5]::OpenThemeData([IntPtr]::Zero,'TASKMANAGER')
foreach ($p in 13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46) {
    $f = LF $th $p 0
    if ($f) { Write-Output ("  TASKMANAGER p={0} s=0  {1}" -f $p,$f) }
}
foreach ($p in 24) { $f = LF $th $p 1; if ($f) { Write-Output ("  TASKMANAGER p={0} s=1  {1}" -f $p,$f) } }
[void][UD5]::CloseThemeData($th)

$cv = [UD5]::OpenThemeData([IntPtr]::Zero,'CHARTVIEW')
foreach ($p in 1..42) { $f = LF $cv $p 0; if ($f) { Write-Output ("  CHARTVIEW p={0} s=0  {1}" -f $p,$f) } }
[void][UD5]::CloseThemeData($cv)

Write-Output "`n########## TASKMANAGER p43 / p46 image + string + enum ##########"
$th = [UD5]::OpenThemeData([IntPtr]::Zero,'TASKMANAGER')
foreach ($p in 2,13,38,41,42,43,44,45,46) {
    $row = @()
    foreach ($pr in @(@(3001,'IMAGEFILE'),@(3201,'TEXT'),@(4001,'BGTYPE'),@(4003,'FILLTYPE'),@(2403,'BORDERSIZE'))) {
        if ($pr[0] -eq 3001 -or $pr[0] -eq 3201) { $s2 = Str $th $p 0 $pr[0] $pr[1]; if ($s2) { $row += $s2 } }
        else { $s2 = En $th $p 0 $pr[0] $pr[1]; if ($s2) { $row += $s2 } }
    }
    if ($row.Count -gt 0) { Write-Output ("  TASKMANAGER p={0}  {1}" -f $p,($row -join ' ')) }
}
[void][UD5]::CloseThemeData($th)

Write-Output "`n########## Class-name equivalence: NOMINAL vs UPPERCASE ##########"
$pairs = @(
  @('TASKMANAGER','TaskManager'), @('CHARTVIEW','ChartView'),
  @('EXPLORER::LISTVIEW','Explorer::ListView'), @('ITEMSVIEW::LISTVIEW','ItemsView::ListView')
)
foreach ($pr in $pairs) {
    $h1 = [UD5]::OpenThemeData([IntPtr]::Zero,$pr[0])
    $h2 = [UD5]::OpenThemeData([IntPtr]::Zero,$pr[1])
    $c1=[uint32]0; $c2=[uint32]0
    $r1 = if($h1 -ne [IntPtr]::Zero){[UD5]::GetThemeColor($h1,1,1,3802,[ref]$c1)} else {-1}
    $r2 = if($h2 -ne [IntPtr]::Zero){[UD5]::GetThemeColor($h2,1,1,3802,[ref]$c2)} else {-1}
    Write-Output ("  {0}(h={1}) p1s1fill=0x{2:X8}   {3}(h={4}) p1s1fill=0x{5:X8}   same={6}" -f `
        $pr[0],$h1,$c1,$pr[1],$h2,$c2,($c1 -eq $c2))
    if($h1 -ne [IntPtr]::Zero){[void][UD5]::CloseThemeData($h1)}
    if($h2 -ne [IntPtr]::Zero){[void][UD5]::CloseThemeData($h2)}
}

Write-Output "`n########## ChartView p29..p42 all states + enums ##########"
$cv = [UD5]::OpenThemeData([IntPtr]::Zero,'CHARTVIEW')
foreach ($p in 29..42) {
    foreach ($s in 1..3) {
        $c=[uint32]0
        if ([UD5]::GetThemeColor($cv,$p,$s,3802,[ref]$c) -eq 0) {
            $r=[int]($c -band 0xFF);$g=[int](($c -shr 8) -band 0xFF);$b=[int](($c -shr 16) -band 0xFF)
            Write-Output ("  CHARTVIEW p={0} s={1} FILL=#{2:X2}{3:X2}{4:X2}" -f $p,$s,$r,$g,$b)
        }
    }
    $c=[uint32]0
    if ([UD5]::GetThemeColor($cv,$p,0,3803,[ref]$c) -eq 0) {
        $r=[int]($c -band 0xFF);$g=[int](($c -shr 8) -band 0xFF);$b=[int](($c -shr 16) -band 0xFF)
        Write-Output ("  CHARTVIEW p={0} s=0 TEXT=#{1:X2}{2:X2}{3:X2}" -f $p,$r,$g,$b)
    }
}
[void][UD5]::CloseThemeData($cv)
