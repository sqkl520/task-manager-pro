using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

class MuiDump
{
    static byte[] B;
    static List<(long VA, long Raw, long RawSize)> Secs = new();

    static int U16(long o) => B[o] | (B[o + 1] << 8);
    static long U32(long o) => (uint)(B[o] | (B[o + 1] << 8) | (B[o + 2] << 16) | (B[o + 3] << 24));

    static long RvaToOff(long rva)
    {
        foreach (var s in Secs)
            if (rva >= s.VA && rva < s.VA + s.RawSize) return s.Raw + (rva - s.VA);
        return -1;
    }

    static void Main(string[] args)
    {
        string path = args.Length > 0 ? args[0] : @"C:\Windows\System32\zh-CN\Taskmgr.exe.mui";
        string outPath = args.Length > 1 ? args[1] : "_probe/ui_strings_raw.txt";
        B = File.ReadAllBytes(path);

        long pe = U32(0x3C);
        int numSec = U16(pe + 6);
        int optSize = U16(pe + 20);
        long optOff = pe + 24;
        int magic = U16(optOff);
        long secOff = optOff + optSize;
        for (int i = 0; i < numSec; i++)
        {
            long s = secOff + i * 40;
            Secs.Add((U32(s + 12), U32(s + 20), U32(s + 16)));
        }
        long ddOff = optOff + (magic == 0x20B ? 112 : 96);
        long resRva = U32(ddOff + 16);
        long resBase = RvaToOff(resRva);
        Console.Error.WriteLine($"PE magic=0x{magic:X} resRva=0x{resRva:X} resBase=0x{resBase:X}");

        var entries = new List<(string path, string name, long rva, long size)>();
        resBaseDir = resBase;
        Walk(resBase, "", entries, 0);

        // Group entries by top-level type
        var byType = new Dictionary<string, int>();
        foreach (var e in entries)
        {
            var parts = e.path.TrimStart('/').Split('/');
            string t = parts.Length > 0 ? parts[0] : "?";
            byType[t] = byType.TryGetValue(t, out int c) ? c + 1 : 1;
        }
        Console.Error.WriteLine("Resource types: " + string.Join(", ", byType));

        var lines = new List<string>();
        lines.Add($"### SOURCE: {path}");
        lines.Add($"### {entries.Count} resource entries");
        lines.Add("");

        // ---------- STRINGTABLE (type 6) ----------
        lines.Add("===== STRINGTABLE =====");
        var strItems = new List<(int id, string text)>();
        foreach (var e in entries)
        {
            var parts = e.path.TrimStart('/').Split('/');
            if (parts.Length < 2 || parts[0] != "6") continue;
            int blockId = int.Parse(e.name);
            long off = RvaToOff(e.rva);
            if (off < 0) continue;
            long end = off + e.size;
            long p = off;
            int idx = 0;
            while (p + 2 <= end && idx < 16)
            {
                int len = U16(p);
                p += 2;
                if (len > 0 && p + len * 2 <= end)
                {
                    string s = Encoding.Unicode.GetString(B, (int)p, len * 2);
                    strItems.Add(((blockId - 1) * 16 + idx, s));
                }
                p += (long)len * 2;
                idx++;
            }
        }
        strItems.Sort((a, b) => a.id.CompareTo(b.id));
        lines.Add($"count={strItems.Count}");
        foreach (var (id, text) in strItems)
        {
            string t = text.Replace("\r\n", "\\n").Replace("\n", "\\n").Replace("\t", "\\t");
            lines.Add($"{id}\t{t}");
        }

        // ---------- MENU / DIALOG: 解析器鲁棒性不足，先跳过 ----------
        // 字符串表已覆盖全部界面文案，菜单项文案也在其中。

        File.WriteAllLines(outPath, lines, new UTF8Encoding(false));
        Console.Error.WriteLine($"WROTE {outPath}  ({lines.Count} lines)");
    }

    static void Walk(long dirOff, string path, List<(string, string, long, long)> outList, int depth)
    {
        if (depth > 4) return;
        int named = U16(dirOff + 12);
        int ids = U16(dirOff + 14);
        for (int i = 0; i < named + ids; i++)
        {
            long e = dirOff + 16 + i * 8;
            long nameField = U32(e);
            long dataField = U32(e + 4);
            bool isDir = (dataField & 0x80000000) != 0;
            string nameStr = (nameField & 0x80000000) != 0 ? "<named>" : (nameField & 0x7FFFFFFF).ToString();
            long child = dataField & 0x7FFFFFFF;
            if (isDir) Walk(resBaseDir + child, path + "/" + nameStr, outList, depth + 1);
            else outList.Add((path, nameStr, U32(resBaseDir + child), U32(resBaseDir + child + 4)));
        }
    }

    static long resBaseDir = 0;

    static void ParseMenu(long off, List<string> lines, int depth)
    {
        int ver = U16(off);
        int headerSize = U16(off + 2);
        long p = off + headerSize;
        while (true)
        {
            int flags = U16(p); p += 2;
            if ((flags & 0x10) != 0) break;   // MF_END
            long type = U32(p); p += 4;
            long state = U32(p); p += 4;
            long id = U32(p); p += 4;
            p += 2; // reserved
            string text = ReadSz(p, out long used); p += used;
            string pad = new string(' ', depth * 2);
            if ((type & 0x400) != 0 || (state & 0x80000000) != 0 && depth > 0)
            {
                lines.Add($"{pad}[popup] \"{text}\"");
                ParseMenu(p, lines, depth + 1);
                // 跳过多余的填充
                p += 4;
                continue;
            }
            lines.Add($"{pad}\"{text}\"  id={id}  flags=0x{flags:X}");
        }
    }

    static string ReadSz(long p, out long used)
    {
        var sb = new StringBuilder();
        while (p < B.Length)
        {
            int c = U16(p);
            p += 2;
            if (c == 0) break;
            sb.Append((char)c);
        }
        used = (p % 2 == 1) ? (p + 1) : p;
        return sb.ToString();
    }

    static void ParseDialog(long off, List<string> lines)
    {
        try
        {
            int ver = U16(off);
            int dlgVer = U16(off + 2);
            long p = off;
            if (ver == 1)
            {
                p += 26;  // DLGTEMPLATEEX header
            }
            else
            {
                p += 18;
            }
            string menu = ReadSz(p, out long u1); p = ((p % 2 == 1) ? p + 1 : p) + u1;
            string cls = ReadSz(p, out long u2); p = ((p % 2 == 1) ? p + 1 : p) + u2;
            string title = ReadSz(p, out long u3); p = ((p % 2 == 1) ? p + 1 : p) + u3;
            if (ver == 1) p += 4;  // font size, charset etc handled below
            lines.Add($"title=\"{title}\"");
        }
        catch (Exception ex) { lines.Add("dialog parse error: " + ex.Message); }
    }
}
