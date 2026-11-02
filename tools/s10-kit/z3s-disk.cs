// z3s-disk.cs - helpers for make-windows.ps1 (loaded with Add-Type, C# 5 so
// Windows PowerShell 5.1 can compile it).
//
//   Gpt.SetGuids  : give the VHDX the phone's disk GUID and partition GUIDs, so
//                   the BCD written by bcdedit points at cache / userdata on
//                   the phone (GPT devices are stored as disk GUID + partition
//                   GUID, nothing else).
//   Vol.Bitmap    : FSCTL_GET_VOLUME_BITMAP of the NTFS volume.
//   Export.Plan   : 1 MiB chunks that hold used clusters (+ first and last MiB).
//   Export.Write  : copy those chunks out of the raw disk into files, fix the
//                   boot sector's HiddenSectors for the phone, SHA-256 each file.
//   Vhd.Create4Kn : dynamic VHDX with 4096-byte logical sectors without the
//                   Hyper-V module (virtdisk.dll CreateVirtualDisk, version 2).

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Z3s
{
    public static class Crc32
    {
        static readonly uint[] T = MakeTable();

        static uint[] MakeTable()
        {
            uint[] t = new uint[256];
            for (uint i = 0; i < 256; i++) {
                uint c = i;
                for (int k = 0; k < 8; k++) {
                    c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[i] = c;
            }
            return t;
        }

        public static uint Compute(byte[] b, int off, int len)
        {
            uint c = 0xFFFFFFFFu;
            for (int i = off; i < off + len; i++) {
                c = T[(c ^ b[i]) & 0xFF] ^ (c >> 8);
            }
            return c ^ 0xFFFFFFFFu;
        }
    }

    public interface IBlockDev
    {
        void Read(long offset, byte[] buf, int len);
        void Write(long offset, byte[] buf, int len);
    }

    // ------------------------------------------------------------------ GPT
    public static class Gpt
    {
        static void PutU32(byte[] b, int off, uint v)
        {
            b[off] = (byte)v; b[off + 1] = (byte)(v >> 8); b[off + 2] = (byte)(v >> 16); b[off + 3] = (byte)(v >> 24);
        }

        static byte[] ReadHeader(IBlockDev d, int ss, long lba, string what)
        {
            byte[] h = new byte[ss];
            d.Read(lba * ss, h, ss);
            if (Encoding.ASCII.GetString(h, 0, 8) != "EFI PART") {
                throw new Exception(what + " GPT header not found at LBA " + lba);
            }
            uint size = BitConverter.ToUInt32(h, 12);
            if (size < 92 || size > ss) {
                throw new Exception(what + " GPT header size " + size);
            }
            uint crc = BitConverter.ToUInt32(h, 16);
            PutU32(h, 16, 0);
            if (Crc32.Compute(h, 0, (int)size) != crc) {
                throw new Exception(what + " GPT header CRC is wrong");
            }
            PutU32(h, 16, crc);
            return h;
        }

        static void Seal(byte[] h, Guid disk, uint entriesCrc)
        {
            Array.Copy(disk.ToByteArray(), 0, h, 56, 16);
            PutU32(h, 88, entriesCrc);
            PutU32(h, 16, 0);
            PutU32(h, 16, Crc32.Compute(h, 0, (int)BitConverter.ToUInt32(h, 12)));
        }

        // partOffsets: byte offsets of the partitions to rename (must match an
        // entry's first LBA exactly). Writes primary + backup header and arrays.
        public static string SetGuids(IBlockDev d, int ss, Guid disk, long[] partOffsets, Guid[] partGuids)
        {
            StringBuilder log = new StringBuilder();
            byte[] h = ReadHeader(d, ss, 1, "primary");
            long altLba = BitConverter.ToInt64(h, 32);
            long entLba = BitConverter.ToInt64(h, 72);
            uint n = BitConverter.ToUInt32(h, 80);
            uint esz = BitConverter.ToUInt32(h, 84);
            if (esz < 128 || n == 0 || n > 1024) {
                throw new Exception("odd GPT entry array: " + n + " x " + esz);
            }
            int entBytes = (int)(n * esz);
            int entSpan = (entBytes + ss - 1) / ss * ss;
            byte[] e = new byte[entSpan];
            d.Read(entLba * ss, e, entSpan);
            if (Crc32.Compute(e, 0, entBytes) != BitConverter.ToUInt32(h, 88)) {
                throw new Exception("GPT entry array CRC is wrong");
            }

            for (int p = 0; p < partOffsets.Length; p++) {
                int hit = -1;
                for (int i = 0; i < n; i++) {
                    int o = (int)(i * esz);
                    bool used = false;
                    for (int k = 0; k < 16; k++) {
                        if (e[o + k] != 0) { used = true; break; }
                    }
                    if (used && BitConverter.ToInt64(e, o + 32) * ss == partOffsets[p]) { hit = i; break; }
                }
                if (hit < 0) {
                    throw new Exception("no GPT entry starts at byte " + partOffsets[p]);
                }
                Array.Copy(partGuids[p].ToByteArray(), 0, e, (int)(hit * esz) + 16, 16);
                log.AppendFormat("entry {0}: unique GUID {1}\n", hit + 1, partGuids[p].ToString("B"));
            }

            uint ecrc = Crc32.Compute(e, 0, entBytes);
            Seal(h, disk, ecrc);
            byte[] b = ReadHeader(d, ss, altLba, "backup");
            long bEnt = BitConverter.ToInt64(b, 72);
            Seal(b, disk, ecrc);

            d.Write(entLba * ss, e, entSpan);
            d.Write(1L * ss, h, ss);
            d.Write(bEnt * ss, e, entSpan);
            d.Write(altLba * ss, b, ss);
            log.AppendFormat("disk GUID {0}, primary + backup GPT rewritten\n", disk.ToString("B"));
            return log.ToString();
        }
    }

    // ------------------------------------------- partition type (phone GPT)
    public static class GptType
    {
        public static readonly Guid Esp = new Guid("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");

        static void PutU32(byte[] b, int off, uint v)
        {
            b[off] = (byte)v; b[off + 1] = (byte)(v >> 8); b[off + 2] = (byte)(v >> 16); b[off + 3] = (byte)(v >> 24);
        }

        // region: sectors [regionLba, regionLba + region.Length / ss) of the disk,
        // holding one GPT header (at headerLba) and its whole entry array.
        // Sets the type of the entry named `name`; recomputes the entry-array CRC
        // and the header CRC over the header's own HeaderSize (Samsung: 512).
        // Returns false if the type already matches. Throws on anything odd.
        public static bool Set(byte[] region, int ss, long regionLba, long headerLba, string name, Guid type)
        {
            int ho = (int)((headerLba - regionLba) * ss);
            if (ho < 0 || ho + ss > region.Length || Encoding.ASCII.GetString(region, ho, 8) != "EFI PART") {
                throw new Exception("no GPT header at LBA " + headerLba);
            }
            uint hsize = BitConverter.ToUInt32(region, ho + 12);
            if (hsize < 92 || hsize > ss) {
                throw new Exception("GPT header size " + hsize);
            }
            uint hcrc = BitConverter.ToUInt32(region, ho + 16);
            byte[] h = new byte[hsize];
            Array.Copy(region, ho, h, 0, (int)hsize);
            PutU32(h, 16, 0);
            if (Crc32.Compute(h, 0, (int)hsize) != hcrc) {
                throw new Exception("GPT header CRC wrong at LBA " + headerLba);
            }
            if (BitConverter.ToInt64(region, ho + 24) != headerLba) {
                throw new Exception("GPT header at LBA " + headerLba + " says MyLBA " + BitConverter.ToInt64(region, ho + 24));
            }
            long entLba = BitConverter.ToInt64(region, ho + 72);
            uint n = BitConverter.ToUInt32(region, ho + 80);
            uint esz = BitConverter.ToUInt32(region, ho + 84);
            int eo = (int)((entLba - regionLba) * ss);
            int eb = (int)(n * esz);
            if (esz < 128 || n == 0 || n > 1024 || eo < 0 || eo + eb > region.Length) {
                throw new Exception("GPT entry array not inside the region");
            }
            if (Crc32.Compute(region, eo, eb) != BitConverter.ToUInt32(region, ho + 88)) {
                throw new Exception("GPT entry array CRC wrong (header LBA " + headerLba + ")");
            }
            int hit = -1;
            for (int i = 0; i < n; i++) {
                int o = eo + (int)(i * esz);
                string nm = Encoding.Unicode.GetString(region, o + 56, 72);
                int z = nm.IndexOf('\0');
                if (z >= 0) nm = nm.Substring(0, z);
                if (nm == name) {
                    if (hit >= 0) throw new Exception("two partitions named " + name);
                    hit = i;
                }
            }
            if (hit < 0) {
                throw new Exception("no partition named " + name);
            }
            int to = eo + (int)(hit * esz);
            byte[] want = type.ToByteArray();
            bool same = true;
            for (int k = 0; k < 16; k++) { if (region[to + k] != want[k]) { same = false; break; } }
            if (same) {
                return false;
            }
            Array.Copy(want, 0, region, to, 16);
            PutU32(region, ho + 88, Crc32.Compute(region, eo, eb));
            Array.Copy(region, ho, h, 0, (int)hsize);
            PutU32(h, 16, 0);
            PutU32(region, ho + 16, Crc32.Compute(h, 0, (int)hsize));
            return true;
        }
    }

    // ------------------------------------- rebuild a missing backup GPT
    public static class GptRepair
    {
        static void PutU32(byte[] b, int off, uint v)
        {
            b[off] = (byte)v; b[off + 1] = (byte)(v >> 8); b[off + 2] = (byte)(v >> 16); b[off + 3] = (byte)(v >> 24);
        }
        static void PutU64(byte[] b, int off, long v)
        {
            for (int i = 0; i < 8; i++) b[off + i] = (byte)(v >> (8 * i));
        }

        // primary: sectors LBA 1 .. 1+entSec-1 (header + entry array), checked here.
        // Returns the backup region: entSec sectors of entries, then the header
        // (MyLBA = lastLba, AlternateLBA = 1, EntriesLBA = lastLba - entSec),
        // CRC over the header's own HeaderSize - what gdisk/Windows would write.
        public static byte[] BackupFromPrimary(byte[] primary, int ss, long lastLba)
        {
            if (Encoding.ASCII.GetString(primary, 0, 8) != "EFI PART") throw new Exception("primary GPT header missing");
            uint hsize = BitConverter.ToUInt32(primary, 12);
            if (hsize < 92 || hsize > ss) throw new Exception("primary header size " + hsize);
            byte[] h = new byte[ss];
            Array.Copy(primary, 0, h, 0, ss);
            uint crc = BitConverter.ToUInt32(h, 16);
            PutU32(h, 16, 0);
            if (Crc32.Compute(h, 0, (int)hsize) != crc) throw new Exception("primary GPT header CRC wrong - not rebuilding from it");
            if (BitConverter.ToInt64(h, 24) != 1 || BitConverter.ToInt64(h, 32) != lastLba || BitConverter.ToInt64(h, 72) != 2)
                throw new Exception("primary GPT header does not describe this disk");
            uint n = BitConverter.ToUInt32(h, 80), esz = BitConverter.ToUInt32(h, 84);
            int eb = (int)(n * esz);
            int entSec = (eb + ss - 1) / ss;
            if (esz < 128 || n == 0 || n > 1024 || primary.Length < (1 + entSec) * ss) throw new Exception("primary entry array not in the buffer");
            if (Crc32.Compute(primary, ss, eb) != BitConverter.ToUInt32(h, 88)) throw new Exception("primary entry array CRC wrong - not rebuilding from it");
            byte[] r = new byte[(entSec + 1) * ss];
            Array.Copy(primary, ss, r, 0, entSec * ss);
            PutU64(h, 24, lastLba);
            PutU64(h, 32, 1);
            PutU64(h, 72, lastLba - entSec);
            PutU32(h, 16, Crc32.Compute(h, 0, (int)hsize));
            Array.Copy(h, 0, r, entSec * ss, ss);
            return r;
        }
    }

    // ------------------------------------ read / check / send windows.z3s
    public sealed class ImgSeg
    {
        public int Index;
        public int Target;          // 0 cache, 1 userdata
        public long Offset;         // in the partition
        public long Length;
        public long FileOffset;     // in windows.z3s
        public string Sha256;
    }

    public sealed class ImgInfo
    {
        public long CacheSize;
        public long UserdataSize;
        public long DataOffset;
        public long ExpectedLength;
        public long FileLength;
        public List<ImgSeg> Segs = new List<ImgSeg>();
    }

    public static class Img
    {
        static string Hex(byte[] h, int o) { StringBuilder b = new StringBuilder(); for (int i = 0; i < 32; i++) b.Append(h[o + i].ToString("x2")); return b.ToString(); }

        public static ImgInfo Read(string path)
        {
            ImgInfo r = new ImgInfo();
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                byte[] h = new byte[64];
                if (fs.Read(h, 0, 64) != 64 || Encoding.ASCII.GetString(h, 0, 8) != "Z3SIMG01") throw new Exception("not a windows.z3s image");
                if (BitConverter.ToUInt32(h, 8) != 1) throw new Exception("image version " + BitConverter.ToUInt32(h, 8));
                uint n = BitConverter.ToUInt32(h, 12);
                r.DataOffset = BitConverter.ToInt64(h, 16);
                r.CacheSize = BitConverter.ToInt64(h, 24);
                r.UserdataSize = BitConverter.ToInt64(h, 32);
                if (n == 0 || n > 100000 || r.DataOffset < 64 + 56L * n || r.DataOffset > (64L << 20)) throw new Exception("bad image header");
                byte[] t = new byte[r.DataOffset - 64];
                if (fs.Read(t, 0, t.Length) != t.Length) throw new Exception("image too short for its segment table");
                if (Crc32.Compute(t, 0, t.Length) != BitConverter.ToUInt32(h, 0x28)) throw new Exception("segment table CRC wrong - image damaged");
                long at = r.DataOffset;
                for (int i = 0; i < n; i++) {
                    int e = 56 * i;
                    ImgSeg g = new ImgSeg();
                    g.Index = i;
                    g.Target = (int)BitConverter.ToUInt32(t, e);
                    g.Offset = BitConverter.ToInt64(t, e + 8);
                    g.Length = BitConverter.ToInt64(t, e + 16);
                    g.Sha256 = Hex(t, e + 24);
                    g.FileOffset = at;
                    if (g.Target > 1 || g.Length <= 0 || g.Offset < 0 || (g.Offset % 4096) != 0 ||
                        g.Offset + g.Length > (g.Target == 0 ? r.CacheSize : r.UserdataSize))
                        throw new Exception("segment " + i + " is outside its partition");
                    at += g.Length;
                    r.Segs.Add(g);
                }
                r.ExpectedLength = at;
                r.FileLength = fs.Length;
            }
            return r;
        }

        // Re-hash every segment from the file. Returns the index of the first bad one, or -1.
        public static int Verify(string path, ImgInfo info, Action<string> progress)
        {
            byte[] buf = new byte[4 << 20];
            long total = 0, done = 0, nextSay = 0;
            foreach (ImgSeg g in info.Segs) total += g.Length;
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
                foreach (ImgSeg g in info.Segs) {
                    fs.Seek(g.FileOffset, SeekOrigin.Begin);
                    using (SHA256 sha = SHA256.Create()) {
                        for (long left = g.Length; left > 0; ) {
                            int c = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                            if (c <= 0) return g.Index;
                            sha.TransformBlock(buf, 0, c, null, 0);
                            left -= c; done += c;
                            if (progress != null && done >= nextSay) { progress(string.Format("checking windows.z3s  {0:N0} / {1:N0} MiB", done >> 20, total >> 20)); nextSay = done + (2048L << 20); }
                        }
                        sha.TransformFinalBlock(new byte[0], 0, 0);
                        if (Hex(sha.Hash, 0) != g.Sha256) return g.Index;
                    }
                }
            }
            return -1;
        }
    }

    public sealed class Piece
    {
        public int N;
        public string Partition;    // "cache" / "userdata"
        public long StartMiB;       // in the partition
        public long CountMiB;
        public string File;         // relative: pieces\pNNN-....bin
        public string Sha256;
    }

    public static class Split
    {
        // Cuts windows.z3s into files of <= pieceMiB (MiB aligned, so dd seek=MiB works),
        // checks every segment against its table SHA-256 on the way.
        public static List<Piece> Run(string image, ImgInfo info, int pieceMiB, string outDir, Action<string> progress)
        {
            const long MiB = 1 << 20;
            List<Piece> list = new List<Piece>();
            byte[] buf = new byte[4 << 20];
            long total = 0, done = 0, nextSay = 0;
            foreach (ImgSeg g in info.Segs) total += g.Length;
            Directory.CreateDirectory(Path.Combine(outDir, "pieces"));
            using (FileStream fs = new FileStream(image, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
                foreach (ImgSeg g in info.Segs) {
                    if ((g.Offset % MiB) != 0 || (g.Length % MiB) != 0) throw new Exception("segment " + g.Index + " is not MiB aligned");
                    fs.Seek(g.FileOffset, SeekOrigin.Begin);
                    using (SHA256 segSha = SHA256.Create()) {
                        for (long o = 0; o < g.Length; o += pieceMiB * MiB) {
                            long len = Math.Min(pieceMiB * MiB, g.Length - o);
                            Piece p = new Piece();
                            p.N = list.Count + 1;
                            p.Partition = g.Target == 0 ? "cache" : "userdata";
                            p.StartMiB = (g.Offset + o) / MiB;
                            p.CountMiB = len / MiB;
                            p.File = "pieces\\" + string.Format("p{0:D3}-{1}-{2}.bin", p.N, p.Partition, p.StartMiB);
                            using (SHA256 sha = SHA256.Create())
                            using (FileStream os = new FileStream(Path.Combine(outDir, "pieces", Path.GetFileName(p.File.Replace("\\", "/"))), FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) {
                                for (long left = len; left > 0; ) {
                                    int c = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                                    if (c <= 0) throw new Exception("windows.z3s ends early in segment " + g.Index);
                                    sha.TransformBlock(buf, 0, c, null, 0);
                                    segSha.TransformBlock(buf, 0, c, null, 0);
                                    os.Write(buf, 0, c);
                                    left -= c; done += c;
                                    if (progress != null && done >= nextSay) { progress(string.Format("{0:N0} / {1:N0} MiB", done >> 20, total >> 20)); nextSay = done + (1024L << 20); }
                                }
                                sha.TransformFinalBlock(new byte[0], 0, 0);
                                StringBuilder b = new StringBuilder(); foreach (byte x in sha.Hash) b.Append(x.ToString("x2"));
                                p.Sha256 = b.ToString();
                            }
                            list.Add(p);
                        }
                        segSha.TransformFinalBlock(new byte[0], 0, 0);
                        StringBuilder sb = new StringBuilder(); foreach (byte x in segSha.Hash) sb.Append(x.ToString("x2"));
                        if (sb.ToString() != g.Sha256) throw new Exception("windows.z3s is damaged in segment " + g.Index + " (SHA-256 differs from its table)");
                    }
                }
            }
            return list;
        }
    }

    // Runs a program and feeds it a byte range of a file on stdin (binary-safe,
    // unlike a PowerShell pipeline). Returns the exit code; Sha = SHA-256 of
    // what was sent; Sent = bytes the program accepted.
    public static class Feed
    {
        public static int Run(string exe, string args, string file, long offset, long length, out string sha, out long sent)
        {
            System.Diagnostics.ProcessStartInfo psi = new System.Diagnostics.ProcessStartInfo(exe, args);
            psi.UseShellExecute = false;
            psi.RedirectStandardInput = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.CreateNoWindow = true;
            sent = 0;
            using (System.Diagnostics.Process p = System.Diagnostics.Process.Start(psi))
            using (SHA256 h = SHA256.Create()) {
                p.OutputDataReceived += delegate { };
                p.ErrorDataReceived += delegate { };
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                byte[] buf = new byte[1 << 20];
                using (FileStream fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
                    fs.Seek(offset, SeekOrigin.Begin);
                    Stream st = p.StandardInput.BaseStream;
                    try {
                        for (long left = length; left > 0; ) {
                            int c = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
                            if (c <= 0) break;
                            h.TransformBlock(buf, 0, c, null, 0);
                            st.Write(buf, 0, c);
                            left -= c; sent += c;
                        }
                        st.Flush();
                    } catch (IOException) {
                        // the program (adb) went away - the exit code and the phone's log say why
                    }
                    try { st.Close(); } catch (IOException) { }
                }
                h.TransformFinalBlock(new byte[0], 0, 0);
                StringBuilder b = new StringBuilder();
                foreach (byte x in h.Hash) b.Append(x.ToString("x2"));
                sha = b.ToString();
                p.WaitForExit();
                return p.ExitCode;
            }
        }
    }

    // ------------------------------------------------------- raw disk (Win32)
    public sealed class RawDisk : IBlockDev, IDisposable
    {
        const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
        const uint FILE_SHARE_RW = 3, OPEN_EXISTING = 3;
        const uint NO_BUFFERING = 0x20000000, WRITE_THROUGH = 0x80000000;
        const int CAP = 4 << 20;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetFilePointerEx(SafeFileHandle h, long dist, out long pos, uint method);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ReadFile(SafeFileHandle h, IntPtr buf, int n, out int got, IntPtr ov);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool WriteFile(SafeFileHandle h, IntPtr buf, int n, out int put, IntPtr ov);

        readonly SafeFileHandle h;
        readonly IntPtr raw;
        readonly IntPtr buf;
        readonly string name;

        public RawDisk(string path, bool write)
        {
            name = path;
            h = CreateFile(path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_RW, IntPtr.Zero,
                           OPEN_EXISTING, NO_BUFFERING | (write ? WRITE_THROUGH : 0), IntPtr.Zero);
            if (h.IsInvalid) {
                throw new Exception("open " + path + ": Win32 error " + Marshal.GetLastWin32Error());
            }
            raw = Marshal.AllocHGlobal(CAP + 4096);
            buf = new IntPtr((raw.ToInt64() + 4095) & ~4095L);
        }

        void Seek(long off)
        {
            long pos;
            if (!SetFilePointerEx(h, off, out pos, 0)) {
                throw new Exception("seek " + name + " @" + off + ": Win32 error " + Marshal.GetLastWin32Error());
            }
        }

        public void Read(long off, byte[] dst, int len)
        {
            for (int done = 0; done < len; ) {
                int n = Math.Min(CAP, len - done);
                int got;
                Seek(off + done);
                if (!ReadFile(h, buf, n, out got, IntPtr.Zero) || got != n) {
                    throw new Exception("read " + name + " @" + (off + done) + ": Win32 error " + Marshal.GetLastWin32Error());
                }
                Marshal.Copy(buf, dst, done, n);
                done += n;
            }
        }

        public void Write(long off, byte[] src, int len)
        {
            for (int done = 0; done < len; ) {
                int n = Math.Min(CAP, len - done);
                int put;
                Marshal.Copy(src, done, buf, n);
                Seek(off + done);
                if (!WriteFile(h, buf, n, out put, IntPtr.Zero) || put != n) {
                    throw new Exception("write " + name + " @" + (off + done) + ": Win32 error " + Marshal.GetLastWin32Error());
                }
                done += n;
            }
        }

        public void Dispose()
        {
            h.Dispose();
            Marshal.FreeHGlobal(raw);
        }
    }

    // ------------------------------------------------------- volume bitmap
    public static class Vol
    {
        const uint FSCTL_GET_VOLUME_BITMAP = 0x0009006F;
        const int ERROR_MORE_DATA = 234;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inb, int inl, byte[] outb, int outl, out int ret, IntPtr ov);

        // volume: \\?\Volume{...} (no trailing backslash) or \\.\X:
        // Returns the bitmap; clusters = number of clusters it covers.
        public static byte[] Bitmap(string volume, long clustersHint, out long clusters)
        {
            using (SafeFileHandle h = CreateFile(volume, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
                if (h.IsInvalid) {
                    throw new Exception("open " + volume + ": Win32 error " + Marshal.GetLastWin32Error());
                }
                byte[] input = new byte[8];
                int size = (int)(16 + clustersHint / 8 + 65536);
                for (int attempt = 0; attempt < 4; attempt++) {
                    byte[] output = new byte[size];
                    int ret;
                    bool ok = DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, input, 8, output, size, out ret, IntPtr.Zero);
                    int err = Marshal.GetLastWin32Error();
                    if (!ok && err != ERROR_MORE_DATA) {
                        throw new Exception("FSCTL_GET_VOLUME_BITMAP: Win32 error " + err);
                    }
                    clusters = BitConverter.ToInt64(output, 8);
                    if (ok && BitConverter.ToInt64(output, 0) == 0) {
                        byte[] bmp = new byte[(clusters + 7) / 8];
                        Array.Copy(output, 16, bmp, 0, bmp.Length);
                        return bmp;
                    }
                    size = (int)(16 + clusters / 8 + 65536);
                }
                throw new Exception("FSCTL_GET_VOLUME_BITMAP did not fit");
            }
        }
    }

    // -------------------------------------------------------------- export
    public sealed class Extent
    {
        public long StartMiB;
        public long CountMiB;
        public string File;
        public string Sha256;
    }

    public static class Export
    {
        const int MiB = 1 << 20;

        public static List<Extent> Plan(byte[] bmp, long clusters, int clusterSize, long partSize, int gapMiB)
        {
            if (partSize % MiB != 0 || MiB % clusterSize != 0) {
                throw new Exception("partition size / cluster size not MiB aligned");
            }
            long mib = partSize / MiB;
            long cpm = MiB / clusterSize;
            bool[] used = new bool[mib];
            for (long i = 0; i < bmp.Length; i++) {
                if (bmp[i] == 0) {
                    continue;
                }
                for (int bit = 0; bit < 8; bit++) {
                    long c = i * 8 + bit;
                    if (c < clusters && (bmp[i] & (1 << bit)) != 0) {
                        used[c / cpm] = true;
                    }
                }
            }
            used[0] = true;          // boot sector
            used[mib - 1] = true;    // NTFS backup boot sector (last sector)

            List<Extent> list = new List<Extent>();
            for (long m = 0; m < mib; ) {
                if (!used[m]) { m++; continue; }
                long s = m;
                while (m < mib && used[m]) { m++; }
                Extent last = list.Count > 0 ? list[list.Count - 1] : null;
                if (last != null && s - (last.StartMiB + last.CountMiB) <= gapMiB) {
                    last.CountMiB = m - last.StartMiB;
                } else {
                    Extent x = new Extent();
                    x.StartMiB = s;
                    x.CountMiB = m - s;
                    list.Add(x);
                }
            }
            return list;
        }

        // Copies each extent (offsets relative to the partition at partOffset)
        // into outDir\prefixNNNN.bin. For every offset in bootSectors that falls
        // inside, the 32-bit HiddenSectors field (BPB +0x1C) becomes hiddenLba,
        // if the sector ends in 55 AA.
        public static void Write(IBlockDev disk, long partOffset, int ss, List<Extent> ext, string outDir,
                                 string prefix, long[] bootSectors, uint hiddenLba, Action<string> progress)
        {
            byte[] buf = new byte[4 * MiB];
            for (int i = 0; i < ext.Count; i++) {
                Extent x = ext[i];
                x.File = prefix + i.ToString("D4") + ".bin";
                string path = Path.Combine(outDir, x.File);
                using (SHA256 sha = SHA256.Create())
                using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) {
                    long start = x.StartMiB * MiB;
                    long end = (x.StartMiB + x.CountMiB) * MiB;
                    for (long off = start; off < end; ) {
                        int n = (int)Math.Min(buf.Length, end - off);
                        disk.Read(partOffset + off, buf, n);
                        foreach (long bs in bootSectors) {
                            if (bs >= off && bs + ss <= off + n) {
                                int o = (int)(bs - off);
                                if (buf[o + 510] == 0x55 && buf[o + 511] == 0xAA) {
                                    buf[o + 0x1C] = (byte)hiddenLba;
                                    buf[o + 0x1D] = (byte)(hiddenLba >> 8);
                                    buf[o + 0x1E] = (byte)(hiddenLba >> 16);
                                    buf[o + 0x1F] = (byte)(hiddenLba >> 24);
                                    if (progress != null) progress("boot sector @" + bs + ": HiddenSectors = " + hiddenLba);
                                } else if (progress != null) {
                                    progress("WARN no boot sector signature @" + bs);
                                }
                            }
                        }
                        sha.TransformBlock(buf, 0, n, null, 0);
                        fs.Write(buf, 0, n);
                        off += n;
                    }
                    sha.TransformFinalBlock(new byte[0], 0, 0);
                    x.Sha256 = BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
                }
                if (progress != null) {
                    progress(string.Format("{0}/{1}  {2}  MiB {3} +{4}", i + 1, ext.Count, x.File, x.StartMiB, x.CountMiB));
                }
            }
        }
    }

    // ------------------------------------------- single-file image (v1.7)
    public sealed class Seg
    {
        public int Target;          // 0 = cache, 1 = userdata
        public long PartOffset;     // where the partition starts on the source disk
        public long StartMiB;       // relative to the partition
        public long CountMiB;
        public long[] BootSectors;  // partition-relative offsets of boot sectors to fix
        public uint HiddenLba;
        public string Sha256;
    }

    // v2.1: make-windows writes the flash kit's files straight from the VHDX - no
    // windows.z3s in between. Pieces are MiB aligned (dd seek= in MiB) and <= pieceMiB.
    public static class Pieces
    {
        const long MiB = 1 << 20;

        static void Put32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }

        public static List<Piece> Write(IBlockDev disk, int ss, List<Seg> segs, int pieceMiB, string outDir, Action<string> progress)
        {
            List<Piece> list = new List<Piece>();
            byte[] buf = new byte[4 << 20];
            long total = 0, done = 0, nextSay = 0;
            foreach (Seg g in segs) total += g.CountMiB * MiB;
            string pdir = Path.Combine(outDir, "pieces");
            Directory.CreateDirectory(pdir);
            foreach (Seg g in segs) {
                for (long m = 0; m < g.CountMiB; m += pieceMiB) {
                    long cnt = Math.Min(pieceMiB, g.CountMiB - m);
                    Piece p = new Piece();
                    p.N = list.Count + 1;
                    p.Partition = g.Target == 0 ? "cache" : "userdata";
                    p.StartMiB = g.StartMiB + m;
                    p.CountMiB = cnt;
                    string name = string.Format("p{0:D3}-{1}-{2}.bin", p.N, p.Partition, p.StartMiB);
                    p.File = "pieces\\" + name;
                    using (SHA256 sha = SHA256.Create())
                    using (FileStream os = new FileStream(Path.Combine(pdir, name), FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) {
                        long start = p.StartMiB * MiB, end = (p.StartMiB + cnt) * MiB;
                        for (long off = start; off < end; ) {
                            int c = (int)Math.Min(buf.Length, end - off);
                            disk.Read(g.PartOffset + off, buf, c);
                            if (g.BootSectors != null) {
                                foreach (long bs in g.BootSectors) {
                                    if (bs >= off && bs + ss <= off + c) {
                                        int o = (int)(bs - off);
                                        if (buf[o + 510] == 0x55 && buf[o + 511] == 0xAA) {
                                            Put32(buf, o + 0x1C, g.HiddenLba);
                                            if (progress != null) progress("boot sector @" + bs + ": HiddenSectors = " + g.HiddenLba);
                                        }
                                    }
                                }
                            }
                            sha.TransformBlock(buf, 0, c, null, 0);
                            os.Write(buf, 0, c);
                            off += c; done += c;
                            if (progress != null && done >= nextSay) {
                                progress(string.Format("{0:N0} / {1:N0} MiB", done / MiB, total / MiB));
                                nextSay = done + 1024L * MiB;
                            }
                        }
                        sha.TransformFinalBlock(new byte[0], 0, 0);
                        p.Sha256 = BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
                    }
                    list.Add(p);
                }
            }
            return list;
        }
    }

    // flash-windows.cmd for a pieces\ kit: plain adb exec-in + dd per file (the way that
    // wrote 12 GB fine), sha256sum read-back, a bad piece written again once, cache ->
    // ESP type (set-esp-type.ps1), UEFI to boot + read-back, reboot. Also pieces.txt.
    public static class FlashCmd
    {
        public static void Write(string dir, List<Piece> pieces, long cacheSize, long userdataSize,
                                 string uefiFile, string uefiName, string what, string tool)
        {
            long mib = 0;
            foreach (Piece p in pieces) mib += p.CountMiB;
            int n = pieces.Count;
            List<string> L = new List<string>();
            L.Add("@echo off");
            L.Add("setlocal");
            L.Add("rem flash-windows.cmd (" + tool + "): " + what);
            L.Add("rem Phone in TWRP, USB cable in. ERASES userdata (Android data / WinPE / old Windows) and cache.");
            L.Add("cd /d \"%~dp0\"");
            L.Add("adb get-state >nul 2>&1 || (echo [FAIL] adb sees no device - boot TWRP and plug the cable & goto :end)");
            L.Add("set \"UD=\" & set \"CA=\"");
            L.Add("for /f %%S in ('adb shell \"blockdev --getsize64 /dev/block/by-name/userdata\"') do set \"UD=%%S\"");
            L.Add("for /f %%S in ('adb shell \"blockdev --getsize64 /dev/block/by-name/cache\"') do set \"CA=%%S\"");
            L.Add("if not \"%UD%\"==\"" + userdataSize + "\" (echo [FAIL] userdata is %UD% bytes, expected " + userdataSize + " - different layout, nothing written & goto :end)");
            L.Add("if not \"%CA%\"==\"" + cacheSize + "\" (echo [FAIL] cache is %CA% bytes, expected " + cacheSize + " - different layout, nothing written & goto :end)");
            L.Add("echo About to write Windows: " + mib + " MiB in " + n + " files. Close this window to cancel.");
            L.Add("pause");
            L.Add("echo [1/5] cache: EFI system partition type");
            L.Add("powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0set-esp-type.ps1\" -FromFlash || (echo [FAIL] could not set the cache partition type - nothing else was written & goto :end)");
            L.Add("adb shell \"umount /data; umount /cache; umount /dev/block/by-name/userdata; umount /dev/block/by-name/cache\" >nul 2>&1");
            L.Add("echo [2/5] writing " + n + " files");
            foreach (Piece p in pieces) L.Add("call :w " + p.N + " " + p.Partition + " " + p.StartMiB + " " + p.File);
            L.Add("adb shell \"sync; echo 3 > /proc/sys/vm/drop_caches\" >nul 2>&1");
            L.Add("echo [3/5] reading everything back");
            L.Add("set BAD=0");
            foreach (Piece p in pieces) L.Add("call :v " + p.Partition + " " + p.StartMiB + " " + p.CountMiB + " " + p.File + " " + p.Sha256);
            L.Add("if not \"%BAD%\"==\"0\" (echo [FAIL] %BAD% pieces still differ after a second write - run this file again & goto :end)");
            L.Add("echo [ OK ] Windows written and verified");
            if (uefiFile != null && File.Exists(uefiFile)) {
                long ulen = new FileInfo(uefiFile).Length;
                string uhash;
                using (SHA256 sha = SHA256.Create())
                using (FileStream fs = File.OpenRead(uefiFile)) uhash = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "").ToLowerInvariant();
                L.Add("echo [4/5] UEFI " + uefiName + " to boot");
                L.Add("adb exec-in \"dd of=/dev/block/by-name/boot bs=1048576 conv=fsync 2>/dev/null\" < uefi.img");
                L.Add("adb shell \"sync; echo 3 > /proc/sys/vm/drop_caches\" >nul 2>&1");
                L.Add("set \"GOT=\"");
                L.Add("for /f \"tokens=1\" %%H in ('adb shell \"head -c " + ulen + " /dev/block/by-name/boot | sha256sum\"') do set \"GOT=%%H\"");
                L.Add("if /i not \"%GOT%\"==\"" + uhash + "\" (echo [FAIL] UEFI reads back wrong - Windows is fine, run this file again or flash the UEFI with Odin & goto :end)");
                L.Add("echo [ OK ] UEFI " + uefiName + " written and verified");
            } else {
                L.Add("echo [4/5] no uefi.img here - the UEFI already on the phone stays (needs a beyond1lte UEFI)");
            }
            L.Add("echo [5/5] rebooting into Windows in 5 s - close this window to stay in TWRP");
            L.Add("timeout /t 5 >nul");
            L.Add("adb reboot");
            L.Add("echo [ OK ] done. First boot sets Windows up by itself, 10-20 minutes.");
            L.Add(":end");
            L.Add("pause");
            L.Add("exit /b 0");
            L.Add("");
            L.Add(":w");
            L.Add("echo   %1/" + n + "  %2 @ %3 MiB");
            L.Add("adb exec-in \"dd of=/dev/block/by-name/%2 bs=1048576 seek=%3 conv=fsync 2>/dev/null\" < %4");
            L.Add("exit /b 0");
            L.Add("");
            L.Add(":v");
            L.Add("set \"GOT=\"");
            L.Add("for /f \"tokens=1\" %%H in ('adb shell \"dd if=/dev/block/by-name/%1 bs=1048576 skip=%2 count=%3 2>/dev/null | sha256sum\"') do set \"GOT=%%H\"");
            L.Add("if /i \"%GOT%\"==\"%5\" (echo   ok  %4 & exit /b 0)");
            L.Add("echo   BAD %4 - writing it again");
            L.Add("adb exec-in \"dd of=/dev/block/by-name/%1 bs=1048576 seek=%2 conv=fsync 2>/dev/null\" < %4");
            L.Add("adb shell \"sync; echo 3 > /proc/sys/vm/drop_caches\" >nul 2>&1");
            L.Add("set \"GOT=\"");
            L.Add("for /f \"tokens=1\" %%H in ('adb shell \"dd if=/dev/block/by-name/%1 bs=1048576 skip=%2 count=%3 2>/dev/null | sha256sum\"') do set \"GOT=%%H\"");
            L.Add("if /i \"%GOT%\"==\"%5\" (echo   ok  %4 - fixed & exit /b 0)");
            L.Add("echo   BAD %4 & set /a BAD+=1");
            L.Add("exit /b 0");
            File.WriteAllText(Path.Combine(dir, "flash-windows.cmd"), string.Join("\r\n", L.ToArray()) + "\r\n", new ASCIIEncoding());

            List<string> M = new List<string>();
            M.Add(tool + "  " + what);
            foreach (Piece p in pieces) M.Add(string.Format("{0,3} {1,-8} {2,7} {3,4} {4} {5}", p.N, p.Partition, p.StartMiB, p.CountMiB, p.File, p.Sha256));
            File.WriteAllText(Path.Combine(dir, "pieces.txt"), string.Join("\r\n", M.ToArray()) + "\r\n", new ASCIIEncoding());
        }
    }

    // windows.z3s, read by tools/z3sflash.c on the phone:
    //   "Z3SIMG01", u32 1, u32 n, u64 dataOffset, u64 cacheSize, u64 userdataSize,
    //   u32 crc32(table region), pad to 0x40; n x { u32 target, u32 0, u64 offset,
    //   u64 length, sha256[32] }; zero pad to 4 KiB; segment data back to back.
    public static class Pack
    {
        const int MiB = 1 << 20;

        static void Put32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
        static void Put64(byte[] b, int o, long v) { Put32(b, o, (uint)v); Put32(b, o + 4, (uint)(v >> 32)); }

        public static long Write(IBlockDev disk, int ss, List<Seg> segs, string outPath,
                                 long cacheSize, long userdataSize, Action<string> progress)
        {
            int n = segs.Count;
            long dataOff = (64L + 56L * n + 4095) / 4096 * 4096;
            byte[] head = new byte[dataOff];
            byte[] buf = new byte[4 * MiB];
            long total = 0, done = 0, nextSay = 0;
            foreach (Seg g in segs) total += g.CountMiB * MiB;

            using (FileStream fs = new FileStream(outPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20)) {
                fs.Write(head, 0, head.Length);
                for (int i = 0; i < n; i++) {
                    Seg g = segs[i];
                    using (SHA256 sha = SHA256.Create()) {
                        long start = g.StartMiB * MiB, end = (g.StartMiB + g.CountMiB) * MiB;
                        for (long off = start; off < end; ) {
                            int c = (int)Math.Min(buf.Length, end - off);
                            disk.Read(g.PartOffset + off, buf, c);
                            if (g.BootSectors != null) {
                                foreach (long bs in g.BootSectors) {
                                    if (bs >= off && bs + ss <= off + c) {
                                        int o = (int)(bs - off);
                                        if (buf[o + 510] == 0x55 && buf[o + 511] == 0xAA) {
                                            Put32(buf, o + 0x1C, g.HiddenLba);
                                            if (progress != null) progress("boot sector @" + bs + ": HiddenSectors = " + g.HiddenLba);
                                        }
                                    }
                                }
                            }
                            sha.TransformBlock(buf, 0, c, null, 0);
                            fs.Write(buf, 0, c);
                            off += c; done += c;
                            if (progress != null && done >= nextSay) {
                                progress(string.Format("{0:N0} / {1:N0} MiB", done / MiB, total / MiB));
                                nextSay = done + 1024L * MiB;
                            }
                        }
                        sha.TransformFinalBlock(new byte[0], 0, 0);
                        g.Sha256 = BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
                        int e = 64 + 56 * i;
                        Put32(head, e, (uint)g.Target);
                        Put64(head, e + 8, start);
                        Put64(head, e + 16, end - start);
                        Array.Copy(sha.Hash, 0, head, e + 24, 32);
                    }
                }
                Encoding.ASCII.GetBytes("Z3SIMG01").CopyTo(head, 0);
                Put32(head, 8, 1);
                Put32(head, 12, (uint)n);
                Put64(head, 16, dataOff);
                Put64(head, 24, cacheSize);
                Put64(head, 32, userdataSize);
                Put32(head, 40, Crc32.Compute(head, 64, (int)(dataOff - 64)));
                fs.Position = 0;
                fs.Write(head, 0, head.Length);
                return fs.Length;
            }
        }
    }

    // ------------------------------------------------- 4Kn VHDX (no Hyper-V)
    public static class Vhd
    {
        [StructLayout(LayoutKind.Sequential)]
        struct VIRTUAL_STORAGE_TYPE
        {
            public uint DeviceId;
            public Guid VendorId;
        }

        // CREATE_VIRTUAL_DISK_PARAMETERS, Version2. The int after Version pads
        // the union to offset 8 (it holds 8-byte members).
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct CREATE_PARAMS_V2
        {
            public int Version;
            public int Pad;
            public Guid UniqueId;
            public ulong MaximumSize;
            public uint BlockSizeInBytes;
            public uint SectorSizeInBytes;
            public uint PhysicalSectorSizeInBytes;
            [MarshalAs(UnmanagedType.LPWStr)] public string ParentPath;
            [MarshalAs(UnmanagedType.LPWStr)] public string SourcePath;
            public int OpenFlags;
            public VIRTUAL_STORAGE_TYPE ParentVirtualStorageType;
            public VIRTUAL_STORAGE_TYPE SourceVirtualStorageType;
            public Guid ResiliencyGuid;
        }

        [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
        static extern int CreateVirtualDisk(ref VIRTUAL_STORAGE_TYPE type, string path, int accessMask, IntPtr sd,
                                            int flags, uint providerFlags, ref CREATE_PARAMS_V2 p, IntPtr ov, out IntPtr handle);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr h);

        public static void Create4Kn(string path, ulong size)
        {
            VIRTUAL_STORAGE_TYPE t = new VIRTUAL_STORAGE_TYPE();
            t.DeviceId = 3;   // VHDX
            t.VendorId = new Guid("EC984AEC-A0F9-47e9-901F-71415A66345B");
            CREATE_PARAMS_V2 p = new CREATE_PARAMS_V2();
            p.Version = 2;
            p.UniqueId = Guid.NewGuid();
            p.MaximumSize = size;
            p.SectorSizeInBytes = 4096;
            p.PhysicalSectorSizeInBytes = 4096;
            IntPtr h;
            int rc = CreateVirtualDisk(ref t, path, 0, IntPtr.Zero, 0, 0, ref p, IntPtr.Zero, out h);
            if (rc != 0) {
                throw new Exception("CreateVirtualDisk: Win32 error " + rc);
            }
            CloseHandle(h);
        }
    }
}
