# S10Touch — driver cảm ứng mới cho Galaxy S10

Viết lại từ đầu (11/2026) thay cho `SecTsY761Touch` / `S6SY761TouchBeyond0`. Cùng phần cứng
(`ACPI\SCSY0761`: chip sec_ts y761 trên I2C0, chân ngắt gpa1-0 = EINT8, GSIV 45), khác cách chạy:

| | Driver cũ | S10Touch |
|---|---|---|
| Xử lý ngắt | ISR → work item hệ thống (độ ưu tiên thường, bị CPU vẽ màn hình chiếm) | ISR → DPC → **luồng riêng ưu tiên real-time** (24) |
| Đọc I2C | tạo request mới mỗi lần | 1 request dùng lại, không cấp phát trên đường nhập |
| Hàng đợi | khóa + work item + timer | chỉ luồng worker chạm dữ liệu, không khóa |
| Giữ ngón tay | chip im → Windows không nhận khung | gửi lại khung hiện tại mỗi `RepeatMs` (8 ms) khi ngón còn chạm |
| Kéo | dồn khung khi tắc | khung "chỉ di chuyển" thay khung cũ chưa gửi → luôn vị trí mới nhất; khung chạm/nhả không bao giờ bị gộp |
| Toạ độ | vá descriptor lúc chạy | 0..4095 ghi thẳng trong descriptor, kích thước vật lý 2.61 × 5.51 in |

Khóa `Device Parameters` của thiết bị:
- `RepeatMs` (DWORD, mặc định 8, 0 = tắt, tối đa 100).
- Driver ghi lại: `S10tBootStatus` (0x20 = chip sẵn sàng), `S10tStartStatus`, `S10tIrqs`, `S10tFrames`,
  `S10tRepeats`, `S10tCoalesced`, `S10tDelivered`, `S10tI2cErrors`, `S10tInvalidEvents`, `S10tFifoFull`,
  `S10tMaxIrqToReportUs` (độ trễ lớn nhất từ ngắt tới lúc giao khung cho Windows, µs).

Chưa build/chạy thử trên máy (chỉ kiểm tra cú pháp với header giả). Ghi chú: `notes/s10-touch-driver.md`.
