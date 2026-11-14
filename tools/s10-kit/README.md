# Kit Windows cho Galaxy S10 (beyond1lte, SM-G973F)

Y hệt quy trình kit z3s (S20 Ultra): **EWDK → build driver → make-windows.ps1 → TWRP →
flash-windows.cmd**. Chỉ khác layout máy, bộ driver và UEFI.

| File | Việc |
|---|---|
| `build-s10.cmd` | v1.5: build + ký `Exynos9820Hsi2c`, `S10Touch` (cảm ứng **mới**), `Exynos9820Ufs` (board **Beyond1**), `S10eBattery` (pin/sạc), `Exynos9820WlanCx` (WiFi), `Exynos9820Audio` (loa, **bật sẵn**), `S10eDiag` (phím) → `out\`. GPU tạm gỡ |
| `get-wifi-fw.cmd` | v1.1: kéo firmware WiFi BCM4375 **của chính con S10** (`/vendor/firmware`) ra `bcm4375\` (TWRP hoặc Android có root). **Bắt buộc cho WiFi** |
| `install-in-windows.ps1` | v1.1: đã có Windows rồi: chạy **trên máy** (PowerShell Admin) để thêm cảm ứng, pin, WiFi (+firmware), loa (bật phần cứng), phím mà không cài lại; cuối cùng in trạng thái từng thiết bị |
| `update-ufs.ps1` | v1.0: thay driver UFS (Exynos9820Ufs w47) cho Windows **đang chạy**, chạy trên máy (PowerShell Admin). Không phải sửa LBA kể cả dual boot: script đọc layout thật, kiểm tra ổ Windows + ESP nằm trong fence (cache..cuối đĩa, trừ omr/cp_debug) rồi mới cài. `-Check` = chỉ kiểm tra. Cài cái này + reboot TRƯỚC khi flash UEFI v0.2.0 (UEFI mới bật ngắt UFS thật, driver cũ < w43 sẽ bão ngắt) |
| `push-to-windows.cmd` | **Không cần USB:** máy ở TWRP (Windows mount ở `/data`), chạy trên PC → đẩy driver + firmware WiFi thẳng vào Windows; lần đăng nhập tới bấm **Yes** là tự cài rồi khởi động lại |
| `collect-logs.ps1` | Chạy **trên máy** (PowerShell Admin): gom trạng thái thiết bị, log driver, `UEFILOG.TXT` trên ESP… vào **một file zip** `C:\S10Prov\s10-logs-*.zip` để gửi |
| `setup\SetupComplete.cmd` | Chạy cuối Setup: cài dịch vụ phím (S10eDiag + S10eAgent), tắt sleep màn hình |
| `make-windows.ps1` | v2.2-s10: dựng Windows từ ISO → `C:\s10\windows\` (pieces + uefi.img + flash-windows.cmd) |
| `set-esp-type.ps1` | v1.3-s10: đổi type `cache` thành ESP trong cả 2 bản GPT (flash-windows.cmd tự gọi) |
| `z3s-disk.cs` | Helper C# của kit z3s (giữ nguyên tên, chỉ sửa 1 dòng thông báo) |
| `new-cert.ps1` | Chứng chỉ test "z3s WoA Test" — **dùng chung với kit z3s**, có rồi thì khỏi chạy |
| `beyond1lte-uefi-v*.img` | UEFI cho S10 (make-windows tự lấy bản mới nhất) |

## 0. Cài lại Windows hoàn chỉnh (kit v2.8, make-windows v2.4-s10)

Bản dựng ra đã ở **trạng thái cuối**: GPU Mali (KMD 1.0.72.2 + UMD s10.4, độ sáng), S10ePower + governor
(GPU 806 MHz, A55 2002 MHz), giải mã video (nếu có `mfc\mfc_fw.bin`), không còn task/biến chẩn đoán nào.

1. Máy ở TWRP: `get-wifi-fw.cmd` (lấy firmware WiFi + `mfc_fw.bin`).
2. Mở **cửa sổ EWDK mới** (không `set S10_KEEP_UFS` / `S10_GPU` gì cả), `build-s10.cmd` → `out\` + `out-gpu\`.
3. PowerShell Admin: `.\make-windows.ps1 -Source <iso> -UserName Anh` → `C:\s10\windows\`.
4. TWRP: chạy `C:\s10\windows\flash-windows.cmd` trên PC.
5. Lần đầu vào desktop máy **tự khởi động lại 1 lần** (sau 20 s) để chuyển màn hình sang GPU Mali.

`-NoOc` = xung gốc, `-NoGpu` = không GPU, `-NoPower` = không S10ePower.

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

## 2b. Đã có Windows: cài/cập nhật driver + gom log

**Cách nhanh (từ TWRP, không cần chép tay):** máy vào TWRP, mount Windows ở `/data`, trên PC:

```bat
get-wifi-fw.cmd          :: một lần, lấy firmware WiFi của máy
push-to-windows.cmd      :: đẩy out\ + bcm4375\ vào C:\S10Prov\kit, firmware vào System32\drivers\bcm4375
```

Rồi Reboot > System. Đăng nhập Windows, bấm **Yes** ở hộp quyền Admin: `install-in-windows.ps1` chạy
(log `C:\S10Prov\install.log`) và máy tự khởi động lại một lần. Báo "read-only" nghĩa là Windows chưa
tắt hẳn (Fast Startup): vào Windows, Shut down, thử lại.

**Hoặc trên máy (PowerShell Admin):**

```powershell
.\install-in-windows.ps1    # cài cảm ứng / pin / WiFi / loa / phím, rồi reboot
.\collect-logs.ps1          # cái gì chưa chạy: gửi file C:\S10Prov\s10-logs-*.zip
```

GPU đang **tạm gỡ** (UEFI v0.1.5 ẩn `GPU0`, kit không build driver Mali; `set S10_GPU=1` trước
`build-s10.cmd` nếu muốn build lại).

## 2c. Giải mã video phần cứng (MFC, kit v2.6+)

1. Máy ở TWRP, trên PC: `get-wifi-fw.cmd` (v1.2) — lấy thêm `mfc_fw.bin` của máy vào `mfc\`.
2. EWDK: `build-s10.cmd` (v1.14) — có `mfc\mfc_fw.bin` thì build thêm `out\Exynos9820Mfc\`.
3. Chép kit (cả `out\`, `tools\`, `video\`) sang máy, PowerShell Admin trong thư mục kit:

```powershell
.\video-s10.ps1 install     # cài + chạy driver MFC (không cần reboot)
.\video-s10.ps1 all         # test giải mã (so từng khung với libavcodec) + phát thử Media Player / Edge
```

Gửi lại `C:\S10Prov\video-s10.txt`.

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
