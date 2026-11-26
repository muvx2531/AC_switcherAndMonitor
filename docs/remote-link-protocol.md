# SmartSwitch Remote Link: protocol ระหว่างบอร์ดกับ RPi (port 9001)

> **สถานะ: protocol 1, มีใน firmware ตั้งแต่ 0.9.0 (2026-11-26)** ทดสอบกับบอร์ดจริงด้วยโปรแกรมจำลอง RPi แล้ว (หัวข้อ 13)
> ถ้าส่วนไหนไม่สะดวกฝั่ง RPi ให้แจ้งกลับมาได้ เราปรับได้ (เพิ่ม field ได้โดยไม่เปลี่ยน `proto`)

## 1. ภาพรวม

```
NARIT server  ⇄  (4G)  ⇄  RPi4: โปรแกรมของทีม RPi       ⇄  (LAN ในตู้)  ⇄  SmartSwitch
                           เปิด TCP server ที่ port 9001  ←── บอร์ดเป็นฝ่ายต่อเข้ามา
```

- **บอร์ดเป็น client, RPi เป็น server** บอร์ดหา RPi เองโดย scan ช่วง IP (หัวข้อ 3)
  RPi ไม่ต้องรู้ IP ของบอร์ด
- เชื่อมต่อแล้วเปิดค้างไว้เส้นเดียว ข้อมูลขึ้นและคำสั่งลงใช้เส้นเดียวกัน
- บอร์ดส่ง**สถานะครบทุกค่า**ทุก 1 วินาที และส่ง **event** ทันทีเมื่อเกิดเหตุการณ์
- RPi สั่งรีเลย์ ขอ waveform ขอประวัติ และตั้งเวลาให้บอร์ดได้
- ส่วน RPi → NARIT เป็นของทีม RPi ทั้งหมด บอร์ดไม่รู้จัก NARIT
- หน้าเว็บของบอร์ด (port 80) ยังใช้ได้ตามปกติสำหรับคนที่เข้ามาดูหรือสั่งเอง

## 2. รูปแบบข้อความ

- TCP, ข้อความละหนึ่งบรรทัด: **JSON (UTF-8) จบด้วย `\n`** (แบบ JSON Lines)
- ทุกข้อความมี `"t"` บอกชนิดข้อความ
- ข้อความจาก RPi ยาวได้ไม่เกิน **1024 bytes** ต่อบรรทัด (ถ้าเกิน บอร์ดตอบ `error` แล้วตัดการเชื่อมต่อ)
- ข้อความจากบอร์ดยาวได้ถึงประมาณ 8 KB (ข้อความ `status` ปัจจุบันประมาณ 3.4 KB)
- คำขอจาก RPi มี `"id"` ที่ RPi ตั้งเอง (string 1–32 ตัวอักษร ไม่ซ้ำกัน) คำตอบจากบอร์ดจะมี `id` เดียวกัน
- field ที่ไม่รู้จักให้ข้ามไป ทั้งสองฝั่ง (เพื่อให้เพิ่ม field ในอนาคตได้โดยไม่พัง)
- ค่าที่อ่านไม่ได้หรือไม่น่าเชื่อถือจะเป็น `null` เสมอ ห้ามแปลง `null` เป็น 0

## 3. การหา RPi (scan)

| ค่าตั้ง | ค่าเริ่มต้น | หมายเหตุ |
|---|---|---|
| ช่วง IP | `.10` ถึง `.20` | ใน subnet เดียวกับที่บอร์ดได้จาก DHCP เช่นบอร์ดได้ `192.168.1.57/24` จะ scan `192.168.1.10`–`192.168.1.20` |
| port | `9001` | |
| IP ตายตัว | ไม่ตั้ง | ถ้าตั้ง บอร์ดต่อ IP นั้นอย่างเดียว ไม่ scan |

ค่าทั้งหมดแก้ได้จากหน้าเว็บของบอร์ด **แท็บ Network → Remote link settings** (ต้อง sign in และใส่รหัสผ่านอีกครั้ง)
โดยไม่ต้อง update firmware บันทึกแล้วบอร์ดตัดการเชื่อมต่อเดิม (ถ้ามี) แล้ว scan ใหม่ทันที

**บอร์ดใหม่ยังไม่มี shared key จึงไม่เชื่อมต่อ (สถานะ `NO_KEY`)** จนกว่าจะตั้ง key ที่หน้าเว็บ

ขั้นตอนของบอร์ด
1. ลอง IP ที่เชื่อมต่อสำเร็จครั้งล่าสุดก่อน (ถ้ามี)
2. ไล่ทีละ IP ในช่วง รอ connect ไม่เกิน 500 ms ต่อ IP
3. ต่อได้แล้วทำ handshake (หัวข้อ 4) ถ้าไม่ผ่าน ตัดแล้วไป IP ถัดไป
4. ครบช่วงแล้วไม่เจอ รอ 10 วินาที แล้วเริ่มใหม่
5. หลุดเมื่อไร (รวมถึงตอนบอร์ดรีสตาร์ท) เริ่มข้อ 1 ใหม่

**ข้อกำหนดฝั่ง RPi:** รับการเชื่อมต่อได้ครั้งละหนึ่งบอร์ดต่อตู้ ถ้ามีการเชื่อมต่อใหม่จากบอร์ดเดิม (MAC เดียวกัน)
ที่ผ่าน handshake แล้ว ให้ปิดเส้นเก่าทิ้ง (เส้นเก่าอาจค้างอยู่หลังบอร์ดรีสตาร์ท)

> แนะนำ: ถ้า router จอง IP ให้ RPi ได้ (DHCP static lease) ให้จองไว้ในช่วง .10–.20 จะหาเจอเร็วที่สุด

## 4. Handshake (ยืนยันตัวตนทั้งสองฝ่าย)

ทั้งสองฝ่ายมี **shared key** เดียวกัน (ASCII ที่พิมพ์ได้ 16–64 ตัว ไม่มีช่องว่าง ตั้งที่หน้าเว็บของบอร์ด และในค่าตั้งของโปรแกรม RPi)
หน้าเว็บไม่แสดง key ที่ตั้งไว้ (บอกแค่ว่าตั้งแล้วหรือยัง)
key ไม่ถูกส่งบนสาย ใช้ HMAC-SHA256 พิสูจน์ว่ารู้ key

```
บอร์ด → RPi   {"t":"hello","proto":1,"device":"smartswitch","mac":"02:1D:51:08:57:A2",
               "fw":"0.9.0","nonce":"<32 hex สุ่ม>"}
RPi → บอร์ด   {"t":"auth","nonce":"<32 hex สุ่ม>","proof":"<64 hex>"}
บอร์ด → RPi   {"t":"auth_ok","proof":"<64 hex>","status_interval_s":1}
              หรือ {"t":"auth_fail"} แล้วบอร์ดตัดการเชื่อมต่อ
```

```
proof ของ RPi   = hex( HMAC_SHA256(key, "SSW1-RPI:"   + nonce_board + ":" + nonce_rpi) )
proof ของบอร์ด = hex( HMAC_SHA256(key, "SSW1-BOARD:" + nonce_rpi   + ":" + nonce_board) )
```

- hex ตัวพิมพ์เล็ก, nonce เป็น hex 32 ตัว (16 bytes) จาก random generator
- RPi ต้องตรวจ `proof` ของบอร์ด ถ้าไม่ตรงให้ปิดการเชื่อมต่อ (อาจเป็นอุปกรณ์ที่ปลอมตัว)
- handshake ต้องเสร็จภายใน 5 วินาที นับจากต่อ TCP ได้
- `proto` ไม่ตรงกับที่ RPi รองรับ ให้ RPi ปิดการเชื่อมต่อ
- หลัง handshake ข้อความไม่ได้เข้ารหัส (อยู่ใน LAN ของตู้เท่านั้น) ห้าม port-forward port 9001 ออกอินเทอร์เน็ต

## 5. การรักษาการเชื่อมต่อ

- บอร์ดส่ง `status` ทุก `status_interval_s` วินาที (ค่าเริ่มต้น 1) ถ้า RPi ไม่ได้รับอะไรเลยนาน 5 วินาที ถือว่าหลุด
- RPi ต้องส่งข้อความอะไรก็ได้อย่างน้อยทุก 15 วินาที ถ้าไม่มีอะไรจะส่งให้ส่ง `{"t":"ping","id":"..."}`
  บอร์ดตอบ `{"t":"pong","id":"..."}` ถ้าบอร์ดไม่ได้รับอะไรจาก RPi นาน 30 วินาที บอร์ดตัดแล้ว scan ใหม่
- **การเชื่อมต่อหลุดไม่ทำให้รีเลย์เปลี่ยน** บอร์ดคงสถานะรีเลย์ไว้ตามเดิมเสมอ

**สิ่งที่ RPi ต้องรู้: ตอนไฟ 24 V ดับ บอร์ดตัดไฟวงจร Ethernet เพื่อประหยัดแบต** การเชื่อมต่อจะหลุด
จนกว่าไฟ 24 V กลับมา บอร์ดพยายามส่ง event `SUPPLY` (present = false) ก่อนตัด แต่ไม่รับประกัน
ถ้าหลุดโดยที่ `status` ล่าสุดมี `power.vin_detect = 0` หรือ `power.mode` ไม่ใช่ `MAINS` แปลว่าไฟ 24 V ดับ

## 6. ข้อความจากบอร์ด

### 6.1 `status` (ทุก 1 วินาที)

```json
{"t":"status","seq":1234,"unix":1795600000,"data":{ ...เหมือน GET /api/status ทุกประการ... }}
```

- `seq` นับขึ้นทีละ 1 ต่อการเชื่อมต่อ (เริ่ม 1 ใหม่ทุกครั้งที่ต่อ) ใช้ตรวจว่าข้อความหาย
- `unix` เวลาของบอร์ด (วินาที UTC) หรือ `null` ถ้าบอร์ดยังไม่เคยได้รับเวลาจาก RPi (ดู `time` ในหัวข้อ 7.4)
- `data` คือข้อมูลชุดเดียวกับที่หน้าเว็บใช้ ครบทุกค่า ความหมายของแต่ละ field อยู่ในหัวข้อ 9

### 6.2 `event` (ส่งทันทีเมื่อเกิด)

```json
{"t":"event","seq":5821,"unix":1795600123,"up_s":3605,"type":"SUPPLY","present":false}
```

- `seq` เลขลำดับของ event log บนบอร์ด (เก็บใน flash ไม่ reset ตอนรีสตาร์ท) RPi ใช้ตรวจว่าพลาด event ไหนไป
  แล้วขอย้อนหลังด้วย `get events` ได้
- `unix` อาจเป็น `null` ถ้าตอนเกิด event บอร์ดยังไม่รู้เวลา ใช้ `up_s` (วินาทีนับจากบูต) แทน

| `type` | field เพิ่มเติม | ความหมาย |
|---|---|---|
| `BOOT` | `reset_cause`, `reset_count`, `safe_mode` | บอร์ดบูต (รวมถึง watchdog reset) |
| `RELAY` | `request_id`, `action`, `channels`, `outcome`, `source`, `actor` | คำสั่งรีเลย์ทำเสร็จ (`APPLIED`) หรือล้ม (`FAILED`) `source` = `REMOTE` / `WEB` / `BUTTON` / `SERIAL` / `SYSTEM` `actor` มีเฉพาะคำสั่ง REMOTE ของการเชื่อมต่อนี้ (อื่นๆ เป็น `null`) |
| `REMOTE_CMD` | `request_id`, `actor` | บอร์ดรับคำสั่ง REMOTE (บันทึก `actor` 8 ตัวอักษรแรกไว้ใน flash) |
| `REMOTE_LINK` | `connected`, `peer` | การเชื่อมต่อกับ RPi เริ่ม / จบ (เห็นเมื่อขอย้อนหลังด้วย `get events`) |
| `RULE_C` | `channels` | ไฟ 24 V หายแล้วกลับมา บอร์ดคืนช่องเหล่านี้เป็น ON (ดู 8.3) |
| `SUPPLY` | `present` | ไฟ 24 V มา (`true`) / หาย (`false`) |
| `POWER_MODE` | `from`, `to` | `MAINS` / `BACKUP` / `LOW_POWER` |
| `PQ` | `kind`, `duration_ms`, `extreme_v_x10` | ไฟ AC ตก (`SAG`), เกิน (`SWELL`), ดับ (`INTERRUPTION`) `extreme_v_x10` = แรงดันต่ำสุด/สูงสุด ×10 |
| `CONTACT` | `channel` (1–4), `state` | ผลตรวจหน้าสัมผัสรีเลย์: `OK`, `NO_VOLTAGE`, `STUCK_ON`, `UNKNOWN` |
| `CT_REVERSED` | `set` | CT ใส่กลับด้าน (`true`) / แก้แล้ว (`false`) |
| `CONFIG` | `remote` | ค่าตั้งของบอร์ดเปลี่ยน (`remote: true` = ค่าตั้ง remote link) |
| `FORMAT` | — | storage ถูก format |
| `FIRMWARE` | `state`, `result`, `version` | ขั้นตอนและผลการ update firmware: `result` = `NONE`, `COMMITTED`, `ROLLED_BACK`, `INSTALL_FAILED`, `RESTORED`, `REPAIRED` |

event ชนิดที่บอร์ดรุ่นใหม่กว่าอาจเพิ่มเข้ามาจะมี `type` ที่ไม่อยู่ในตารางนี้ ให้เก็บหรือข้ามไป อย่าถือเป็น error

### 6.3 คำตอบของคำขอ

- `ack`, `result`: ดูหัวข้อ 7.1
- `data`: คำตอบของ `get` (7.2)
- `pong`, `error`: ดู 5 และ 10

## 7. คำขอจาก RPi

### 7.1 `relay` สั่งรีเลย์

```json
{"t":"relay","id":"narit-000123","action":"POWER_CYCLE","channels":[2],"duration_s":10,
 "actor":"operator:somchai","reason":"camera hang"}
```

| field | ค่า | หมายเหตุ |
|---|---|---|
| `action` | `SET_ON`, `SET_OFF`, `POWER_CYCLE`, `CANCEL_CYCLE` | ดู 8.1 |
| `channels` | array ของ 1–4 เช่น `[1,3]` | ห้ามว่าง ห้ามซ้ำ |
| `duration_s` | 1–600 | เฉพาะ `POWER_CYCLE` ไม่ใส่ = 10 วินาที |
| `actor` | string ไม่เกิน 32 ตัว | **บังคับ** ใครหรืออะไรเป็นผู้สั่ง เช่น `operator:<ชื่อ>`, `ai:<ชื่อระบบ>` |
| `reason` | string ไม่เกิน 64 ตัว | ไม่บังคับ แต่ควรใส่ |

บอร์ดตอบสองจังหวะ

```json
{"t":"ack","id":"narit-000123","ok":true,"request_id":88}
{"t":"result","id":"narit-000123","request_id":88,"state":"APPLIED","reason":null}
```

- `ack` มาทันที บอกว่ารับคำสั่งหรือไม่ ถ้า `ok:false` จะมี `"error"` (ดูตารางล่าง) และไม่มี `result` ตามมา
- `result` มาเมื่อทำเสร็จ (ปกติไม่ถึง 1 วินาที) `state`:
  - `APPLIED` สำเร็จ (สำหรับ `POWER_CYCLE` = เริ่มตัดไฟแล้ว ไฟกลับมาเองเมื่อครบเวลา ดู `control.cycle_remaining_s` ใน status)
  - `FAILED` ทำไม่สำเร็จ ดู `reason`
  - `SUPERSEDED` ถูกคำสั่งที่ใหม่กว่ายกเลิก (เช่นสั่ง `SET_ON` ระหว่าง power-cycle)
  - `REJECTED` ถูกปฏิเสธตอนลงมือทำ เช่นไฟ 24 V หายไประหว่างนั้น (`reason` = `NO_COIL_SUPPLY`, `STATE_UNKNOWN`)
  - `UNKNOWN` บอร์ดไม่มีผลของคำสั่งนี้แล้ว ให้ดู `control.effective` ใน status

| `error` ใน `ack` | ความหมาย | RPi ควรทำอะไร |
|---|---|---|
| `LOCAL_CONTROL` | มีคนกดปุ่มอยู่หน้าตู้ (โหมดปุ่ม) **ปุ่มมีอำนาจเหนือคำสั่งจากระยะไกล** | แจ้งผู้สั่ง แล้วลองใหม่ภายหลัง (`control.local_lock` ใน status บอกสถานะนี้) |
| `NO_COIL_SUPPLY` | ไม่มีไฟ 24 V สำหรับ coil รีเลย์ | แจ้งผู้สั่ง |
| `STATE_UNKNOWN` | บอร์ดยังอ่านสถานะรีเลย์ไม่ได้ | ลองใหม่ภายหลัง |
| `RATE_LIMIT` | สั่งถี่เกินกำหนด (ดู 8.2) | รอแล้วค่อยลองใหม่ ห้ามวนลูปส่งซ้ำทันที |
| `INVALID` | field ผิด เช่น channel ไม่อยู่ใน 1–4 หรือไม่มี `actor` | แก้โปรแกรม |
| `BUSY` | คิวคำสั่งเต็มชั่วคราว | ลองใหม่หลัง 1 วินาที |

**ส่งซ้ำได้อย่างปลอดภัย:** ถ้าส่ง `relay` ด้วย `id` เดิมซ้ำ (เช่นไม่ได้ `ack` เพราะเน็ตกระตุก) บอร์ดจะ**ไม่ทำซ้ำ**
แต่ตอบ `ack` / `result` เดิมกลับไป บอร์ดจำ `id` ล่าสุดได้ 32 รายการ ภายในการเชื่อมต่อเดียวกัน
ถ้าการเชื่อมต่อหลุดก่อนได้ `result` ให้ดูผลจาก event `RELAY` (ขอย้อนหลังด้วย `get events`) หรือจาก
`control.effective` ใน status แทนการส่งซ้ำ

### 7.2 `get` ขอข้อมูล

```json
{"t":"get","id":"g1","what":"waveform"}
```

| `what` | พารามิเตอร์ | คำตอบ `{"t":"data","id":...,"what":...,"data":...}` |
|---|---|---|
| `status` | — | เหมือน `status` แต่ส่งทันที |
| `waveform` | — | รูปคลื่นแรงดันและกระแส 1 cycle ล่าสุด (128 จุด) เหมือน `GET /api/waveform` |
| `events` | `n` (1–50, ค่าเริ่มต้น 20), `before_seq` (ไม่บังคับ) | `{"last_seq":..,"events":[..]}` ใหม่สุดก่อน ใช้ `before_seq` = `seq` ของรายการสุดท้ายเพื่อไล่ย้อนหลังทีละหน้า แต่ละรายการรูปแบบเดียวกับ 6.2 (ไม่มี `"t"`) |
| `history` | `offset` (ค่าเริ่มต้น 0), `n` (1–120, ค่าเริ่มต้น 60) | ประวัติรายนาทีใน RAM สูงสุด 24 ชั่วโมง ดู 9.3 |
| `fw` | — | ข้อมูล firmware และผล update ล่าสุด เหมือน `GET /api/fw/status` |

> ประวัติ 91 วันใน flash ยังไม่มีใน v0.1 จะเพิ่มเป็น `what: "history_daily"` ภายหลัง

### 7.3 `ping`

`{"t":"ping","id":"p1"}` → `{"t":"pong","id":"p1"}`

### 7.4 `time` ตั้งเวลาให้บอร์ด

```json
{"t":"time","id":"t1","unix":1795600000}
```

บอร์ดไม่มี NTP และอาจลืมเวลาหลังไฟดับนาน RPi **ควรส่ง `time` ทุกครั้งหลัง handshake** และทุก 1 ชั่วโมง
(ถ้าเวลาของ RPi ถูกต้อง) บอร์ดตอบ `{"t":"ack","id":"t1","ok":true}` แล้วใช้เวลานี้กับ status และ event ถัดไป
เวลาก่อนปี 2024 ถูกปฏิเสธ (`ok:false`, `INVALID`) event ที่เกิดก่อนบอร์ดได้เวลาครั้งแรกจะมี `unix: null`

## 8. พฤติกรรมของรีเลย์ที่ทีม RPi ต้องรู้

### 8.1 ความหมายของคำสั่ง

- **ON / OFF หมายถึงไฟที่โหลด** (อุปกรณ์ปลายทาง) ไม่ใช่สถานะ coil
- `SET_OFF` ตัดไฟค้างไว้จนกว่าจะสั่ง `SET_ON` (คงอยู่แม้บอร์ดรีสตาร์ทหรือ update firmware)
- `POWER_CYCLE` ตัดไฟ `duration_s` วินาทีแล้ว**คืนไฟเอง** ใช้สำหรับ reset อุปกรณ์ที่ค้าง
- `CANCEL_CYCLE` คืนไฟทันทีให้ช่องที่กำลัง power-cycle
- สถานะจริงของแต่ละช่องดูที่ `control.effective` ใน status (`ON` / `OFF` / `UNKNOWN`)

### 8.2 ตัวจำกัดความถี่ (กันโปรแกรมอัตโนมัติสั่งรัว)

| กฎ | ค่าเริ่มต้น |
|---|---|
| แต่ละช่องเปลี่ยนสถานะจากคำสั่ง REMOTE ห่างกันอย่างน้อย | 5 วินาที |
| คำสั่ง `relay` สูงสุดต่อนาที (ทุกช่องรวมกัน) | 10 คำสั่ง |

เกินกำหนดบอร์ดตอบ `RATE_LIMIT` ค่าทั้งสองตั้งได้ที่หน้าเว็บของบอร์ด คำสั่งจากปุ่มหน้าตู้ไม่ถูกจำกัด

### 8.3 กฎที่บอร์ดทำเอง (คำสั่งจากระยะไกลไม่สามารถเปลี่ยนได้)

- **ไฟ 24 V ดับแล้วกลับมา ทุกช่องกลับเป็น ON** รวมถึงช่องที่เคยสั่ง `SET_OFF` ไว้ (event `RULE_C`)
  ถ้าต้องการให้ช่องนั้นยัง OFF อยู่ RPi ต้องสั่ง `SET_OFF` ใหม่หลังไฟกลับ
- **โหมดปุ่มหน้าตู้ชนะเสมอ** ระหว่างที่มีคนใช้ปุ่ม คำสั่งจากระยะไกลถูกปฏิเสธด้วย `LOCAL_CONTROL`
  และกลับมาใช้ได้เมื่อไม่มีการกดปุ่ม 10 วินาที
- คำสั่งจากระยะไกล**ไม่ต้องใส่รหัสผ่าน** (ต่างจากหน้าเว็บ) เพราะการเชื่อมต่อผ่าน handshake แล้ว
- event log บนบอร์ดเก็บ `actor` ได้ 8 ตัวอักษรแรก (event `REMOTE_CMD`) และไม่เก็บ `reason`
  **ฝั่ง RPi หรือ NARIT ต้องเก็บ log ของคำสั่งฉบับเต็มเอง** (ใครสั่ง สั่งอะไร เพราะอะไร ผลเป็นอย่างไร)

## 9. ความหมายของข้อมูลใน `status.data`

ตัวเลขส่วนใหญ่เป็นจำนวนเต็มพร้อมหน่วยในชื่อ field: `_x10` = ค่า ×10, `_x100`, `_x1000` เช่นกัน,
`_mhz` = มิลลิเฮิรตซ์, `_mv` / `mv_` = มิลลิโวลต์, `_dw` = เดซิวัตต์ (×0.1 W), `_mwh` = มิลลิวัตต์-ชั่วโมง

### 9.1 field หลักที่ NARIT น่าจะใช้

| field | หน่วย / ค่า | ความหมาย |
|---|---|---|
| `fw.version`, `fw.uptime_ms` | string, ms | เวอร์ชัน firmware, เวลาตั้งแต่บูต (ลดลง = บอร์ดรีสตาร์ท) |
| `control.effective[4]` | `ON`/`OFF`/`UNKNOWN` | **สถานะไฟของโหลด CH1–CH4** |
| `control.cycle_remaining_s[4]` | s | เวลาที่เหลือของ power-cycle (0 = ไม่ได้ cycle) |
| `control.coil_supply` | bool | มีไฟ 24 V สำหรับสั่งรีเลย์ |
| `control.local_lock` | bool | มีคนใช้โหมดปุ่มหน้าตู้อยู่ (สั่งจากระยะไกลไม่ได้) |
| `control.available`, `control.state_known` | bool | ระบบสั่งรีเลย์พร้อม, รู้สถานะรีเลย์แน่นอน |
| `power.mode` | `MAINS`/`BACKUP`/`LOW_POWER` | ใช้ไฟ 24 V / ใช้แบต / แบตใกล้หมด |
| `power.vin_detect` | 0/1 | มีไฟ 24 V เข้า |
| `vbat.mv_est` | mV | แรงดันแบต (ประมาณ ยังไม่ calibrate) **ข้อจำกัด: บอร์ดรุ่นนี้ตรวจไม่ได้ว่าถอดแบตออกหรือไม่** |
| `acq.ac[4].v_x10` | V ×10 | แรงดัน AC ของแต่ละช่อง (`state` ไม่ใช่ `VALID` → `null`) |
| `acq.ct.ma` | mA | กระแสรวมของตู้ (CT ตัวเดียวที่สายป้อน) |
| `acq.pq.freq_mhz` | mHz | ความถี่ไฟ เช่น 49970 = 49.97 Hz |
| `acq.pq.p_dw`, `s_dva`, `q_dvar` | ×0.1 W / VA / var | กำลังจริง / ปรากฏ / รีแอกทีฟ |
| `acq.pq.pf_x1000` | ×0.001 | power factor |
| `acq.pq.energy_mwh` | mWh | พลังงานสะสม (เก็บใน flash ไม่หายตอนรีสตาร์ท) |
| `acq.pq.v_thd_x10`, `i_thd_x10` | % ×10 | THD แรงดัน / กระแส |
| `acq.pq.v_h_x10[24]`, `i_h_x10[24]` | % ×10 | ฮาร์มอนิก h2–h25 |
| `acq.pq.events` | count และ `last` | จำนวนครั้งไฟตก/เกิน/ดับ และเหตุการณ์ล่าสุด |
| `acq.pq.contact[4]` | `OK`/`NO_VOLTAGE`/`STUCK_ON`/`UNKNOWN` | ผลตรวจหน้าสัมผัสรีเลย์ (รีเลย์ติดค้าง) |
| `acq.pq.ct_reversed` | bool | CT ใส่กลับด้าน |
| `sys.reset_cause`, `sys.reset_count` | string, count | สาเหตุการรีสตาร์ทล่าสุด |
| `sys.safe_mode` | bool | บอร์ดรีสตาร์ทซ้ำบ่อยจนเข้า safe mode ต้องมีคนตรวจ |
| `storage.state` | `READY` / อื่นๆ | flash เก็บข้อมูลทำงานปกติ |
| `time.valid`, `time.unix` | bool, s | บอร์ดได้เวลาจาก RPi แล้วหรือยัง, เวลาปัจจุบัน |
| `remote.*` | | สถานะ remote link ที่บอร์ดเห็น (`state`, `peer`, `connected_s`, `commands`, `rejected`, ค่าตั้ง) ไม่มี key |

### 9.2 field สำหรับวินิจฉัย

`net.*` (IP, MAC, link), `relay.*` (register ของชิปรีเลย์), `power.efuse_ilim/chrg/fault/eth_rail/amc_rail/...`,
`acq.running/restarts/missed/cycles/cal`, `acq.ac[].rms_raw`, `amc_diag`, `flash[]`, `storage.*`,
`sys.heap_*`, `sys.watchdog.*`, `sys.log.*`, `sys.tasks[]`, `sys.i2c_recoveries`
ส่งต่อขึ้น NARIT ได้ทั้งหมดเพื่อให้ทีม SmartSwitch ใช้วิเคราะห์ปัญหาจากระยะไกล
รายละเอียดอยู่ใน `docs/web-status-api.md`, `docs/ac-quality.md`, `docs/phase4-power.md`

### 9.3 รูปแบบ `history`

```json
{"period_s":60,"total":1440,"offset":0,"up_now_s":86410,"unix_now":1795600000,
 "fields":["end_up_s","v_avg_x10","v_min_x10","v_max_x10","i_ma","p_dw","freq_mhz","pf_x1000","v_thd_x10","i_thd_x10"],
 "rows":[[60,2297,2281,2305,420,1032,49987,800,21,384], ...]}
```

- แต่ละแถวคือหนึ่งนาที เก่าสุดก่อน (`offset` 0 = เก่าสุด) ค่าที่ไม่มีข้อมูลเป็น `null`
- `end_up_s` = uptime ของบอร์ด (วินาที) ตอนจบนาทีนั้น แปลงเป็นเวลาจริงได้จาก `unix_now - (up_now_s - end_up_s)`
- ขอทีละไม่เกิน 120 แถว ไล่ `offset` จนครบ `total`
- **ประวัตินี้อยู่ใน RAM และเริ่มนับใหม่ทุกครั้งที่บอร์ดรีสตาร์ท** (ประวัติ 91 วันใน flash จะเพิ่มภายหลัง)
  แนะนำให้ RPi เก็บค่าจาก `status` เองเป็นหลัก แล้วใช้ `history` เติมช่วงที่การเชื่อมต่อหลุด

## 10. ข้อผิดพลาดทั่วไป

```json
{"t":"error","id":"x1","error":"UNKNOWN_TYPE"}
```

| `error` | ความหมาย |
|---|---|
| `BAD_JSON` | อ่าน JSON ไม่ได้ |
| `TOO_LONG` | บรรทัดยาวเกิน 1024 bytes (บอร์ดตัดการเชื่อมต่อหลังส่ง) |
| `UNKNOWN_TYPE` | ไม่รู้จัก `t` นี้ |
| `MISSING_ID` | คำขอไม่มี `id` |
| `NOT_AUTHENTICATED` | ส่งคำขอก่อน handshake เสร็จ (บอร์ดตัดการเชื่อมต่อ) |

## 11. ตัวอย่างฝั่ง RPi (Python, สำหรับทดสอบ)

```python
import asyncio, hashlib, hmac, json, secrets, time

KEY = b"change-me-shared-key"          # ต้องตรงกับค่าที่ตั้งบนบอร์ด

def proof(label, a, b):
    return hmac.new(KEY, f"{label}:{a}:{b}".encode(), hashlib.sha256).hexdigest()

async def handle(reader, writer):
    send = lambda m: writer.write((json.dumps(m) + "\n").encode())
    hello = json.loads(await asyncio.wait_for(reader.readline(), 5))
    nb, nr = hello["nonce"], secrets.token_hex(16)
    send({"t": "auth", "nonce": nr, "proof": proof("SSW1-RPI", nb, nr)})
    ok = json.loads(await asyncio.wait_for(reader.readline(), 5))
    if ok.get("t") != "auth_ok" or not hmac.compare_digest(ok["proof"], proof("SSW1-BOARD", nr, nb)):
        writer.close(); return
    print("connected:", hello["mac"], hello["fw"])
    send({"t": "time", "id": "t0", "unix": int(time.time())})
    last_ping = time.monotonic()
    while True:
        line = await asyncio.wait_for(reader.readline(), 5)   # status ทุก 1 s
        if not line: break
        m = json.loads(line)
        if m["t"] == "status":
            d = m["data"]
            print(m["seq"], d["control"]["effective"], d["power"]["mode"])
        else:
            print(m)                                           # event, ack, result, data
        if time.monotonic() - last_ping > 10:
            send({"t": "ping", "id": f"p{int(time.time())}"}); last_ping = time.monotonic()
        await writer.drain()

async def main():
    server = await asyncio.start_server(handle, "0.0.0.0", 9001)
    async with server: await server.serve_forever()

asyncio.run(main())
```

สั่งรีเลย์ (ส่งหลัง handshake):

```python
send({"t": "relay", "id": "narit-000123", "action": "POWER_CYCLE", "channels": [2],
      "duration_s": 10, "actor": "operator:somchai", "reason": "camera hang"})
```

## 12. สิ่งที่ยังเปิดอยู่ (รอความเห็นทีม RPi)

0. ประวัติ 91 วันจาก flash (`history_daily`) ยังไม่มี

1. status ทุก 1 วินาที (~6 KB) เพียงพอหรือมากเกินไป (ตั้งได้ 1–60 วินาที)
2. ต้องการให้ RPi ตั้งค่าบนบอร์ดจากระยะไกลด้วยหรือไม่ เช่นช่วง IP, rate limit (v0.1 ตั้งได้จากหน้าเว็บเท่านั้น)
3. ต้องการ update firmware ของบอร์ดผ่าน RPi หรือไม่ (v0.1 ทำผ่านหน้าเว็บเท่านั้น)
4. รูปแบบ `actor` ที่ NARIT ใช้ระบุผู้สั่ง

## 13. ผลทดสอบ (firmware 0.9.0, บอร์ดที่ 2, 2026-11-26)

ใช้โปรแกรมจำลอง RPi (Python, แบบเดียวกับหัวข้อ 11) บน PC ที่ 192.168.0.222 บอร์ดอยู่ที่ 192.168.0.223
ตั้งช่วง scan .215–.225 (บอร์ดข้าม IP ของตัวเอง)

- scan เจอ RPi และ handshake ผ่านทั้งสองทาง, key ผิด → `auth_fail` แล้วบอร์ดตัด, RPi เงียบ → บอร์ดตัดที่ 30.0 วินาที
- `status` ทุก 1 วินาที (3.4 KB) `seq` ต่อเนื่อง, `time` ตั้งนาฬิกาแล้ว `unix` ตรงกับ RPi
- `get` status / waveform / fw / events (รวม `before_seq`) / history ตอบครบ, `ping` → `pong`
- ข้อผิดพลาด: channel 5 / ไม่มี `actor` → `INVALID`, ไม่มี `id` → `MISSING_ID`, JSON เสีย → `BAD_JSON`,
  `t` ไม่รู้จัก → `UNKNOWN_TYPE`, บรรทัดยาวเกิน 1024 → `TOO_LONG` แล้วบอร์ดตัดและต่อใหม่เอง
- `SET_ON` (ทดสอบเฉพาะทิศที่ไม่จ่ายไฟเข้า coil) → `ack` + `result APPLIED`, event `REMOTE_CMD` และ `RELAY` (source `REMOTE`, actor ครบ),
  ส่ง `id` ซ้ำ → ได้ `ack`/`result` เดิมโดยไม่ทำซ้ำ, คำสั่งที่สองภายใน 5 วินาที → `RATE_LIMIT`, หลัง 5 วินาที → รับ
- เชื่อมต่อต่อเนื่อง 106 วินาทีพร้อมหน้าเว็บ poll ทุกวินาที: เว็บตอบ 196/196, heap ต่ำสุด 11,376 B, stack task `remote` เหลือ 340 words

ยังไม่ได้ทดสอบ: `SET_OFF` / `POWER_CYCLE` ผ่าน remote (ต้องให้เจ้าของโปรเจคยืนยันว่าปลอดภัย), คำสั่งระหว่างโหมดปุ่ม (`LOCAL_CONTROL`),
ไฟ 24 V ดับระหว่างเชื่อมต่อ, RPi จริงหลัง router ในตู้
