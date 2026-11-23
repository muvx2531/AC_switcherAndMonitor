# Control task

## หน้าที่
เป็นเจ้าของสถานะรีเลย์ทั้ง 4 ช่อง และเป็น**คนเดียว**ที่เขียน PCA9536 ได้ ทำตาม invariants
I1–I5 ใน [README](../README.md) รับคำสั่งจากทุก adapter แล้วรายงานสถานะจริง

## Owns
| Resource | หมายเหตุ |
|---|---|
| PCA9536 (I2C1 `0x41`) output/config register | ผ่าน driver [i2c-bus](../drivers/i2c-bus.md) + `pca9536.c` IO0..3 = CH1..4, LOW = coil OFF = โหลด ON |
| Relay state machine, pending power-cycle timers | — |
| ring ของ outcome (8–16 รายการ) | — |
| การตอบสนองต่อ EXTI3 (PB3) | ISR ส่ง notification มาที่ Control โดยตรง ส่วนสถานะ `coil_supply` ระยะยาวเป็นของ Power |
| BKP slot `pending_restore` | ดู [bkp-rtc](../drivers/bkp-rtc.md) |

## Input
| จาก | ช่องทาง | เนื้อหา |
|---|---|---|
| Network, Diagnostics, Local UI | คิวคำสั่ง (`Control_Submit`) | `ControlRequest` |
| EXTI3 ISR | task notification bit `SUPPLY_EDGE` | ปลุกให้ตรวจ PB3 |
| Power | task notification bit `SUPPLY_STATE` + snapshot | `coil_supply` ที่ยืนยันแล้ว, power mode |
| Acquisition | `Acquisition_GetSnapshot()` (Control ดึงเอง) | แรงดัน RMS ต่อช่อง, DIAG, validity |

## Output
- `Control_GetSnapshot()` และ `Control_GetOutcome(request_id)`
- event ไปที่ Storage (เปลี่ยนสถานะ, ปฏิเสธคำสั่ง, กฎ (ค) ทำงาน, glitch, fault)
- heartbeat ไปที่ Supervisor

## API
```c
typedef enum { CTRL_SET_ON, CTRL_SET_OFF, CTRL_POWER_CYCLE, CTRL_CANCEL_CYCLE } CtrlType;
typedef enum { SRC_WEB, SRC_SERIAL, SRC_LOCAL, SRC_SYSTEM } CtrlSource;

typedef struct {
    CtrlType   type;
    uint8_t    channel_mask;   /* bit0..3 = CH1..4 */
    uint16_t   duration_s;     /* POWER_CYCLE: 1..600, 0 = default 10 s */
    CtrlSource source;
} ControlRequest;

CtrlSubmitResult Control_Submit(const ControlRequest *req, uint32_t *out_request_id);
bool             Control_GetOutcome(uint32_t request_id, CtrlOutcome *out);
void             Control_GetSnapshot(ControlSnapshot *out);
```
- `Control_Submit` คืน `ACCEPTED`, `BUSY` (คิวเต็ม) หรือ `REJECTED` + เหตุผล:
  `NO_COIL_SUPPLY` (SET_OFF/POWER_CYCLE ตอนไม่มี 24 V), `STATE_UNKNOWN` (I2C fault ตาม I3),
  `INVALID` (mask = 0, duration นอกช่วง)
- `request_id` ออกโดย Control (นับขึ้น) การกันคำสั่งซ้ำจาก browser เป็นหน้าที่ของ Network
- outcome: `PENDING → APPLIED | FAILED | SUPERSEDED | UNKNOWN`
- "รับคำสั่งแล้ว" ไม่ได้แปลว่ารีเลย์ทำงานแล้ว

### Snapshot ต่อช่อง
| Field | ความหมาย |
|---|---|
| `latch` | ค่าที่อ่านกลับจาก PCA |
| `effective` | `ON / OFF / UNKNOWN` จาก latch + `coil_supply` |
| `contact` | `CONFIRMED / INFERRED / UNKNOWN` + flag `STUCK`, `NC_FAIL` |
| `cycle_remaining_s` | 0 = ไม่มี power-cycle ค้าง |
| `last_source`, `last_change` | ใครสั่งล่าสุด เมื่อไร |

ส่วนรวม: `coil_supply`, สุขภาพ I2C, `glitch_count`, ผลจากการตรวจตอน boot

## กฎและ state

### คำสั่ง (S5-1 … S5-5)
- `SET_OFF`: latch HIGH ค้าง
- `SET_ON`: latch LOW
- `POWER_CYCLE`: latch HIGH → รอ duration → latch LOW เขียน `pending_restore` ก่อนเขียน latch
- `CANCEL_CYCLE`: latch LOW ทันที ล้าง pending
- **Supersede (S5-3)** คำสั่งใหม่บนช่องที่กำลัง cycle: `SET_ON` จบ cycle ทันที, `SET_OFF` เปลี่ยนเป็น OFF ค้าง
  (ไม่ต้องเขียน latch, ล้าง pending), `POWER_CYCLE` เริ่มนับใหม่โดยไม่ให้โหลดติดระหว่างกลาง
  คำสั่งเดิมเป็น `SUPERSEDED`
- ทุกการเขียน PCA เป็น masked read-modify-write แล้วอ่านกลับ ถ้าอ่านกลับไม่ตรงหรือ error → `UNKNOWN`
- ตอนนี้ใช้ "คำสั่งล่าสุดชนะ" กับทุก source ส่วนกฎ local/remote ดู [open-items D1](../open-items.md)

### `pending_restore` (S1-3)
ลำดับเขียน: ลบ magic → เขียน `{channel_mask, duration_s, request_id}` + CRC → เขียน magic
ลำดับล้าง: ลบ magic ก่อน

ตอน boot หลังอ่าน PCA (I1):
| สภาพที่พบ | ทำอะไร |
|---|---|
| ไม่มี magic หรือ CRC ผิด | ไม่มี pending |
| PCA ขาเป็น input (เพิ่งได้ไฟใหม่) | ล้าง pending, event `PENDING_DROPPED_PCA_POWERON` (I4) |
| PCA เป็น output และ latch ของช่องนั้น HIGH | ทำ restore ต่อ **หลังจาก boot ไปครบ duration** (ไม่พึ่ง RTC) |
| PCA เป็น output แต่ latch LOW แล้ว | ล้าง pending, บันทึก event |

### กฎ (ค): 24 V หาย (S2-2, S4-2)
```
EXTI3 ISR: ปิด EXTI3 ชั่วคราว → notify Control (SUPPLY_EDGE) และ Power
Control:
  อ่าน PB3 ทุก 0.5 ms นาน T_CONFIRM_LOSS (เริ่ม 2–3 ms, ค่าจริงจาก open-items H4)
    กลับมามีไฟระหว่างนั้น → glitch_count++, ไม่ทำอะไร
    ไม่มีไฟตลอดช่วง     → เขียน latch LOW ทุกช่อง, ล้าง pending_restore,
                           event RELAY_RELEASED_BY_SUPPLY_LOSS (ระบุช่องที่เคย HIGH)
  เปิด EXTI3 อีกครั้ง
Polling สำรอง: อ่าน PB3 ทุก 20–50 ms และตอน boot ถ้าพบว่าไม่มีไฟ → เข้าขั้นตอนยืนยันเดียวกัน
I2C เขียนไม่สำเร็จ → retry จนสำเร็จ (PCA ยังมีไฟจากแบต) สถานะเป็น UNKNOWN จนกว่าจะอ่านกลับได้
```
- เป้า latency ≤ 10 ms จาก PB3 เปลี่ยนจนเขียน latch เสร็จ (เลวร้ายสุด: รอ OLED chunk ≤ 3 ms + I2C ~0.1 ms)
- กฎนี้มีความสำคัญเหนือคิวคำสั่งปกติ ใช้ notification bit ไม่ผ่านคิว
- ขา 24 V กลับ (ยืนยัน 100–500 ms) ไม่ต้องเขียน PCA เพราะ latch เป็น LOW อยู่แล้ว แค่กลับมารับ SET_OFF/POWER_CYCLE ได้
- known limitation: ไฟดับสั้นกว่าเวลาที่เขียนทัน โหลดจะดับต่อตามพฤติกรรมเดิม

### Contact feedback (S2-4, S5-4)
| coil (latch และไฟ) | VAClineX | ผล |
|---|---|---|
| ไม่มีไฟ | มี | ปกติ ON (`CONFIRMED`) |
| ไม่มีไฟ | ไม่มี (ทุกช่องที่ coil ไม่มีไฟ) | AC ขาเข้าหาย (`INFERRED`) |
| ไม่มีไฟ | ไม่มี (ช่องเดียว) | `NC_FAIL` หรือ AMC เสีย (ดู DIAG) |
| มีไฟ | ไม่มี | ปกติ OFF (`CONFIRMED`) |
| มีไฟ | มี | `STUCK` |
| coil มีไฟทุกช่อง | — | ไม่รู้ว่า AC ขาเข้ามีหรือไม่ |

- หลังสั่งรีเลย์ รอ verification window **200 ms** ก่อนตัดสินว่าไม่ตรง ระหว่างนั้น `contact = UNKNOWN`
- ข้อมูลจาก Acquisition ที่ stale/invalid หรือ DIAG low → `UNKNOWN` ห้ามสรุป

## เวลา
- priority สูงสุดในกลุ่ม application
- block บนคิว + notification ด้วย timeout เพื่อทำ polling PB3 และ timer ของ power-cycle
- ห้ามรอ flash หรือ network

## เมื่อเกิด fault
- I2C error/timeout → ขอ recovery จาก driver แล้ว**อ่านกลับเท่านั้น** (I3) สถานะเป็น `UNKNOWN` จนกว่าจะอ่านได้
- recovery ไม่สำเร็จ N ครั้ง → แจ้ง Supervisor แต่ยังพยายามต่อเป็นระยะ
- Network/logger เสีย ห้ามทำให้รีเลย์เปลี่ยน

## การทดสอบ (gate ของ Phase 2)
- unit test บน PC สำหรับ logic ล้วน (ถ้าตกลงตาม [open-items D5](../open-items.md)): supersede, pending 3 กรณี, กฎ (ค), ปฏิเสธคำสั่ง
- วัด latency กฎ (ค) ด้วย scope จาก PB3 ถึงขา IO ของ PCA ≤ 10 ms
- reset ระหว่าง power-cycle ทั้ง 3 กรณี, PCA ได้ไฟใหม่ → โหลดติดทุกช่อง และไม่มีการคืน OFF
- ตัด 24 V ระหว่าง power-cycle และระหว่าง OFF ค้าง → ไฟกลับแล้วโหลดติด
- I2C fault injection (ช็อต SDA) → ไม่มีการเขียน PCA จนกว่าจะอ่านกลับได้
- ทดสอบกับโหลดจริงเฉพาะตามขั้นตอน bench ที่ตกลงกันแล้วเท่านั้น

## ค้างอยู่
open-items H1, H2, H4, H5, D1, D2
