# Kit Windows cho Galaxy S10 (beyond1lte, SM-G973F)

Y hệt quy trình kit z3s (S20 Ultra): **EWDK → build driver → make-windows.ps1 → TWRP →
flash-windows.cmd**. Chỉ khác layout máy, bộ driver và UEFI.

| File | Việc |
|---|---|
| `build-s10.cmd` | v1.3: build + ký `Exynos9820Hsi2c`, `SecTsY761Touch` (cảm ứng mới), `Exynos9820Ufs` (board **Beyond1**), `S10eBattery` (pin/sạc), `Exynos9820WlanCx` (WiFi), `Exynos9820Audio` (loa), `S10eDiag` (phím), + GPU (tùy chọn): `MaliG76Probe`, `MaliG76Wddm` → `out\` |
| `get-wifi-fw.cmd` | Kéo firmware WiFi BCM4375 **của chính con S10** ra `bcm4375\` (TWRP hoặc Android có root) |
| `install-in-windows.ps1` | Đã có Windows rồi: chạy **trên máy** (PowerShell Admin) để thêm cảm ứng mới, pin, WiFi, loa, phím mà không cài lại |
| `gpu-s10.ps1` | GPU giai đoạn G1, chạy **trên máy** (PowerShell Admin): `probe` (Mali chạy thử 1 job), `display` (driver màn hình Mali), `status`, `off`. Cần UEFI **v0.1.3+** |
| `setup\SetupComplete.cmd` | Chạy cuối Setup: cài dịch vụ phím (S10eDiag + S10eAgent), tắt sleep màn hình |
| `make-windows.ps1` | v2.2-s10: dựng Windows từ ISO → `C:\s10\windows\` (pieces + uefi.img + flash-windows.cmd) |
| `set-esp-type.ps1` | v1.3-s10: đổi type `cache` thành ESP trong cả 2 bản GPT (flash-windows.cmd tự gọi) |
| `z3s-disk.cs` | Helper C# của kit z3s (giữ nguyên tên, chỉ sửa 1 dòng thông báo) |
| `new-cert.ps1` | Chứng chỉ test "z3s WoA Test" — **dùng chung với kit z3s**, có rồi thì khỏi chạy |
| `beyond1lte-uefi-v*.img` | UEFI cho S10 (make-windows tự lấy bản mới nhất) |

## 1. Build driver (cửa sổ EWDK)

```bat
C:\Users\Admin\Downloads\EWDK\LaunchBuildEnv.cmd
cd /d <kit>
build-s10.cmd
```

UFS **không** build từ `.vcxproj`: `build-s10.cmd` gọi `build-ufs.ps1 -WideUncached -DmaWindow
-Board Beyond1` (đúng profile đã chạy trên S10e). Script này tự tìm VS Build Tools và SDK
10.0.28000 của EWDK.

## 1b. Firmware WiFi (một lần, máy ở TWRP)

```bat
get-wifi-fw.cmd
```

## 2. Dựng Windows (PowerShell Admin)

```powershell
cd <kit>
Set-ExecutionPolicy -Scope Process Bypass
.\make-windows.ps1 -Source D:\iso\22621_arm64.iso -UserName Anh
```

## 2b. GPU (tùy chọn, sau khi Windows đã chạy)

Flash `beyond1lte-uefi-v0.1.3.img` (bật nguồn khối G3D và cho Windows thấy thiết bị `GPU0`), rồi
trên máy, PowerShell Admin, trong thư mục kit có `out\`:

```powershell
.\gpu-s10.ps1 probe     # Mali-G76 chạy 1 job thật; RESULT: PASS là GPU sống
.\gpu-s10.ps1 display   # đổi sang driver màn hình Mali (display-only), rồi reboot
.\gpu-s10.ps1 status    # sau reboot: FbMatchesGop = 1 là đúng màn S10
```

Kết quả ghi vào `C:\S10Prov\gpu-report.txt`. Màn đen sau `display`: vào TWRP, tạo file
`\S10EDIAG\nogpu.flg` trên ESP (GPU0 biến mất ở lần boot sau) hoặc flash lại v0.1.2.
`.\gpu-s10.ps1 off` trả màn hình về Microsoft Basic Display.

Tạo `nogpu.flg` từ PC khi máy ở TWRP:

```bat
adb shell "mkdir -p /tmp/esp && mount -t vfat /dev/block/by-name/cache /tmp/esp && mkdir -p /tmp/esp/S10EDIAG && touch /tmp/esp/S10EDIAG/nogpu.flg && sync && umount /tmp/esp"
```

Xóa file đó (`rm /tmp/esp/S10EDIAG/nogpu.flg`) là GPU0 hiện lại.

## 3. Ghi vào máy

Vào TWRP, cắm cáp, chạy `C:\s10\windows\flash-windows.cmd`.

## Các chốt an toàn (khác bản z3s)

- **Driver UFS sai board là bị chặn.** Trong `.sys` có bản ghi `UFSBRD01`. `make-windows.ps1`
  chỉ nhận board 2 (S10) với fence bắt đầu ở LBA 3118592 (`cache`). Driver UFS của S10e mà lọt
  vào S10 thì sẽ ghi vào phân vùng Android.
- **Sai máy là bị chặn.** `flash-windows.cmd` và `set-esp-type.ps1` đọc `ro.boot.bootloader`,
  không phải `G973*` thì dừng trước khi ghi gì. Anh có cả con S20U, cắm nhầm cũng không sao.
- **GUID của `cache`/`userdata` trên con S10 này là ngẫu nhiên**, không phải kiểu Samsung tự
  sinh theo tên như S20U/S10e. BCD trỏ theo GUID, nên `set-esp-type.ps1` so lại GUID trên máy
  trước khi ghi. Sau này flash Odin full (PIT) mà GUID đổi thì nó dừng và báo. Khi đó chạy lại
  `make-windows.ps1 -EspGuidText ... -WinGuidText ...` với GUID mới.
- **`userdata` của S10 không tròn MiB** (114 482 458 624 B). Pipeline của kit z3s xuất theo MiB,
  nên volume NTFS là 109178 MiB, chừa trống 251 LBA cuối `userdata`. Bước kiểm tra kích thước
  trong `flash-windows.cmd` vẫn so với cả phân vùng.

## Layout (đọc từ GPT của con S10 này, 02/11/2026)

| Phân vùng | LBA (4096 B) | Kích thước | GUID |
|---|---|---|---|
| `boot` sda14 | 27264..41343 | 57 671 680 B | — |
| `cache` sda31 → ESP | 3118592..3272191 | 629 145 600 B | `21ca1362-c5e9-45d2-b5bd-3f4dd0f30251` |
| `userdata` sda34 → Windows | 3286272..31236090 | 114 482 458 624 B | `712bcd12-3071-4f1c-8951-9c75a5ecca08` |

GPT có bản dự phòng hợp lệ ở LBA 31236091..31236095; `set-esp-type.ps1` sửa cả hai bản.
