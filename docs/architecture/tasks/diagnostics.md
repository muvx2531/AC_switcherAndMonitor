# Diagnostics task

## หน้าที่
debug log และ serial protocol ทาง USART6 สำหรับ bring-up และการบำรุงรักษาผ่านสายตรง
เป็น adapter: คำสั่งที่มีผลกับรีเลย์ต้องส่งผ่าน `Control_Submit` (`source = SRC_SERIAL`)

## Owns
| Resource | หมายเหตุ |
|---|---|
| USART6 TX (DMA2 Stream 6) | ผ่าน driver [debug-log](../drivers/debug-log.md) (task `debugTx`) |
| USART6 RX (DMA2 Stream 1, circular 512 B) + parser | `debug_protocol.c` (task `debugRx`) |

USART6: PC6 TX / PC7 RX, 115200 8-N-1, header P401 รูปแบบ frame ดู `docs/serial-protocol.md`

## ต้องเปลี่ยนจากโค้ดปัจจุบัน
- `SET` เขียน PCA เอง → แปลงเป็น `SET_ON`/`SET_OFF` ผ่าน Control
- debug hook ใน `main.c` (`relayPatternRequest`, `relay3TestRequest`) ที่เขียน PCA ตรงๆ → ลบ หรือย้ายมาเป็นคำสั่งที่ผ่าน Control
- `BACKUP` / `BATV` เริ่ม ADC2 DMA เอง → อ่านจาก Acquisition snapshot แทน
- priority ของ `debugRx` จาก `tskIDLE_PRIORITY + 25` ลงเป็นต่ำสุด (S4-4)
- เพิ่มคำสั่งตั้ง static IP สำรอง (S7-5)
- UART RX error ตอนนี้หยุดรับจนกว่าจะ reset ต้องมี recovery

## กฎ
- log เป็น best effort ข้อความหายได้ ไม่ใช่ audit log (event สำคัญไปที่ Storage)
- checksum ไม่ใช่ authentication ใช้กับสายตรงที่เชื่อถือได้เท่านั้น
- `DebugLog_Init` และ `DebugProtocol_Start` ต้องเรียกก่อน log ข้อความแรก เพราะ HAL ลงทะเบียน callback ได้เฉพาะตอน TX ว่าง
- error callback ของ USART6 ใช้ร่วมกันทั้ง TX และ RX

## เวลา
priority ต่ำสุด

## การทดสอบ
- log ปริมาณมากพร้อมกับคำสั่ง serial, buffer wrap, RX error แล้ว recovery
