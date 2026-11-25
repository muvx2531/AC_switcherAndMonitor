/* SmartSwitch bootloader (docs/phase5b-bootloader.md).
 *
 * Rules: never touch I2C / PCA9536 (relay state, I1-I4), never touch the RTC or the
 * backup domain, never clear the RCC reset flags (the application reads them), keep the
 * STWD100 fed (PE7, timeout 1.12 s) including during internal flash sector erases
 * (code in RAM, interrupts off, toggling by cycle count).
 *
 * External flash chip 2: staging slot, backup slot, boot record A/B (ssw_image.h).
 * There is always a complete copy of a working image outside the internal flash before
 * the internal flash is erased, so a power cut at any point is recovered at the next boot.
 */
#include "bl_main.h"
#include "main.h"
#include "ssw_image.h"
#include "ssw_sha256.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern SPI_HandleTypeDef hspi2;
extern UART_HandleTypeDef huart6;

/* ---- watchdog ------------------------------------------------------------- */
static uint32_t lastKick;

void Bl_EarlyWdtKick(void)
{
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
  (void)RCC->AHB1ENR;
  GPIOE->MODER = (GPIOE->MODER & ~(3U << (7U * 2U))) | (1U << (7U * 2U));
  GPIOE->ODR ^= (1U << 7);
}

static void kick(void)
{
  uint32_t now = HAL_GetTick();
  if (now - lastKick >= 100U) { lastKick = now; HAL_GPIO_TogglePin(Clr_wdt_GPIO_Port, Clr_wdt_Pin); }
}

/* ---- log (USART6, polling) ------------------------------------------------ */
static void logs(const char *s)
{
  (void)HAL_UART_Transmit(&huart6, (const uint8_t *)s, (uint16_t)strlen(s), 100);
}

static void logx(uint32_t v)
{
  char b[11] = "0x00000000";
  for (int i = 0; i < 8; i++) { uint32_t d = (v >> (28 - 4 * i)) & 15U; b[2 + i] = (char)(d < 10U ? '0' + d : 'A' + d - 10U); }
  logs(b);
}

static void logu(uint32_t v)
{
  char b[11]; int i = 10; b[i] = 0;
  do { b[--i] = (char)('0' + v % 10U); v /= 10U; } while (v && i);
  logs(&b[i]);
}

static void logbuild(const char *b)
{
  char t[33];
  memcpy(t, b, 32); t[32] = 0;
  for (int i = 0; i < 32; i++) if (t[i] && (t[i] < 32 || t[i] > 126)) t[i] = '?';
  logs(t);
}

static void logver(uint32_t v)
{
  logu(v >> 16); logs("."); logu((v >> 8) & 255U); logs("."); logu(v & 255U);
}

/* ---- external flash (chip 2, polling) ------------------------------------- */
static void csLow(void)  { HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_RESET); }
static void csHigh(void) { HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_SET); }

static bool spiTx(const uint8_t *d, uint16_t n) { return HAL_SPI_Transmit(&hspi2, (uint8_t *)d, n, 100) == HAL_OK; }
static bool spiRx(uint8_t *d, uint16_t n) { return HAL_SPI_Receive(&hspi2, d, n, 100) == HAL_OK; }

static bool xfWaitReady(uint32_t timeoutMs)
{
  uint32_t start = HAL_GetTick();
  uint8_t cmd = 0x05, st = 1;
  do {
    csLow(); bool ok = spiTx(&cmd, 1) && spiRx(&st, 1); csHigh();
    if (!ok) return false;
    if ((st & 1U) == 0U) return true;
    kick();
  } while (HAL_GetTick() - start < timeoutMs);
  return false;
}

static bool xfRead(uint32_t addr, void *dst, uint32_t len)
{
  uint8_t cmd[4] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
  uint8_t *p = dst;
  csLow();
  bool ok = spiTx(cmd, 4);
  while (ok && len) { uint16_t n = len > 1024U ? 1024U : (uint16_t)len; ok = spiRx(p, n); p += n; len -= n; kick(); }
  csHigh();
  return ok;
}

static bool xfWriteEnable(void)
{
  uint8_t cmd = 0x06;
  csLow(); bool ok = spiTx(&cmd, 1); csHigh();
  return ok;
}

static bool xfErase4k(uint32_t addr)
{
  uint8_t cmd[4] = { 0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
  if (!xfWriteEnable()) return false;
  csLow(); bool ok = spiTx(cmd, 4); csHigh();
  return ok && xfWaitReady(1000);
}

static bool xfProgram(uint32_t addr, const uint8_t *src, uint32_t len)   /* within one 256-B page */
{
  uint8_t cmd[4] = { 0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
  if (!xfWriteEnable()) return false;
  csLow(); bool ok = spiTx(cmd, 4) && spiTx(src, (uint16_t)len); csHigh();
  return ok && xfWaitReady(20);
}

/* ---- internal flash ------------------------------------------------------- */
/* Sector erase runs from RAM with interrupts off: the CPU cannot fetch from flash while
 * the erase (up to ~2 s for 128 KiB) is running, and the watchdog still needs toggling. */
__attribute__((section(".RamFunc"), noinline))
static uint32_t ramEraseSector(uint32_t sector)
{
  while (FLASH->SR & FLASH_SR_BSY) { }
  FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
  FLASH->CR = (FLASH->CR & ~(FLASH_CR_PSIZE | FLASH_CR_SNB)) | FLASH_CR_PSIZE_1 | FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
  FLASH->CR |= FLASH_CR_STRT;
  uint32_t last = DWT->CYCCNT;
  while (FLASH->SR & FLASH_SR_BSY) {
    if (DWT->CYCCNT - last > 168000000U / 10U) {          /* every ~100 ms */
      last = DWT->CYCCNT;
      GPIOE->ODR ^= (1U << 7);
    }
  }
  FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
  return FLASH->SR & (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR);
}

static uint32_t sectorOf(uint32_t addr)
{
  if (addr < 0x08010000UL) return (addr - 0x08000000UL) / 0x4000UL;         /* 0-3: 16 KiB */
  if (addr < 0x08020000UL) return 4U;                                        /* 4: 64 KiB */
  return 5U + (addr - 0x08020000UL) / 0x20000UL;                             /* 5-11: 128 KiB */
}

static bool eraseAppRange(uint32_t size)
{
  uint32_t first = sectorOf(SSW_APP_ADDR), last = sectorOf(SSW_APP_ADDR + size - 1U);
  HAL_FLASH_Unlock();
  bool ok = true;
  for (uint32_t s = first; ok && s <= last; s++) {
    __disable_irq();
    uint32_t err = ramEraseSector(s);
    __enable_irq();
    lastKick = HAL_GetTick() - 100U; kick();
    if (err) { logs("BL: erase error sector "); logu(s); logs("\r\n"); ok = false; }
  }
  HAL_FLASH_Lock();
  return ok;
}

/* ---- image checks --------------------------------------------------------- */
static bool headerSane(const SswHeader *h)
{
  if (h->magic != SSW_MAGIC_RELEASE && h->magic != SSW_MAGIC_DEV) return false;
  if (h->header_version != SSW_HEADER_VERSION || h->header_size != SSW_HEADER_SIZE) return false;
  if (strncmp(h->board, SSW_BOARD, sizeof(h->board)) != 0) return false;
  if (h->load_addr != SSW_APP_ADDR || h->vector_offset != SSW_APP_VECTOR_OFF) return false;
  if (h->image_end <= SSW_APP_ADDR + SSW_APP_VECTOR_OFF || h->image_end > SSW_APP_ADDR + SSW_APP_MAX) return false;
  if (h->magic == SSW_MAGIC_RELEASE) {
    if (SswCrc32(h, offsetof(SswHeader, header_crc)) != h->header_crc) return false;
    if (h->payload_size != h->image_end - SSW_APP_ADDR - SSW_HEADER_SIZE) return false;
  }
  return true;
}

static bool vectorsSane(const uint32_t *v, uint32_t end)
{
  uint32_t sp = v[0], rst = v[1];
  bool spOk = (sp > 0x20000000UL && sp <= 0x20020000UL) || (sp > 0x10000000UL && sp <= 0x10010000UL);
  bool rstOk = (rst & 1U) && rst > SSW_APP_ADDR + SSW_APP_VECTOR_OFF && rst < end;
  return spOk && rstOk;
}

/* Internal image: header sane, vectors sane, and for release images the payload SHA-256. */
static bool internalValid(SswHeader *out, bool *isDev)
{
  const SswHeader *h = (const SswHeader *)SSW_APP_ADDR;
  if (!headerSane(h)) return false;
  if (!vectorsSane((const uint32_t *)(SSW_APP_ADDR + SSW_APP_VECTOR_OFF), h->image_end)) return false;
  *isDev = h->magic == SSW_MAGIC_DEV;
  if (!*isDev) {
    SswSha256 c; uint8_t d[32];
    SswSha256_Init(&c);
    const uint8_t *p = (const uint8_t *)(SSW_APP_ADDR + SSW_HEADER_SIZE);
    for (uint32_t done = 0; done < h->payload_size; ) {
      uint32_t n = h->payload_size - done > 4096U ? 4096U : h->payload_size - done;
      SswSha256_Update(&c, p + done, n); done += n; kick();
    }
    SswSha256_Final(&c, d);
    if (memcmp(d, h->sha256, 32) != 0) return false;
  }
  if (out) *out = *h;
  return true;
}

static uint8_t buf[4096];

/* SHA-256 of a range of chip 2. */
static bool xfSha(uint32_t addr, uint32_t len, uint8_t out[32])
{
  SswSha256 c;
  SswSha256_Init(&c);
  while (len) {
    uint32_t n = len > sizeof(buf) ? sizeof(buf) : len;
    if (!xfRead(addr, buf, n)) return false;
    SswSha256_Update(&c, buf, n); addr += n; len -= n;
  }
  SswSha256_Final(&c, out);
  return true;
}

/* Staging must be a release image with a matching payload hash. */
static bool stagingValid(SswHeader *h)
{
  if (!xfRead(SSW_SLOT_STAGING, h, sizeof(*h))) return false;
  if (h->magic != SSW_MAGIC_RELEASE || !headerSane(h)) return false;
  uint8_t d[32];
  if (!xfSha(SSW_SLOT_STAGING + SSW_HEADER_SIZE, h->payload_size, d)) return false;
  return memcmp(d, h->sha256, 32) == 0;
}

/* ---- boot record ---------------------------------------------------------- */
static uint32_t brNextSlot = SSW_BOOTREC_A;

static bool brValid(const SswBootRecord *r)
{
  return r->magic == SSW_BOOTREC_MAGIC && r->seq != 0xFFFFFFFFU && SswCrc32(r, offsetof(SswBootRecord, crc)) == r->crc;
}

static void brRead(SswBootRecord *r)
{
  SswBootRecord a, b;
  bool va = xfRead(SSW_BOOTREC_A, &a, sizeof(a)) && brValid(&a);
  bool vb = xfRead(SSW_BOOTREC_B, &b, sizeof(b)) && brValid(&b);
  if (va && (!vb || a.seq > b.seq)) { *r = a; brNextSlot = SSW_BOOTREC_B; }
  else if (vb) { *r = b; brNextSlot = SSW_BOOTREC_A; }
  else { memset(r, 0, sizeof(*r)); r->magic = SSW_BOOTREC_MAGIC; r->state = SSW_BR_IDLE; brNextSlot = SSW_BOOTREC_A; }
}

static bool brWrite(SswBootRecord *r)
{
  r->magic = SSW_BOOTREC_MAGIC;
  r->seq++;
  r->crc = SswCrc32(r, offsetof(SswBootRecord, crc));
  if (!xfErase4k(brNextSlot) || !xfProgram(brNextSlot, (const uint8_t *)r, sizeof(*r))) return false;
  SswBootRecord v;
  if (!xfRead(brNextSlot, &v, sizeof(v)) || memcmp(&v, r, sizeof(v)) != 0) return false;
  brNextSlot = brNextSlot == SSW_BOOTREC_A ? SSW_BOOTREC_B : SSW_BOOTREC_A;
  return true;
}

/* ---- copy operations ------------------------------------------------------ */
/* Internal image -> backup slot, then verify the copy and record its hash. */
static bool backupInternal(SswBootRecord *br)
{
  const SswHeader *h = (const SswHeader *)SSW_APP_ADDR;
  uint32_t size = h->image_end - SSW_APP_ADDR;
  logs("BL: backup "); logu(size); logs(" bytes\r\n");
  br->backup_size = 0;                                    /* invalid until verified */
  for (uint32_t off = 0; off < size; off += 4096U)
    if (!xfErase4k(SSW_SLOT_BACKUP + off)) return false;
  for (uint32_t off = 0; off < size; off += 256U) {
    uint32_t n = size - off > 256U ? 256U : size - off;
    if (!xfProgram(SSW_SLOT_BACKUP + off, (const uint8_t *)(SSW_APP_ADDR + off), n)) return false;
    kick();
  }
  SswSha256 c; uint8_t src[32], dst[32];
  SswSha256_Init(&c);
  SswSha256_Update(&c, (const void *)SSW_APP_ADDR, size);
  SswSha256_Final(&c, src);
  if (!xfSha(SSW_SLOT_BACKUP, size, dst) || memcmp(src, dst, 32) != 0) return false;
  br->backup_size = size;
  memcpy(br->backup_sha, src, 32);
  return true;
}

/* Chip 2 slot -> internal flash. expectedSha covers [from, from+size) of the slot. */
static bool programFromSlot(uint32_t slot, uint32_t size, const uint8_t expectedSha[32])
{
  logs("BL: programming "); logu(size); logs(" bytes from slot "); logx(slot); logs("\r\n");
  if (!eraseAppRange(size)) return false;
  HAL_FLASH_Unlock();
  bool ok = true;
  for (uint32_t off = 0; ok && off < size; off += sizeof(buf)) {
    uint32_t n = size - off > sizeof(buf) ? sizeof(buf) : size - off;
    if (!xfRead(slot + off, buf, n)) { ok = false; break; }
    for (uint32_t i = 0; ok && i < n; i += 4U) {
      uint32_t w = 0xFFFFFFFFU;
      memcpy(&w, &buf[i], (n - i) >= 4U ? 4U : n - i);
      ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, SSW_APP_ADDR + off + i, w) == HAL_OK;
    }
    kick();
  }
  HAL_FLASH_Lock();
  if (!ok) { logs("BL: program error\r\n"); return false; }
  SswSha256 c; uint8_t d[32];
  SswSha256_Init(&c);
  SswSha256_Update(&c, (const void *)SSW_APP_ADDR, size);
  SswSha256_Final(&c, d);
  if (memcmp(d, expectedSha, 32) != 0) { logs("BL: verify after programming failed\r\n"); return false; }
  return true;
}

static bool installStaging(const SswHeader *h)
{
  /* the staging hash covers the payload; verify the whole image incl. header instead */
  uint32_t size = h->image_end - SSW_APP_ADDR;
  uint8_t whole[32];
  if (!xfSha(SSW_SLOT_STAGING, size, whole)) return false;
  return programFromSlot(SSW_SLOT_STAGING, size, whole);
}

static bool restoreBackup(const SswBootRecord *br)
{
  if (br->backup_size == 0U || br->backup_size == 0xFFFFFFFFU || br->backup_size > SSW_APP_MAX) return false;
  uint8_t d[32];
  if (!xfSha(SSW_SLOT_BACKUP, br->backup_size, d) || memcmp(d, br->backup_sha, 32) != 0) {
    logs("BL: backup slot corrupt\r\n");
    return false;
  }
  return programFromSlot(SSW_SLOT_BACKUP, br->backup_size, br->backup_sha);
}

/* ---- jump ----------------------------------------------------------------- */
static void jumpToApp(void)
{
  const uint32_t *v = (const uint32_t *)(SSW_APP_ADDR + SSW_APP_VECTOR_OFF);
  logs("BL: start application\r\n");
  while (__HAL_UART_GET_FLAG(&huart6, UART_FLAG_TC) == RESET) { }
  HAL_GPIO_TogglePin(Clr_wdt_GPIO_Port, Clr_wdt_Pin);
  (void)HAL_SPI_DeInit(&hspi2);
  (void)HAL_UART_DeInit(&huart6);
  HAL_GPIO_DeInit(GPIOD, SPI_CS1_Pin | SPI_CS2_Pin);
  HAL_GPIO_DeInit(LED_Blue_GPIO_Port, LED_Blue_Pin);
  __disable_irq();
  SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;
  /* Back to the reset clock setup (HSI) by hand: HAL_RCC_DeInit() would also clear the
   * RCC reset flags, which the application reads to report the reset cause. */
  RCC->CR |= RCC_CR_HSION;
  while ((RCC->CR & RCC_CR_HSIRDY) == 0U) { }
  RCC->CFGR = 0U;
  while ((RCC->CFGR & RCC_CFGR_SWS) != 0U) { }
  RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON | RCC_CR_CSSON);
  while (RCC->CR & RCC_CR_PLLRDY) { }
  RCC->PLLCFGR = 0x24003010U;   /* reset value */
  RCC->CIR = 0U;
  for (unsigned int i = 0; i < 8U; i++) { NVIC->ICER[i] = 0xFFFFFFFFU; NVIC->ICPR[i] = 0xFFFFFFFFU; }
  SCB->VTOR = SSW_APP_ADDR + SSW_APP_VECTOR_OFF;
  __DSB(); __ISB();
  __set_MSP(v[0]);
  __enable_irq();
  ((void (*)(void))v[1])();
  while (1) { }
}

/* ---- rescue --------------------------------------------------------------- */
static void rescue(void)
{
  logs("BL: RESCUE - no valid firmware. Program with ST-LINK at 0x08008000.\r\n");
  uint32_t t = 0;
  for (;;) {
    kick();
    if (HAL_GetTick() - t >= 100U) { t = HAL_GetTick(); HAL_GPIO_TogglePin(LED_Blue_GPIO_Port, LED_Blue_Pin); }
    static uint32_t say;
    if (HAL_GetTick() - say >= 5000U) { say = HAL_GetTick(); logs("BL: rescue mode\r\n"); }
  }
}

/* ---- main ----------------------------------------------------------------- */
void Bl_Main(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  kick();
  logs("\r\nBL: SmartSwitch bootloader " BL_VERSION "\r\n");

  SswBootRecord br;
  brRead(&br);
  logs("BL: boot record state "); logu(br.state); logs(" seq "); logu(br.seq); logs("\r\n");

  SswHeader cur, stg;
  bool curDev = false;
  bool curOk = internalValid(&cur, &curDev);
  bool userRollback = false;

  for (int guard = 0; guard < 6; guard++) {
    if (br.state == SSW_BR_PENDING) {
      if (!stagingValid(&stg)) {
        logs("BL: staging image invalid, install cancelled\r\n");
        br.state = SSW_BR_IDLE; br.result = SSW_RES_INSTALL_FAILED; br.result_version = 0;
        (void)brWrite(&br);
        continue;
      }
      logs("BL: install "); logver(stg.fw_version); logs(" ("); logbuild(stg.fw_build); logs(")\r\n");
      if (curOk && !backupInternal(&br)) {
        logs("BL: backup failed, install cancelled\r\n");
        br.state = SSW_BR_IDLE; br.result = SSW_RES_INSTALL_FAILED; br.result_version = stg.fw_version;
        (void)brWrite(&br);
        continue;
      }
      br.state = SSW_BR_INSTALLING;
      (void)brWrite(&br);
      continue;
    }
    if (br.state == SSW_BR_INSTALLING) {
      if (stagingValid(&stg) && installStaging(&stg)) {
        br.state = SSW_BR_TRIAL; br.trial_boots = 0; br.result = SSW_RES_NONE; br.result_version = stg.fw_version;
        (void)brWrite(&br);
        curOk = internalValid(&cur, &curDev);
        logs("BL: installed, trial run\r\n");
      } else {
        logs("BL: install failed, restoring backup\r\n");
        br.state = SSW_BR_ROLLBACK; br.result = SSW_RES_INSTALL_FAILED; br.result_version = stg.fw_version;
        (void)brWrite(&br);
      }
      continue;
    }
    if (br.state == SSW_BR_TRIAL) {
      br.trial_boots++;
      if (br.trial_boots > SSW_TRIAL_MAX_BOOTS || !curOk) {
        logs("BL: trial image not confirmed after "); logu(br.trial_boots - 1U); logs(" boots, rolling back\r\n");
        br.state = SSW_BR_ROLLBACK; br.result = SSW_RES_ROLLED_BACK;
      } else {
        logs("BL: trial boot "); logu(br.trial_boots); logs("/"); logu(SSW_TRIAL_MAX_BOOTS); logs("\r\n");
      }
      (void)brWrite(&br);
      if (br.state == SSW_BR_TRIAL) break;
      continue;
    }
    if (br.state == SSW_BR_ROLLBACK) {
      if (br.result == SSW_RES_NONE) userRollback = true;
      if (restoreBackup(&br)) {
        br.state = SSW_BR_IDLE;
        if (userRollback) br.result = SSW_RES_RESTORED;
        (void)brWrite(&br);
        curOk = internalValid(&cur, &curDev);
        logs("BL: backup restored\r\n");
      } else {
        logs("BL: rollback impossible\r\n");
        br.state = SSW_BR_IDLE;
        (void)brWrite(&br);
      }
      continue;
    }
    break;   /* IDLE */
  }

  if (!curOk) {
    /* corrupt or missing image: repair from backup, else from a valid staging image */
    logs("BL: internal image invalid\r\n");
    SswBootRecord rb = br;
    if (restoreBackup(&rb)) {
      br.result = SSW_RES_REPAIRED; (void)brWrite(&br);
    } else if (stagingValid(&stg) && installStaging(&stg)) {
      br.state = SSW_BR_TRIAL; br.trial_boots = 1; br.result = SSW_RES_REPAIRED; br.result_version = stg.fw_version;
      (void)brWrite(&br);
    }
    curOk = internalValid(&cur, &curDev);
  }
  if (!curOk) rescue();

  logs("BL: image "); logver(cur.fw_version); logs(" ("); logbuild(cur.fw_build); logs(curDev ? ") DEV build, no hash\r\n" : ") SHA-256 OK\r\n");
  jumpToApp();
}
