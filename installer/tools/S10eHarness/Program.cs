// SPDX-License-Identifier: BSD-2-Clause-Patent
// Drives S9Woa.Installer.Core's own image and transfer pipeline for the S10e (beyond0lte),
// whose stages the WinUI app still gates. No step here is hand-rolled: each verb calls the
// same Core services the app's InstallPage calls (RunImageAsync / RunPartitionAsync /
// RunTransferAsync), with PartitionMap.For("beyond0lte").
//
// Galaxy S10 (beyond1lte): put `--board beyond1lte` before the verb. Every verb then uses
// PartitionMap.Beyond1Lte (USERDATA sda34, CACHE sda31, this phone's GPT GUIDs) and the
// SM-G973F model string; `check` confirms the phone's sizes match before anything writes.
//
//   image    <media.iso|install.wim> <index> <driversRoot> <workDir>
//   check    <adb.exe> <serial>
//   backup   <adb.exe> <serial> <dir>           (identity partitions: EFS, PARAM, ... - read-only on the phone)
//   transfer <adb.exe> <serial> <workDir>      (writes USERDATA and CACHE - destructive)
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;

var board = "beyond0lte";
if (args.Length >= 2 && args[0] == "--board")
{
    board = args[1].ToLowerInvariant();
    args = args[2..];
}
if (board is not ("beyond0lte" or "beyond1lte"))
{
    Console.Error.WriteLine($"unknown board {board}: beyond0lte (S10e) or beyond1lte (S10)");
    return 1;
}
var geometry = PartitionMap.For(board);
var model = board == "beyond1lte" ? "SM-G973F" : "SM-G970F";
Console.WriteLine($"board {board} ({model})");
var runner = new ProcessRunner();
var log = new Progress<string>(m => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {m}"));

switch (args.FirstOrDefault())
{
    case "image" when args.Length == 5:
    {
        var media = new WindowsMedia(runner);
        var install = await media.ResolveInstallImageAsync(args[1])
            ?? throw new InvalidOperationException("No install.wim/esd in the media.");
        var index = int.Parse(args[2], System.Globalization.CultureInfo.InvariantCulture);
        var drivers = ImageBuilder.DiscoverDrivers(args[3]);
        Console.WriteLine($"install image {install}, index {index}, build {await media.GetBuildAsync(install, index)}");
        foreach (var d in drivers)
        {
            Console.WriteLine($"driver {d.Name} <- {d.Directory}");
        }
        var work = args[4];
        Directory.CreateDirectory(work);
        var vhdx = Path.Combine(work, "s10ewindows.vhdx");
        if (File.Exists(vhdx))
        {
            File.Delete(vhdx);
        }
        var unattend = board == "beyond1lte"
            ? new UnattendOptions { Username = "S10", ComputerName = "GALAXY-S10" }
            : new UnattendOptions { Username = "S10e", ComputerName = "GALAXY-S10E" };
        var built = await new VhdxImageBuilder(runner, null, null, geometry).BuildAsync(vhdx, install, index, drivers,
            SlimProfile.Lite, unattend, Path.Combine(work, "out"), log);
        Console.WriteLine($"windows.img {built.WindowsImage} {built.WindowsBytes} B sha256 {built.WindowsSha256}");
        Console.WriteLine($"esp {built.EspDirectory}");
        Console.WriteLine($"winload sha256 {built.LoaderSha256}");
        Console.WriteLine($"ntoskrnl sha256 {built.KernelSha256}");
        return 0;
    }
    case "check" when args.Length == 3:
    {
        // RunPartitionAsync's read-only verification: names present, USERDATA and CACHE sizes.
        var twrp = new TwrpClient(args[1], args[2], runner);
        var parts = await twrp.ListPartitionsAsync();
        foreach (var name in new[] { PartitionMap.WindowsTarget, PartitionMap.UefiTarget, PartitionMap.EfiSystemPartition,
            PartitionMap.RecoveryTarget, PartitionMap.Misc })
        {
            var actual = parts.Keys.FirstOrDefault(k => k.Equals(name, StringComparison.OrdinalIgnoreCase))
                ?? throw new InvalidOperationException($"missing partition {name}");
            Console.WriteLine($"{actual}: {await twrp.PartitionSizeAsync(actual)} B");
        }
        foreach (var (name, expected) in new[] { (PartitionMap.WindowsTarget, geometry.WindowsBytes),
            (PartitionMap.EfiSystemPartition, geometry.CacheBytes) })
        {
            var actual = parts.Keys.First(k => k.Equals(name, StringComparison.OrdinalIgnoreCase));
            var size = await twrp.PartitionSizeAsync(actual);
            if (size != expected)
            {
                Console.WriteLine($"MISMATCH {actual}: {size} B, geometry says {expected} B");
                return 2;
            }
        }
        Console.WriteLine($"partitions match the {board} geometry");
        return 0;
    }
    case "backup" when args.Length == 4:
    {
        // RunBackupAsync's call: BackupService over PartitionMap.IdentityBackup, hashed on phone and PC.
        var twrp = new TwrpClient(args[1], args[2], runner);
        var manifest = await new BackupService(twrp).BackupAsync(args[3], model, args[2], log);
        Console.WriteLine($"Backed up {manifest.Partitions.Count} partitions to {args[3]}.");
        return 0;
    }
    case "esp-type" when args.Length == 4:
    {
        // W30: Windows specialize needs a real EFI system partition ("BCD: Failed to get system
        // partition c0000452"). The S9+ flow types its secondary slot with GptTypeService; on the
        // S10e the ESP is CACHE itself, so type CACHE. Original sectors are saved to <dir>.
        var twrp = new TwrpClient(args[1], args[2], runner);
        var changed = await new GptTypeService(twrp).EnsureEspTypeAsync(PartitionMap.EfiSystemPartition, args[3], log);
        Console.WriteLine(changed ? "CACHE is now typed as an EFI system partition." : "CACHE already had the EFI system partition type.");
        return 0;
    }
    case "transfer" when args.Length == 4:
    {
        var twrp = new TwrpClient(args[1], args[2], runner);
        var outDir = Path.Combine(args[3], "out");
        if (await twrp.IsMountedAsync("/sdcard"))
        {
            await twrp.UnmountAsync("/sdcard");
        }
        try
        {
            await new TransferService(twrp).WriteRawImageAsync(PartitionMap.WindowsTarget, Path.Combine(outDir, "windows.img"),
                new RawWriteOptions { Verify = true }, mountToEnsureUnmounted: "/data", log: log);
            await new BootFilesService(twrp, geometry).WriteAsync(Path.Combine(outDir, "esp"), log);
        }
        finally
        {
            await twrp.ClearInstallStatusAsync(CancellationToken.None);
        }
        Console.WriteLine("Windows and boot files written and verified on the phone.");
        return 0;
    }
    default:
        Console.Error.WriteLine("usage: [--board beyond0lte|beyond1lte] image <media> <index> <driversRoot> <workDir> | check <adb> <serial> | backup <adb> <serial> <dir> | transfer <adb> <serial> <workDir>");
        return 1;
}
