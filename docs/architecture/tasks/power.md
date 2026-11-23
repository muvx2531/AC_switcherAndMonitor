# Power task

## หน้าที่
รู้สถานะไฟเข้าและแบต คุม rail enable ทั้งหมดและกฎกันไฟย้อน ตัดสินโหมด
MAINS / BACKUP / LOW_POWER แล้วแจ้งข้อเท็จจริงให้ Control (S2-1) Power **ไม่สั่งรีเลย์**

## Owns
| Resource | หมายเหตุ |
|---|---|
| PB3 Vin_Detect (สถานะยืนยันระยะยาว) | หลัง F101 (S2-3) polarity รอยืนยัน (open-items H2) Control ตอบสนองขาลงเองตามกฎ (ค) |
| PB9 eFuse_iLim | low = ไฟเข้าบอร์ดหาย/UVLO/OVP/ILIM ใช้แยกสาเหตุ |
| PE13 Batt_status (CHRG), PE11 Batt_FAULT | LT3652 active-low |
| PE0 En_3v3_ETH, PE2 En_3v3_AMC | service อื่น "ขอ" rail ห้ามสั่งขาเอง |
| PE1 En_3v3_Per | LOW ตลอดใน v1 (RS485 ไม่ใช้) |
| PD1 EN_LTE_POW | LOW ตลอดใน v1 (EC25 ไม่ใช้) |
| ขาที่ต้องตั้งก่อนปิด rail | RMII (PA1/2/7, PB11–13, PC1/4/5), PD5, PA9, PD15 |
| clock ที่ลดลงใน LOW_POWER | — |

## State (S6-3, S6-4)
```
MAINS ──24V หาย (ยืนยัน)──▶ BACKUP ──VBAT < เกณฑ์ หรือ อยู่บนแบต > X ชม.──▶ LOW_POWER
  ▲                            │                                            │
  └──── 24V กลับ (ยืนยัน 100–500 ms) ──┴────────────────────────────────────────┘
```
| State | ทำอะไร |
|---|---|
| MAINS | rail ETH และ AMC เปิด ทำงานปกติ |
| BACKUP | บันทึก event, **ปิด ETH เสมอ** (ไม่มี config), ปิด AMC, ขอ Storage flush log |
| LOW_POWER | ลด clock, รันแค่ WDT feed, ตรวจ PB3 และวัด VBAT เป็นระยะ จนกว่า BMS จะตัด |
| กลับ MAINS | เปิด AMC และ ETH, แจ้ง Network ให้ init ใหม่ |

- เกณฑ์ v1 (S1-5): VBAT < ~6.2 V หรือบนแบต > X ชม. ทั้งคู่เป็น config
- ไม่ใช้ Stop mode ใน v1 (เลื่อนไป v2)
- ตอนอยู่บนแบต coil ไม่มีไฟ รีเลย์จึงไม่ได้รับผลจากโหมดนี้

## กฎกันไฟย้อน (จาก HW doc)
| Rail | ก่อนปิด |
|---|---|
| +3.3V_ETH (PE0) | ตั้งขา RMII เป็น analog/low และหยุด CycloneTCP interface ก่อน |
| +3.3V_PER (PE1) | PD5 (U2_TX) เป็น low/input |
| +3.3V_AMC (PE2) | Acquisition ต้องไม่สนใจ DIAG ระหว่างปิดและ 1–2 ms หลังเปิด |
| +3V3_LTE (PD1) | PA9 / PD15 เป็น low/input (ใน v1 ปิดตลอด) |

## Input
- notification จาก EXTI3 (พร้อมกับ Control), poll PB3/PB9 ของตัวเอง
- snapshot VBAT จาก Acquisition

## Output
- notification bit `SUPPLY_STATE` + snapshot (`coil_supply`, mode) ให้ Control
- คำสั่ง rail/ready ให้ Network และ Acquisition
- event ไปที่ Storage, heartbeat ไปที่ Supervisor

## การทดสอบ (gate ของ Phase 4)
- ตัด/จ่าย 24 V ซ้ำหลายรอบ: ETH กลับมาทุกครั้ง, heap ไม่รั่ว
- ตรวจว่าไม่มีไฟย้อนเข้า PHY ตอน rail ปิด
- เข้า LOW_POWER ด้วยค่าเกณฑ์ที่ตั้งต่ำลงชั่วคราว แล้ว WDT ยังได้รับ toggle
