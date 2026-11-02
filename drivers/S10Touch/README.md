# S10Touch — driver cảm ứng mới cho Galaxy S10

Viết lại từ đầu (11/2026) thay cho `SecTsY761Touch` / `S6SY761TouchBeyond0`. Cùng phần cứng
(`ACPI\SCSY0761`: chip sec_ts **y771** (HID vẫn mang tên 0761) trên I2C0, chân ngắt gpa1-0 = EINT8, GSIV 45), khác cách chạy:

| | Driver cũ | S10Touch |
|---|---|---|
| Xử lý ngắt | ISR → work item hệ thống (độ ưu tiên thường, bị CPU vẽ màn hình chiếm) | ISR → DPC → **luồng riêng ưu tiên real-time** (24) |
| Đọc I2C | tạo request mới mỗi lần | 1 request dùng lại, không cấp phát trên đường nhập |
| Hàng đợi | khóa + work item + timer | chỉ luồng worker chạm dữ liệu, không khóa |
| Giữ ngón tay | chip im → Windows không nhận khung | gửi lại khung hiện tại mỗi `RepeatMs` (10 ms) khi ngón còn chạm và chip đã im ≥ 2×RepeatMs |
| Kéo | dồn khung khi tắc | khung "chỉ di chuyển" thay khung cũ chưa gửi → luôn vị trí mới nhất; khung chạm/nhả không bao giờ bị gộp |
| Toạ độ | vá descriptor lúc chạy | 0..4095 ghi thẳng trong descriptor, kích thước vật lý 2.61 × 5.51 in |

Khóa `Device Parameters` của thiết bị:
- `RepeatMs` (DWORD, mặc định 10, 0 = tắt, tối đa 100).
- Driver ghi lại: `S10tBootStatus` (0x20 = chip sẵn sàng), `S10tStartStatus`, `S10tIrqs`, `S10tFrames`,
  `S10tRepeats`, `S10tCoalesced`, `S10tDelivered`, `S10tI2cErrors`, `S10tInvalidEvents`, `S10tFifoFull`,
  `S10tMaxIrqToReportUs` (độ trễ lớn nhất từ ngắt tới lúc giao khung cho Windows, µs).

Chưa build/chạy thử trên máy (chỉ kiểm tra cú pháp với header giả). Ghi chú: `notes/s10-touch-driver.md`.

## 1.0.1.0
- Mỗi lần chạm/nhấc trong một lô sự kiện có khung riêng, đúng thứ tự (trước đây cả lô gộp thành 1 khung → nhấc + chạm nhanh bị nuốt mất lần nhấc → Windows tưởng 2 ngón → loạn).
- Lỗi I2C / tràn stack / READ_ALL lỗi → nhấc toàn bộ ngón (như SecTsY761Touch cũ), không còn "ngón ma" kẹt.
- Bỏ qua tọa độ HOVER / PROXIMITY / FLIP COVER / STYLUS (như sec_ts Linux).
- SPB timeout 100 → 250 ms; lặp khung chỉ khi chip im ≥ 2×RepeatMs (mặc định 10 ms), không chen vào nhịp ~120 Hz của chip.

## 1.0.2.0
- Chip của S10 là **y771** (`CONFIG_TOUCHSCREEN_SEC_TS_Y771=y` trong beyond1lte_defconfig), không phải y761: số sự kiện còn lại chỉ có **5 bit** (`& 0x1F`), bit 5 là cờ `max_energy_flag`. Bản cũ dùng `& 0x3F` → mỗi khi cờ bật, driver đọc thêm 32 sự kiện cũ trong stack và phát lại thành chạm ma. Đây nhiều khả năng là thủ phạm chính của "loạn cảm ứng".
- Tối đa 31 sự kiện/lô như `sec_ts_read_event`.
- Sự kiện trạng thái: chip tự khởi động lại (watchdog/ESD, `INFO 0x00 0x20`) → nhả hết ngón, gửi lại touch function + sense on; báo đầy hàng đợi (`ERR 0x01`) → nhả hết ngón.

## 1.0.3.0
- Ghi trạng thái driver vào RAM dành riêng (WINPRAM +0xE000) mà UEFI lưu thành `WINPRAM.BIN` trên ESP sau khi Windows bị ép tắt / treo. Không có cảm ứng thì chỉ còn cách ép tắt, nên registry và event log mất hết; cái này thì còn. `get-logs-twrp.cmd` lấy nó về.
  - dòng 1: kết quả D0Entry (boot status, số lần thử, ControllerStart)
  - dòng 2-3: nhịp tim mỗi giây: số ngắt, sự kiện, khung, khung đã giao, số lệnh đọc của hidclass, lỗi I2C, sự kiện bị loại, FIFO đầy, reinit, mức chân ngắt, thanh ghi EINT, độ trễ lớn nhất
  - dòng 4-63: 60 sự kiện thô gần nhất của chip
- Không đổi cách xử lý cảm ứng so với 1.0.2.0.

## 1.0.4.0
- Khởi động chip (chờ app mode, touch function, sense on) chuyển sang luồng worker; `D0Entry` trả về ngay. Bản 1.0.3 chỉ kịp ghi dòng tiêu đề vào PRAM rồi im lặng cả phiên: nó kẹt trong `D0Entry` (mở SPB / đọc boot status) hoặc `PrepareHardware` lỗi. Giờ từng bước đều có dòng PRAM: PrepareHardware (tài nguyên), D0Entry, SpbOpen, interrupts on, từng lần thử boot status (mã lỗi + giá trị), kết quả start.

## 1.0.5.0
- PRAM của 1.0.4 chỉ ra lỗi: `SpbOpen c0000184` (STATUS_INVALID_DEVICE_STATE) - lúc driver cảm ứng mở kết nối I2C thì bộ điều khiển I2C chưa sẵn sàng (đang khởi động, hoặc đang ngủ D3 do tự tắt sau 1 s rảnh). `D0Entry` thất bại một lần là thiết bị cảm ứng chết cả phiên.
- Mở SPB giờ do worker làm, thử lại mỗi 100 ms trong 10 s rồi mỗi giây, có log PRAM. Đi kèm Exynos9820Hsi2c 1.0.1.0 không còn tự ngủ D3.

## 1.0.6.0
- Sửa màn hình xanh WDF_VIOLATION 0x10D (5 = WDF_INVALID_HANDLE, handle 0) của 1.0.5: request SPB dùng lại được tạo ngay sau `WdfIoTargetCreate`, lúc target chưa mở → `WdfRequestCreate` báo `c0000184`; lần thử lại bỏ qua bước tạo, mở được target rồi gửi request NULL. Giờ request chỉ được tạo sau khi target mở thành công; SpbRead/SpbWrite từ chối khi chưa mở.

## 1.0.7.0
- Người dùng (2026-11-03): chạm 1 lần thành 2, kéo bị đúp ở điểm đầu/cuối, đứt đoạn; **3 ngón kéo = chạm giật giật dọc đường kéo**.
- Lỗi I2C không còn nhấc mọi ngón ngay (bản 1.0.1-1.0.6 làm vậy, lần đọc kế tiếp thành một lần chạm mới → chạm đúp). Chỉ nhấc khi lỗi liên tiếp 5 lần (như sec_ts Linux: bỏ lô đó thôi).
- Không dùng READ_ALL (đọc dài left×8 byte, mọi lô nhiều ngón đều cần) nữa: đọc từng sự kiện 8 byte bằng READ_ONE_EVENT (lệnh này lấy ra một sự kiện khỏi stack), đúng kiểu đọc đã chạy ổn ở UEFI và với 1 ngón.
- PRAM: thêm dòng F+/F- cho mỗi lần chạm/nhấc, lý do mỗi lần nhấc toàn bộ, thời gian đọc SPB lâu nhất và số lần đọc > 20 ms; chỉ ghi 1/16 sự kiện move để 60 dòng đủ cho cả một thao tác.
