$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Agent U: authoritative theme color dump (v2 - fixed labels).
# OpenThemeData(NULL, ...) -> no window created, no focus stolen.
# GetThemeColor returns COLORREF = 0x00BBGGRR. Decode R=low, G=mid, B=high.

$code = @'
using System;
using System.Runtime.InteropServices;
public class UTh2 {
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr OpenThemeData(IntPtr hwnd, string classList);
    [DllImport("uxtheme.dll")]
    public static extern int CloseThemeData(IntPtr h);
    [DllImport("uxtheme.dll")]
    public static extern int GetThemeColor(IntPtr h, int partId, int stateId, int propId, out uint color);
    [DllImport("uxtheme.dll")]
    public static extern int GetThemeEnumValue(IntPtr h, int partId, int stateId, int propId, out int val);
    [DllImport("uxtheme.dll")]
    public static extern int IsThemePartDefined(IntPtr h, int partId, int stateId);
}
'@
Add-Type -TypeDefinition $code

function Dec([uint32]$c) {
    $r = [int]($c -band 0xFF)
    $g = [int](($c -shr 8) -band 0xFF)
    $b = [int](($c -shr 16) -band 0xFF)
    return ("#{0:X2}{1:X2}{2:X2}" -f $r,$g,$b)
}

$propIds   = @(3801,3802,3803,3804,3805,3806,3807,3808,3809,3810,3811,3812,3813,3814,3815,3816,3817,3818,3819,3820,3821,3822,3823,3824)
$propNames = @('BORDER','FILL','TEXT','EDGELIGHT','EDGEHIGHLIGHT','EDGESHADOW','EDGEDKSHADOW','EDGEFILL','TRANSPARENT','GRAD1','GRAD2','GRAD3','GRAD4','GRAD5','SHADOW','GLOW','TEXTBORDER','TEXTSHADOW','GLYPHTEXT','GLYPHTRANSPARENT','FILLHINT','BORDERHINT','ACCENTHINT','TEXTHINT')

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("class`tpart`tstate`tprop`tpropname`thex`traw")

foreach ($cls in @('TaskManager','ChartView','ItemsView::ListView','ItemsView')) {
    $h = [UTh2]::OpenThemeData([IntPtr]::Zero, $cls)
    if ($h -eq [IntPtr]::Zero) { Write-Output "### $cls : OpenThemeData FAILED"; continue }
    Write-Output ("### {0} handle={1}" -f $cls,$h)

    $defined = @()
    for ($part = 0; $part -le 80; $part++) {
        $any = $false
        for ($st = 0; $st -le 12; $st++) {
            if ([UTh2]::IsThemePartDefined($h,$part,$st) -ne 0) { $any = $true; break }
        }
        if ($any) { $defined += $part }
    }
    Write-Output ("  parts defined: " + ($defined -join ','))

    foreach ($part in $defined) {
        for ($state = 0; $state -le 12; $state++) {
            if ([UTh2]::IsThemePartDefined($h,$part,$state) -eq 0) { continue }
            $row = @()
            for ($i = 0; $i -lt $propIds.Count; $i++) {
                $c = [uint32]0
                if ([UTh2]::GetThemeColor($h,$part,$state,$propIds[$i],[ref]$c) -eq 0) {
                    $hx = Dec $c
                    $row += ("{0}={1}" -f $propNames[$i], $hx)
                    $lines.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}" -f $cls,$part,$state,$propIds[$i],$propNames[$i],$hx,$c))
                }
            }
            $bg = 0
            if ([UTh2]::GetThemeEnumValue($h,$part,$state,4001,[ref]$bg) -eq 0) { $row += "BGTYPE=$bg" }
            $ft = 0
            if ([UTh2]::GetThemeEnumValue($h,$part,$state,4003,[ref]$ft) -eq 0) { $row += "FILLTYPE=$ft" }
            $bs = 0
            if ([UTh2]::GetThemeEnumValue($h,$part,$state,2403,[ref]$bs) -eq 0) { $row += "BORDERSIZE=$bs" }
            if ($row.Count -gt 0) { Write-Output ("  p={0} s={1}  {2}" -f $part,$state,($row -join ' ')) }
        }
    }
    Write-Output ""
    [void][UTh2]::CloseThemeData($h)
}

[System.IO.File]::WriteAllLines('D:\Projects\Task Manager Pro\_probe\exec\U_colors.tsv', $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("TSV rows: {0}" -f $lines.Count)
