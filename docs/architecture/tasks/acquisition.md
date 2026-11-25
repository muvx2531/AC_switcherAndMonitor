# Acquisition task

## หน้าที่
วัดแรงดัน AC หลังรีเลย์ทั้ง 4 ช่อง กระแส CT และ VBAT แล้วให้ snapshot ที่มี validity
ใช้เป็น contact feedback ของ Control และข้อมูลให้ Power/UI

## Owns
| Resource | หมายเหตุ |
|---|---|
| ADC1 + ADC2 (dual regular simultaneous) | S3-3 |
| Timer trigger 6.4 kHz (128 samples/รอบ 50 Hz) | เลือก timer ตอน implement (เช่น TIM3 TRGO) |
| DMA stream ของ ADC | ต้องไม่ชนกับ DMA2 Stream 1/6 ของ USART6 |
| ADC1 injected channel (VBAT) | trigger ด้วย software ทุก 1 s, sampling time ยาว (source ~37 kΩ) |
| DIAG_S1..4 (PD8/PD10/PD12/PD14) | อ่านอย่างเดียว |

## สัญญาณ (จาก HW doc, VREF+ = 3.000 V)
| สัญญาณ | ขา / channel | สเกล |
|---|---|---|
| AC 1..4 คู่ P/N | PC0/PC2 (IN10/12), PA0/PA3 (IN0/3), PA4/PA5 (IN4/5), PB0/PB1 (IN8/9) | Vline = (P − N) × 200.1 |
| CT | PA6 (IN6) | bias 1.5 V, I = (ADC − ค่าเฉลี่ย DC) × 7.33 mA/LSB, RMS ทั้งรอบ |
| VBAT | PC3 (IN13) | VBAT = ADC × 2.038 mV (ต้อง calibrate) |

mapping ยืนยันจาก schematic แล้ว ([open-items H6](../open-items.md)) **สถานะ implement:** `docs/phase3-acquisition.md` (VBAT เป็น rank ปกติของ ADC2 เฉลี่ย 1 s ไม่ใช่ injected; บอร์ดที่ไม่ได้ลง AMC3330 ตั้ง `ACQ_AMC_FITTED 0` ช่อง AC เป็น `NOT_FITTED`)

## Power quality
ส่วนขยาย 2026-11-25: ความถี่ + ล็อก sampling 128×f, THD/harmonic, P/S/Q/PF, พลังงาน, event sag/swell/ไฟดับ และตรวจหน้าสัมผัสรีเลย์ ดู `docs/ac-quality.md` (CT อยู่ที่สายเข้าบอร์ด = กำลังรวมทั้ง 4 ช่อง)

## Input
- คำสั่งจาก Power: เปิด/ปิดการวัด AC ตามสถานะ rail AMC (PC10 เป็นของ Power)

## Output
`Acquisition_GetSnapshot()` คืนค่าต่อสัญญาณ: value, unit, raw, monotonic sample time, age,
validity (`VALID / STALE / OUT_OF_RANGE / DIAG_FAIL / RAIL_OFF`), calibration version
**ห้ามใส่ 0 แทนค่าที่ไม่มี**

## กฎ
- DMA circular แบบ half/full (ครึ่งละ 64 samples ต่อช่อง) task ตื่นทุก 10 ms สะสม sum-of-squares
  คำนวณ RMS ทุก 20 ms (1 รอบ) ใช้ CPU ประมาณ 1–2%
- AMC3330: DIAG low หรือ (P − N) ≈ −2.5 V → ข้อมูลช่องนั้นใช้ไม่ได้
- ระหว่าง rail AMC ปิดและ 1–2 ms หลังเปิด ไม่สนใจ DIAG (จะอ่านได้ low)
- ตรวจว่า DMA ยังเดิน (sequence counter) ถ้าหยุด → snapshot เป็น `STALE`
- โค้ด ADC ใน `debug_protocol.c` (BACKUP/BATV) ต้องเปลี่ยนเป็นอ่านจาก snapshot ห้ามมีคนที่สองเริ่ม ADC DMA

## เวลา
priority ต่ำกว่า Control แต่สูงกว่า Network

## เมื่อเกิด fault
ADC error / DMA error → restart การวัดแบบมีขอบเขต, ตั้ง snapshot เป็น invalid, แจ้ง Supervisor

## การทดสอบ (gate ของ Phase 3)
- ค่าที่วัดได้ตรงกับมิเตอร์บน bench ที่ปลอดภัย
- จำลองช่องที่ไม่มีแรงดันและ DIAG low
- ตรวจว่า DMA หยุดแล้ว snapshot เป็น STALE
