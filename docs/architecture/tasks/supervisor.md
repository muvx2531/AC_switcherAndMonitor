# Supervisor task

## หน้าที่
ตัดสินว่าระบบยังทำงานดีพอที่จะ toggle watchdog หรือไม่ เก็บเหตุผลของการ reset
ตรวจ boot loop และดูแล time module (RTC)

## Owns
| Resource | หมายเหตุ |
|---|---|
| PE7 Clr_wdt (หลัง boot phase) | STWD100 (WDT801) ต้อง toggle ภายใน 1.12 s, ปิดได้ด้วย jumper DIS_WDT801 เท่านั้น |
| health registry (heartbeat ของแต่ละ task) | — |
| reset cause (จาก Init), จำนวน reset, ข้อมูล crash | BKP slot ของ Supervisor ดู [bkp-rtc](../drivers/bkp-rtc.md) |
| RTC + magic ของ RTC (Time module) | UTC ภายใน, ระบุว่าเวลา valid หรือไม่, ห้าม reset backup domain |

## กฎ
- **WDT 2 ช่วง (S4-1):** boot phase เป็นของ [Init](init.md) Supervisor รับต่อเมื่อ task ที่จำเป็นส่ง heartbeat แรกครบ
- toggle เฉพาะเมื่อ task ที่จำเป็นในสถานะปัจจุบันส่ง heartbeat ภายในกรอบเวลาของตัวเอง
  (เช่น Network ไม่จำเป็นใน BACKUP/LOW_POWER/safe mode)
- ไม่มี task อื่นได้ toggle PE7
- priority ต่ำกว่า Control/Acquisition/Power โดยตั้งใจ ถ้า task ข้างบนกิน CPU จนตัวเองไม่ได้รัน ก็ควร reset
- **Safe mode (S6-5):** reset ≥ 5 ครั้งใน 10 นาที → boot ครั้งถัดไปเข้า safe mode (ให้ Init อ่านค่านี้)
  ใช้เวลา RTC ถ้า valid ถ้าไม่ valid ใช้จำนวน reset ติดกันแทน
- HardFault handler บันทึก PC/LR/สาเหตุลง BKP ก่อน reset (ถ้าทำได้อย่างปลอดภัย)
- ตรวจ stack high-water, heap ต่ำสุด, คิวเต็ม/drop และรายงานเป็น diagnostic

## Input
heartbeat (progress counter ต่อ task), fault report จาก task ต่างๆ

## Output
สถานะสุขภาพระบบ (snapshot), event ไปที่ Storage

## การทดสอบ (gate ของ Phase 1 และ 4)
- ถอด jumper DIS_WDT: boot ปกติ, ไม่เสียบ LAN, จำลอง task ค้าง → WDT reset จริง และหลัง reset PCA ยังคงสถานะเดิม (I1)
- จำลอง boot loop ให้ถึงเกณฑ์ → เข้า safe mode
