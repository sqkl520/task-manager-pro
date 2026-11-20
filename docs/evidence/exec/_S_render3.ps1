$ErrorActionPreference='Stop'
$code=@'
using System;using System.Runtime.InteropServices;using System.Collections.Generic;
public class R4 {
 [DllImport("uxtheme.dll",CharSet=CharSet.Unicode)] public static extern IntPtr OpenThemeData(IntPtr h,string c);
 [DllImport("uxtheme.dll")] public static extern int CloseThemeData(IntPtr h);
 [DllImport("uxtheme.dll")] public static extern int DrawThemeBackground(IntPtr h,IntPtr dc,int p,int s,ref RECT r,IntPtr clip);
 [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr h);
 [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr h,ref BITMAPINFO b,uint u,out IntPtr bits,IntPtr sec,uint off);
 [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc,IntPtr o);
 [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
 [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
 [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h,IntPtr dc);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int left,top,right,bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct BIH{public uint sz;public int w,h;public ushort pl,bc;public uint comp,si;public int xp,yp;public uint cu,ci;}
 [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFO{public BIH h; [MarshalAs(UnmanagedType.ByValArray,SizeConst=3)] public uint[] c;}
 public static string Hist(string cls,int part,int state,int w,int h,int bg){
   IntPtr sc=GetDC(IntPtr.Zero), dc=CreateCompatibleDC(sc);
   var bmi=new BITMAPINFO(); bmi.h.sz=(uint)Marshal.SizeOf(typeof(BIH)); bmi.h.w=w; bmi.h.h=-h; bmi.h.pl=1; bmi.h.bc=32; bmi.h.comp=0; bmi.c=new uint[3];
   IntPtr bits; IntPtr dib=CreateDIBSection(dc,ref bmi,0,out bits,IntPtr.Zero,0);
   IntPtr old=SelectObject(dc,dib);
   byte[] pre=new byte[w*h*4]; for(int i=0;i<w*h;i++){pre[i*4]=(byte)bg;pre[i*4+1]=(byte)bg;pre[i*4+2]=(byte)bg;pre[i*4+3]=0xFF;}
   Marshal.Copy(pre,0,bits,pre.Length);
   IntPtr th=OpenThemeData(IntPtr.Zero,cls);
   var r=new RECT{left=0,top=0,right=w,bottom=h};
   if(th!=IntPtr.Zero) DrawThemeBackground(th,dc,part,state,ref r,IntPtr.Zero);
   if(th!=IntPtr.Zero) CloseThemeData(th);
   byte[] post=new byte[w*h*4]; Marshal.Copy(bits,post,0,post.Length);
   SelectObject(dc,old); DeleteObject(dib); DeleteDC(dc); ReleaseDC(IntPtr.Zero,sc);
   var d=new Dictionary<int,int>();
   for(int i=0;i<w*h;i++){int k=(post[i*4+2]<<16)|(post[i*4+1]<<8)|post[i*4]; if(d.ContainsKey(k))d[k]++;else d[k]=1;}
   var list=new List<KeyValuePair<int,int>>(d);
   list.Sort((a,b)=>b.Value.CompareTo(a.Value));
   string s="";
   for(int i=0;i<Math.Min(3,list.Count);i++){int k=list[i].Key;s+=string.Format("#{0:X6}x{1} ",k,list[i].Value);}
   return s;
 }
}
'@
Add-Type -TypeDefinition $code
foreach($cls in @('Explorer::ListView','ItemsView::ListView')){
 foreach($s in 0,1,2,3,4,5,6){
   $onW=[R4]::Hist($cls,1,$s,200,24,255)
   Write-Output ("{0} p=1 s={1} onWhite: {2}" -f $cls,$s,$onW)
 }
}
