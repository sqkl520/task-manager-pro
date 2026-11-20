Add-Type -AssemblyName System.Drawing
$path = 'D:\Projects\Task Manager Pro\shots\reference\original_win10.png'
$b = [System.Drawing.Bitmap]::FromFile($path)
Write-Output ("size={0}x{1}" -f $b.Width, $b.Height)

function ScanRow($y, $x0, $x1) {
    $prev = $null
    $start = $x0
    $out = New-Object System.Collections.ArrayList
    for ($x = $x0; $x -lt $x1; $x++) {
        $c = $b.GetPixel($x, $y)
        $s = ('{0:X2}{1:X2}{2:X2}' -f $c.R, $c.G, $c.B)
        if ($s -ne $prev) {
            if ($null -ne $prev) {
                [void]$out.Add(('{0}-{1}:{2}' -f $start, ($x - 1), $prev))
            }
            $prev = $s
            $start = $x
        }
    }
    [void]$out.Add(('{0}-{1}:{2}' -f $start, ($x1 - 1), $prev))
    return ($out -join ' ')
}

$ys = @(136, 150, 165, 173, 174, 190, 202, 218, 230, 250)
foreach ($y in $ys) {
    Write-Output ("y={0} | {1}" -f $y, (ScanRow $y 0 560))
}
$b.Dispose()
