# Network task

## หน้าที่
ดูแล Ethernet และ CycloneTCP, ขอ IP ด้วย DHCP และให้บริการ web UI (HTTP + login)
เฉพาะใน LAN ของตู้ (S7-4) เป็น adapter ที่ส่งคำสั่งเข้า Control และอ่าน snapshot

## Owns
| Resource | หมายเหตุ |
|---|---|
| ETH MAC + IRQ, PHY LAN8742A (MDIO addr 0, REF_CLK จาก PHY) | HAL probe → ส่งต่อให้ CycloneTCP แบบลำดับ ห้าม init พร้อมกัน, `MX_ETH_Init` ยัง bypass |
| CycloneTCP stack และ task ภายใน | GPLv2 ใช้ได้เพราะเปิด source (S7-1) |
| MAC address | สร้างจาก UID 96-bit ของ STM32 ตั้งบิต locally administered (S7-2) |
| DHCP state, IP, static fallback | — |
| HTTP server, session/login | — |

rail +3.3V_ETH (PE0) เป็นของ Power Network ขอเปิด/ได้รับแจ้งก่อนปิด

## การเข้าถึง (S7-4, S7-5)
- web เปิดเฉพาะใน LAN ส่วนเข้าจากระยะไกลผ่าน VPN ของ RUTM51 (แนะนำ Tailscale subnet router)
- **ห้าม port forward มาที่บอร์ด** (เขียนในคู่มือติดตั้ง)
- router ตั้ง DHCP static lease จาก MAC ของบอร์ด
- DHCP ล้มเหลว → retry ต่อ, OLED แสดง `NO IP`, มี static IP สำรองตั้งผ่าน serial
- OLED แสดง IP และ MAC

## Security ขั้นต่ำของ v1
- ทุก endpoint ต้อง login ส่วน endpoint ที่สั่งรีเลย์มี rate limit และกัน CSRF
- รหัสผ่านเก็บเป็น hash ใน config (Storage) รหัส default ต้องไม่ซ้ำกันต่อบอร์ด หรือบังคับเปลี่ยนตอนเข้าครั้งแรก
- กันคำสั่งซ้ำจาก browser (client token) ก่อนเรียก `Control_Submit`
- browser หลุดไม่ได้แปลว่าคำสั่งล้มเหลว ให้แสดง outcome จาก `Control_GetOutcome`
- ไม่มี TLS บน MCU ใน v1 (VPN เข้ารหัสให้)
- TCP echo test (port 5000) และ IP/MAC ชั่วคราวต้องเอาออกก่อนใช้งานจริง

## Power (S6-4)
- BACKUP → Power ปิด ETH: หยุด interface → ตั้งขา RMII เป็น analog → PE0 LOW
- 24 V กลับ → PE0 HIGH → รอ PHY → reset PHY ผ่าน MDIO (BCR bit 15) → init MAC ใหม่ → DHCP
- ทุกขั้นมี timeout CycloneTCP driver มีจุดรอแบบไม่จำกัดเวลาที่ต้องแก้ก่อน

## เวลา
priority ต่ำกว่า Supervisor ไม่อยู่ในเส้นทาง boot ห้ามถือ socket/lock ระหว่างรอแบบไม่จำกัดเวลา

## v2 (S7-6)
remote ผ่าน router (Modbus TCP server หรือ MQTT ไปที่ broker บน router) เพิ่มเป็น adapter ใหม่
โดยไม่ต้องแก้ Control

## การทดสอบ (gate ของ Phase 6)
- ใช้งานผ่าน Tailscale ได้, login, ทดสอบ CSRF และ rate limit
- link หลุด/กลับ, DHCP หาย, ตัดไฟ PHY ซ้ำหลายรอบ
- หลาย client พร้อมกัน, ใช้งานต่อเนื่องนานโดยไม่รั่ว heap
