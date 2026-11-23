# Init task (ทำครั้งเดียวตอน boot)

## หน้าที่
พาระบบจาก reset ไปถึงสถานะพร้อมทำงานตามลำดับที่ตกลงไว้ (S6-1) สร้าง task อื่น
แล้วลบตัวเองทิ้ง ระหว่างนี้เป็นคน toggle WDT ใน boot phase (S4-1)

## Owns
- ลำดับ boot และเส้นตายรวมของ boot
- การ toggle PE7 **เฉพาะช่วง boot phase** (หลังจากนั้นเป็นของ Supervisor)

## ลำดับ
```
main() — ก่อน scheduler
 1. toggle PE7 (เริ่ม boot phase ของ WDT, ต้องภายใน 1.12 s หลัง reset)
 2. HAL / clock init (HSE 16 MHz) — MX_RTC_Init ต้องเช็ก magic และไม่ reset backup domain
 3. อ่าน reset cause จาก RCC_CSR เก็บไว้ให้ Supervisor แล้ว clear flag
 4. ตั้งขาที่ต้องปลอดภัยตั้งแต่ต้น: PD1 = LOW (EC25), PE1 = LOW (RS485), PE0/PE2 ตามค่า default
 5. start scheduler → Init task

Init task — priority สูง
 6. I2C init → recovery bus (9 clock + STOP) → อ่าน PCA (I1) พร้อม retry
    ถ้าล้มเหลว: Control เริ่มในสถานะ UNKNOWN และห้ามเขียน (I3)
    นี่ต้องเป็น I2C transaction แรกของระบบ ห้ามใครใช้ I2C ก่อน (รวม OLED)
 7. ตรวจ pending_restore ใน BKP เทียบกับ PCA (ดู control.md)
 8. อ่าน PB3 → ถ้าไม่มี 24 V ใช้กฎ (ค) ทันที
 9. สร้าง Control → พร้อมรับคำสั่ง
10. ตรวจ safe mode (Supervisor: reset ≥ 5 ครั้งใน 10 นาที)
11. สร้าง Acquisition, Power, Storage, Local UI (OLED init หลังข้อ 6 เท่านั้น), Diagnostics
12. ถ้าไม่ใช่ safe mode: สร้าง Network (เริ่มแบบไม่ block มี timeout ของตัวเอง)
13. รอ heartbeat แรกของ task ที่จำเป็น → ส่งต่อ WDT ให้ Supervisor → ลบตัวเอง
```
- toggle PE7 ระหว่างทุกขั้นที่อาจใช้เวลา
- เส้นตายรวมของ boot (เสนอ 5 s, ปรับตามที่วัดได้ใน open-items H7) ถ้าเลยให้หยุด toggle แล้วปล่อยให้ WDT reset
- network init และการรอ link **ไม่อยู่ในเส้นทาง boot** ถ้า ETH เสีย รีเลย์ยังต้องใช้ได้
- log tick ของแต่ละขั้นเพื่อวัด margin ของเวลา boot

## Safe mode (S6-5)
สร้างแค่ Control, Local UI, Diagnostics (และ Supervisor) ข้าม Network/Storage ยก alarm
รีเลย์ยังทำตาม I1–I5 ทุกกรณี

## เมื่อเกิด fault
ขั้นไหนล้มเหลวให้บันทึกและเดินต่อแบบ degraded ยกเว้นข้อ 6–9 ที่ Control ต้องเริ่มได้เสมอ
(แม้อยู่ในสถานะ UNKNOWN)

## การทดสอบ
- boot ภายใน 1.12 s ต่อ toggle แรก และครบทุกขั้นภายในเส้นตาย เมื่อถอด jumper DIS_WDT
- boot โดยไม่เสียบสาย LAN, ไม่มี 24 V, I2C ค้าง
- reset ระหว่าง I2C write แล้วตรวจว่า latch ไม่เปลี่ยน
