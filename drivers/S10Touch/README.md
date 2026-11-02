# S10Touch — driver cảm ứng mới cho Galaxy S10

Viết lại từ đầu (11/2026) thay cho `SecTsY761Touch` / `S6SY761TouchBeyond0`. Cùng phần cứng
(`ACPI\SCSY0761`: chip sec_ts y761 trên I2C0, chân ngắt gpa1-0 = EINT8, GSIV 45), khác cách chạy:

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
