$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Agent U: render theme parts into an offscreen DIB, prefilled WHITE.
# Aero list-item hot/selected states paint with alpha, so the composite over white
# is exactly what the real list row shows. GDI memory DC only: no window, no focus.

$code = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class UDraw2 {
    [DllImport("uxtheme.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr OpenThemeData(IntPtr hwnd, string classList);
    [DllImport("uxtheme.dll")]
    public static extern int CloseThemeData(IntPtr h);
    [DllImport("uxtheme.dll")]
    public static extern int DrawThemeBackground(IntPtr h, IntPtr hdc, int partId, int stateId, ref RECT r, IntPtr clip);
    [DllImport("uxtheme.dll")]
    public static extern int IsThemePartDefined(IntPtr h, int partId, int stateId);

    [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr h);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr hdc, ref BITMAPINFO bmi, uint u, out IntPtr bits, IntPtr hs, uint o);
    [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr hdc, IntPtr o);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
    [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr d);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFOHEADER {
        public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount;
        public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter;
        public uint biClrUsed, biClrImportant;
    }
    [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFO {
        public BITMAPINFOHEADER bmiHeader;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst=256)] public uint[] bmiColors;
    }

    static byte[] RenderBuf(string cls, int part, int state, int w, int h, byte fr, byte fg, byte fb) {
        IntPtr screen = GetDC(IntPtr.Zero);
        IntPtr dc = CreateCompatibleDC(screen);
        var bmi = new BITMAPINFO();
        bmi.bmiHeader.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
        bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = 0;
        IntPtr bits;
        IntPtr dib = CreateDIBSection(dc, ref bmi, 0, out bits, IntPtr.Zero, 0);
        IntPtr old = SelectObject(dc, dib);

        byte[] pre = new byte[w*h*4];
        for (int i = 0; i < w*h; i++) { pre[i*4]=fb; pre[i*4+1]=fg; pre[i*4+2]=fr; pre[i*4+3]=0xFF; }
        Marshal.Copy(pre, 0, bits, pre.Length);

        IntPtr th = OpenThemeData(IntPtr.Zero, cls);
        if (th != IntPtr.Zero && IsThemePartDefined(th, part, state) != 0) {
            var rc = new RECT { left=0, top=0, right=w, bottom=h };
            DrawThemeBackground(th, dc, part, state, ref rc, IntPtr.Zero);
        }
        if (th != IntPtr.Zero) CloseThemeData(th);

        byte[] buf = new byte[w*h*4];
        Marshal.Copy(bits, buf, 0, buf.Length);
        SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc); ReleaseDC(IntPtr.Zero, screen);
        return buf;
    }

    public static string Render(string cls, int part, int state, int w, int h) {
        byte[] buf = RenderBuf(cls, part, state, w, h, 0xFF, 0xFF, 0xFF);
        var counts = new Dictionary<string,int>();
        for (int i = 0; i < w*h; i++) {
            string key = string.Format("#{0:X2}{1:X2}{2:X2}", buf[i*4+2], buf[i*4+1], buf[i*4+0]);
            if (!counts.ContainsKey(key)) counts[key] = 0;
            counts[key]++;
        }
        var list = new List<KeyValuePair<string,int>>(counts);
        list.Sort(delegate(KeyValuePair<string,int> a, KeyValuePair<string,int> b) { return b.Value.CompareTo(a.Value); });
        var res = new List<string>();
        int n = Math.Min(4, list.Count);
        for (int i = 0; i < n; i++) res.Add(list[i].Key + "x" + list[i].Value);
        return string.Join(" ", res.ToArray());
    }

    public static string Pixels(string cls, int part, int state, int w, int h) {
        byte[] buf = RenderBuf(cls, part, state, w, h, 0xFF, 0xFF, 0xFF);
        var res = new List<string>();
        int x = w/2;
        int[] ys = new int[]{ 0, 2, h/2, h-3, h-1 };
        foreach (int y in ys) {
            int i = (y*w + x)*4;
            res.Add(string.Format("y{0}=#{1:X2}{2:X2}{3:X2}", y, buf[i+2], buf[i+1], buf[i+0]));
        }
        int li = 0;
        res.Add(string.Format("TL=#{0:X2}{1:X2}{2:X2}", buf[li+2], buf[li+1], buf[li+0]));
        int mi = (x)*4;
        res.Add(string.Format("MIDTOP=#{0:X2}{1:X2}{2:X2}", buf[mi+2], buf[mi+1], buf[mi+0]));
        return string.Join(" ", res.ToArray());
    }

    // Horizontal scan of the middle row: identifies left border vs fill vs right border.
    public static string Scan(string cls, int part, int state, int w, int h) {
        byte[] buf = RenderBuf(cls, part, state, w, h, 0xFF, 0xFF, 0xFF);
        var res = new List<string>();
        int y = h/2;
        for (int x = 0; x < w; x += 1) {
            if (x > 6 && x < w-7) continue;
            int i = (y*w + x)*4;
            res.Add(string.Format("x{0}=#{1:X2}{2:X2}{3:X2}", x, buf[i+2], buf[i+1], buf[i+0]));
        }
        return string.Join(" ", res.ToArray());
    }
}
'@
Add-Type -TypeDefinition $code

$targets = @(
    @('TaskManager',43,1,'TmColHeaderItem sort arrow (dtb TASKMANAGER,43,1)'),
    @('TaskManager',43,2,'TmColHeaderItem sort arrow (dtb TASKMANAGER,43,2)'),
    @('TaskManager',2,0,'TmGroupHeaderStyle bg (gtc TASKMANAGER,2,0,3802)'),
    @('TaskManager',38,0,'cpuHeatGrid (gtc 38,0,3802)'),
    @('TaskManager',39,0,'cpuBlockStyleR (gtc 39,0,3802)'),
    @('TaskManager',40,0,'cpuBlockStyleB (gtc 40,0,3802)'),
    @('TaskManager',41,0,'ResizerImage grad1 (gtc 41,0,3802)'),
    @('TaskManager',42,0,'ResizerImage grad2 (gtc 42,0,3802)'),
    @('TaskManager',45,0,'unreferenced fill'),
    @('TaskManager',46,0,'unreferenced fill'),
    @('ItemsView::ListView',1,1,'list row normal'),
    @('ItemsView::ListView',1,2,'list row hot'),
    @('ItemsView::ListView',1,3,'list row selected'),
    @('ItemsView::ListView',1,5,'list row selected-not-focus'),
    @('ItemsView::ListView',1,6,'list row hot-selected'),
    @('ItemsView::ListView',6,0,'group header'),
    @('ItemsView::ListView',6,2,'group header hot'),
    @('ItemsView::ListView',6,3,'group header selected'),
    @('ItemsView::Header',1,0,'column header'),
    @('ItemsView::Header',4,0,'column header sort arrow'),
    @('ItemsView::Header',7,0,'column header overflow'),
    @('Explorer::ListView',1,2,'explorer row hot'),
    @('Explorer::ListView',1,3,'explorer row selected'),
    @('Explorer::ListView',1,5,'explorer row selected-nofocus'),
    @('Tab',1,0,'tab item'),
    @('Tab',9,0,'tab item (variant)'),
    @('Tab',10,0,'tab item (variant)'),
    @('Tab',11,0,'tab body'),
    @('Button',1,1,'toolbar button'),
    @('ScrollBar',3,1,'scrollbar thumb'),
    @('ScrollBar',2,1,'scrollbar track'),
    @('ItemsView',1,1,'ItemsView row normal'),
    @('ItemsView',2,0,'ItemsView part2'),
    @('ItemsView',3,1,'ItemsView part3'),
    @('ItemsView',6,0,'ItemsView part6')
)

$out = New-Object System.Collections.Generic.List[string]
$out.Add("class`tpart`tstate`tlabel`tcolors`tpixels`tscan")

Write-Output "=== RENDERED (white prefill) ==="
foreach ($t in $targets) {
    $cls=$t[0]; $p=[int]$t[1]; $s=[int]$t[2]; $lbl=$t[3]
    $colors = [UDraw2]::Render($cls,$p,$s,200,40)
    $px     = [UDraw2]::Pixels($cls,$p,$s,200,40)
    $scan   = [UDraw2]::Scan($cls,$p,$s,200,40)
    Write-Output ("{0}({1},{2})`t{3}`n   colors: {4}`n   px: {5}`n   edge: {6}" -f $cls,$p,$s,$lbl,$colors,$px,$scan)
    $out.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}" -f $cls,$p,$s,$lbl,$colors,$px,$scan))
}
[System.IO.File]::WriteAllLines('D:\Projects\Task Manager Pro\_probe\exec\U_rendered2.tsv', $out, (New-Object System.Text.UTF8Encoding($false)))
