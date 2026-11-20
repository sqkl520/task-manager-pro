$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$code = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public class ThemeProbe {
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr OpenThemeData(IntPtr hwnd, string classList);
    [DllImport("uxtheme.dll")]
    public static extern int CloseThemeData(IntPtr h);
    [DllImport("uxtheme.dll")]
    public static extern int GetThemeColor(IntPtr h, int partId, int stateId, int propId, out uint color);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern int GetThemeFont(IntPtr h, IntPtr hdc, int partId, int stateId, int propId, out LOGFONT lf);
    [DllImport("uxtheme.dll")]
    public static extern int GetThemeEnumValue(IntPtr h, int partId, int stateId, int propId, out int val);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern int GetThemeInt(IntPtr h, int partId, int stateId, int propId, out int val);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern int GetThemePartSize(IntPtr h, IntPtr hdc, int partId, int stateId, IntPtr prc, int eSize, out SIZE sz);
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern int GetThemeBool(IntPtr h, int partId, int stateId, int propId, out int val);
    [DllImport("uxtheme.dll")]
    public static extern int GetThemeMetric(IntPtr h, IntPtr hdc, int partId, int stateId, int propId, out int val);

    [StructLayout(LayoutKind.Sequential)]
    public struct SIZE { public int cx, cy; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct LOGFONT {
        public int lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
        public byte lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string lfFaceName;
    }
}
'@
Add-Type -TypeDefinition $code

function FmtColor([uint32]$c) {
    # DUI colors are ARGB; high byte is alpha
    $a = ($c -shr 24) -band 0xFF
    $r = ($c -shr 16) -band 0xFF
    $g = ($c -shr 8) -band 0xFF
    $b = $c -band 0xFF
    return ("#{0:X2}{1:X2}{2:X2} (a={3} #{4:X8})" -f $r,$g,$b,$a,$c)
}

$TMT = @{ 3802='FILLCOLOR'; 3803='TEXTCOLOR'; 3804='BORDERCOLOR'; 3805='SHADOWCOLOR'; 3806='GLOWCOLOR'; 3807='EDGELIGHTCOLOR';
          3808='EDGESHADOWCOLOR'; 3809='EDGEFILLCOLOR'; 3810='TRANSPARENTCOLOR'; 3811='GRADIENTCOLOR1'; 3813='GRADIENTCOLOR2';
          3814='GRADIENTCOLOR3'; 3815='GRADIENTCOLOR4'; 3816='GRADIENTCOLOR5'; 3817='MENUHILIGHT'; 3818='MENUBAR';
          3820='ACCENTCOLORHINT'; 3821='TEXTCOLORHINT'; 3822='BORDERCOLORHINT'; 210='FONT'; 2401='BGTYPE';
          2402='BORDERTYPE'; 2404='SIZINGTYPE'; 2409='BORDERSIZE'; 2415='ROUNDCORNERWIDTH' }

foreach ($cls in @('TaskManager','ChartView','ItemsView::ListView','ItemsView')) {
    $h = [ThemeProbe]::OpenThemeData([IntPtr]::Zero, $cls)
    if ($h -eq [IntPtr]::Zero) { Write-Output "### $cls : OpenThemeData FAILED"; continue }
    Write-Output "### $cls : handle=$h"
    for ($part = 0; $part -le 60; $part++) {
        for ($state = 0; $state -le 12; $state++) {
            $rows = @()
            foreach ($prop in @(3802,3803,3804,3805,3811,3813,3820,3821)) {
                $c = [uint32]0
                $hr = [ThemeProbe]::GetThemeColor($h, $part, $state, $prop, [ref]$c)
                if ($hr -eq 0) { $rows += ("    {0} = {1}" -f $TMT[$prop], (FmtColor $c)) }
            }
            $iv = 0
            if ([ThemeProbe]::GetThemeInt($h, $part, $state, 2401, [ref]$iv) -eq 0) { $rows += "    BGTYPE = $iv" }
            if ([ThemeProbe]::GetThemeInt($h, $part, $state, 2409, [ref]$iv) -eq 0) { $rows += "    BORDERSIZE = $iv" }
            if ($rows.Count -gt 0) {
                Write-Output ("  part=$part state=$state")
                $rows | ForEach-Object { Write-Output $_ }
            }
        }
    }
    # fonts
    for ($part = 0; $part -le 60; $part++) {
        $lf = New-Object ThemeProbe+LOGFONT
        if ([ThemeProbe]::GetThemeFont($h, [IntPtr]::Zero, $part, 0, 210, [ref]$lf) -eq 0) {
            if ($lf.lfFaceName) {
                Write-Output ("  FONT part=$part state=0 : face='{0}' height={1} weight={2}" -f $lf.lfFaceName, $lf.lfHeight, $lf.lfWeight)
            }
        }
    }
    [void][ThemeProbe]::CloseThemeData($h)
}
