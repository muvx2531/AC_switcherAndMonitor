# Driver: RTC backup registers

ไม่ใช่ task เป็นการแบ่ง RTC backup register (`RTC->BKP0R` … `BKP19R`, 20 × 32-bit) ให้เจ้าของแต่ละ slot

## คุณสมบัติ
- รอดผ่าน reset ทุกแบบ (WDT, NRST, software, fault) และรอดผ่านไฟดับเพราะขา VBAT
  มี CR1220 (BT401) diode-OR กับ +3V3_MCU
- หายเมื่อสั่ง backup domain reset (`RCC_BDCR.BDRST`), tamper หรือถ่านหมดระหว่างที่ VDD ดับ
- ต้องเปิด PWR clock และ `HAL_PWR_EnableBkUpAccess()` ก่อนเขียน
- เขียนได้ทีละ 32-bit ข้อมูลหลาย register ต้องมี magic เขียนเป็นตัวสุดท้าย และลบเป็นตัวแรก

## Slot map (ข้อเสนอ)
| Register | เจ้าของ | เนื้อหา |
|---|---|---|
| BKP0R | Time module (Supervisor) | magic ว่า RTC ถูกตั้งแล้ว `MX_RTC_Init` ต้องเช็กค่านี้และไม่ตั้งเวลาใหม่ |
| BKP1R | Control | `pending_restore` magic |
| BKP2R | Control | `channel_mask` (8 bit) + `duration_s` (16 bit) |
| BKP3R | Control | `request_id` |
| BKP4R | Control | CRC ของ BKP2R–BKP3R |
| BKP5R | Supervisor | จำนวน reset ในหน้าต่างปัจจุบัน |
| BKP6R | Supervisor | เวลาเริ่มหน้าต่าง (RTC) |
| BKP7R | Supervisor | สาเหตุ reset ล่าสุด |
| BKP8R | Supervisor | PC ตอน HardFault ล่าสุด |
| BKP9R–BKP19R | สำรอง | — |

## ต้องเปลี่ยน
`MX_RTC_Init()` ปัจจุบันเรียก `HAL_RTC_SetTime` ทุก boot ต้องแก้ใน `USER CODE` ให้ข้ามเมื่อ BKP0R มี magic
