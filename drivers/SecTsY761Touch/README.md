# SecTsY761Touch: cảm ứng Galaxy S10 (sec_ts y761)

Lấy driver **SecTsY79ATouch v0.2 của bản S20 Ultra (z3s)** (cái chạy ổn) rồi đổi sang chip
cảm ứng của S10. Thay hoàn toàn `S6SY761TouchBeyond0` (driver của S10e).

## Khác gì driver S10e cũ

| | S6SY761TouchBeyond0 (S10e) | SecTsY761Touch (S10) |
|---|---|---|
| Ngắt | ISR **passive-level** trên ngắt GIC (Windows chỉ hỗ trợ kiểu này cho ngắt từ GPIO controller) | ISR **DIRQL**: che EINT8, đẩy việc đọc I2C sang work item, đọc xong mở lại (y như bản S20U) |
| Khởi động chip | chỉ SENSE_ON | chờ app mode (`0x55` = `0x20`), bật touch function (`0x30` \|= `0x61`), SENSE_ON, xoá stack, như `sec_ts_probe` |
| Event lỗi (EID 3) | bỏ qua | xoá event stack, bỏ cả lô (như `sec_ts_read_event`) |
| DSDT | Level, ActiveLow | Level, **ActiveHigh** (UEFI S10 v0.1.2 trở lên) |
| Kích thước vật lý (HID) | 2.71 × 5.58 in (của S9+) | **2.60 × 5.51 in** (S10: 1440/554 dpi × 3040/551 dpi) |

Giữ theo chip y761 (giống S9+/S10e): event **8 byte**, số event còn lại ở byte 7 bit 0-5, tối đa
64 event một lần đọc, toạ độ 12 bit (0..4095). Mọi opcode (`0x10 0x30 0x55 0x60 0x61 0x62`) có
trong danh sách sec_ts của 9820 (`notes/exynos9820-research.md` §8).

Gắn `ACPI\SCSY0761` (node `\_SB.TSP0`), bus `\_SB.I2C0` (Exynos9820Hsi2c), ngắt gpa1-0 = SPI 13 =
GSIV 45, ALIVE GPIO `0x15850000` (thanh ghi EINT giống hệt S20U). Nguồn LDO19M/LDO27M do UEFI bật.

## Giá trị debug

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SCSY0761\0\Device Parameters`, ý nghĩa giống README
của SecTsY79ATouch: `TouchBootStatus` 0x20 = chip OK, `TouchFunction` = 0x0061,
`TouchIrqCount` / `TouchI2cErrorCount` / `TouchOverflowCount`.

**Chưa chạy trên máy.** Giấy phép: GPL-2.0 (xem `LICENSE`).
