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
| BKP1R | Control | `0x5052` ใน 16 bit บน + channel mask (bit 0–3) เขียนเป็นตัวสุดท้าย ลบเป็นตัวแรก |
| BKP2R | Control | duration_s ของ CH1 (16 bit ล่าง) และ CH2 (16 bit บน) |
| BKP3R | Control | duration_s ของ CH3 และ CH4 |
| BKP4R | Control | ค่าตรวจสอบของ BKP1R–BKP3R |
| BKP5R | Supervisor | `0x5243` ใน 16 bit บน + จำนวน reset ในหน้าต่างปัจจุบัน (เขียนโดย `Boot_EvaluateSafeMode` ตอน boot, ล้างโดย Supervisor/`CLRSAFE`) |
| BKP6R | Supervisor | เวลาเริ่มหน้าต่าง (วินาทีตั้งแต่ 2000-01-01 จาก RTC) |
| BKP7R | Supervisor | สาเหตุ reset ล่าสุด |
| BKP8R | Supervisor | PC ตอน HardFault ล่าสุด |
| BKP9R–BKP19R | สำรอง | — |

## สถานะ
ทำแล้วใน Phase 1: `MX_RTC_Init()` (USER CODE `Check_RTC_BKUP`) ข้ามการตั้งเวลาเมื่อ BKP0R = `0x52544331` ("RTC1")
และเขียน magic หลังตั้งเวลาครั้งแรก ทดสอบแล้วว่าเวลาคงอยู่ผ่าน reset (`docs/phase1-foundation.md`)
slot ของ Control และ Supervisor ยังไม่ได้ใช้
