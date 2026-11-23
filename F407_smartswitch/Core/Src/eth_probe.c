#include "eth_probe.h"
#include "main.h"
#include "cmsis_os.h"
#include "debug_log.h"
#include "web_server.h"
#include "core/net.h"
#include "ipv4/ipv4.h"
#include "dhcp/dhcp_client.h"
#include "drivers/mac/stm32f4xx_eth_driver.h"
#include "drivers/phy/lan8742_driver.h"
#include <stdio.h>
#include <string.h>

extern ETH_HandleTypeDef heth;
extern ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];
extern ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];

#define DHCP_TIMEOUT_MS 10000U
#define FALLBACK_IP "192.168.0.223"
#define FALLBACK_MASK "255.255.255.0"

static bool available;
static NetInterface *iface;
static DhcpClientContext dhcpContext;
static volatile bool dhcpTimedOut, dhcpRunning, usingFallback;
static uint16_t phyId1, phyId2;
static uint8_t macBytes[6];
static Ipv4Addr lastLoggedIp;

/* CycloneTCP's weak default only sets pins for known evaluation boards. */
void stm32f4xxEthInitGpio(NetInterface *interface)
{
  (void)interface;
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
  HAL_ETH_MspInit(&heth);
}

/* Locally administered unicast MAC from a 64-bit FNV-1a hash of the 96-bit UID
 * (decision S7-2). Stable for the life of the chip. */
static void macFromUid(uint8_t mac[6])
{
  uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  const uint8_t *p = (const uint8_t *)uid;
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned int i = 0; i < sizeof(uid); i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
  mac[0] = 0x02U;
  for (unsigned int i = 1; i < 6U; i++) mac[i] = (uint8_t)(h >> (8U * (i - 1U)));
}

/* Called from the CycloneTCP task with the stack locked: only set a flag. */
static void dhcpTimeoutEvent(DhcpClientContext *context, NetInterface *interface)
{
  (void)context; (void)interface;
  dhcpTimedOut = true;
}

/* "02:A2:C0:99:BD:88" (colons, as router DHCP lease pages use). */
static void formatMac(char out[18])
{
  (void)snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
      macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
}

static void applyFallback(void)
{
  Ipv4Addr ip, mask;
  (void)dhcpClientStop(&dhcpContext);
  dhcpRunning = false;
  if (ipv4StringToAddr(FALLBACK_IP, &ip) == NO_ERROR &&
      ipv4StringToAddr(FALLBACK_MASK, &mask) == NO_ERROR &&
      ipv4SetHostAddr(iface, ip) == NO_ERROR &&
      ipv4SetSubnetMask(iface, mask) == NO_ERROR) {
    (void)ipv4SetDefaultGateway(iface, IPV4_UNSPECIFIED_ADDR);
    usingFallback = true;
    (void)DebugLog_Printf("ETH: no DHCP lease in %u s, static fallback %s/24\r\n",
        (unsigned)(DHCP_TIMEOUT_MS / 1000U), FALLBACK_IP);
  } else {
    (void)DebugLog_Printf("ETH: static fallback failed\r\n");
  }
}

bool EthProbe_Start(void)
{
  uint32_t id1, id2;

  macFromUid(macBytes);
  /* Board LDO enable is active high per the available hardware design. */
  HAL_GPIO_WritePin(En_3v3_ETH_GPIO_Port, En_3v3_ETH_Pin, GPIO_PIN_SET);
  osDelay(100);
  heth.Instance = ETH;
  heth.Init.MACAddr = macBytes;
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1524;
  if (HAL_ETH_Init(&heth) != HAL_OK) {
    (void)DebugLog_Printf("ETH probe: MAC init failed, HAL error 0x%08lX\r\n", (unsigned long)heth.ErrorCode);
    return false;
  }
  if (HAL_ETH_ReadPHYRegister(&heth, 0, 2, &id1) != HAL_OK ||
      HAL_ETH_ReadPHYRegister(&heth, 0, 3, &id2) != HAL_OK) {
    (void)DebugLog_Printf("ETH probe: MDIO read failed\r\n");
    return false;
  }
  phyId1 = (uint16_t)id1; phyId2 = (uint16_t)id2;
  (void)DebugLog_Printf("ETH probe: PHY addr=0 ID=%04lX:%04lX\r\n", (unsigned long)id1, (unsigned long)id2);
  if (id1 != 0x0007U || (id2 & 0xfff0U) != 0xc130U) {
    (void)DebugLog_Printf("ETH probe: unexpected PHY ID\r\n");
    return false;
  }
  /* Release the HAL MAC before CycloneTCP takes ownership of MAC and IRQ. */
  if (HAL_ETH_DeInit(&heth) != HAL_OK) return false;
  error_t error = netInit();
  if (error != NO_ERROR) {
    (void)DebugLog_Printf("ETH: netInit failed %ld\r\n", (long)error);
    return false;
  }
  iface = netGetDefaultInterface();
  MacAddr macAddress;
  memcpy(macAddress.b, macBytes, sizeof(macBytes));
  error = netSetDriver(iface, &stm32f4xxEthDriver);
  if (!error) error = netSetPhyDriver(iface, &lan8742PhyDriver);
  if (!error) error = netSetMacAddr(iface, &macAddress);
  if (!error) error = netConfigInterface(iface);
  if (error) {
    (void)DebugLog_Printf("ETH: network init failed %ld\r\n", (long)error);
    return false;
  }
  char macText[18];
  formatMac(macText);
  (void)DebugLog_Printf("ETH: MAC %s (from STM32 UID)\r\n", macText);

  DhcpClientSettings dhcpSettings;
  dhcpClientGetDefaultSettings(&dhcpSettings);
  dhcpSettings.interface = iface;
  dhcpSettings.rapidCommit = FALSE;
  dhcpSettings.timeout = DHCP_TIMEOUT_MS;
  dhcpSettings.timeoutEvent = dhcpTimeoutEvent;
  error = dhcpClientInit(&dhcpContext, &dhcpSettings);
  if (!error) error = dhcpClientStart(&dhcpContext);
  if (error) {
    (void)DebugLog_Printf("ETH: DHCP start failed %ld, using fallback\r\n", (long)error);
    applyFallback();
  } else {
    dhcpRunning = true;
    (void)DebugLog_Printf("ETH: DHCP started (fallback %s after %u s)\r\n", FALLBACK_IP,
        (unsigned)(DHCP_TIMEOUT_MS / 1000U));
  }
  available = true;
  if (!WebServer_Start(iface))
    (void)DebugLog_Printf("WEB: HTTP server start failed\r\n");
  return true;
}

void EthProbe_Report(void)
{
  if (!available) return;
  /* Fallback is applied here, outside the CycloneTCP task, to avoid self-locking. */
  if (dhcpTimedOut && dhcpRunning && dhcpClientGetState(&dhcpContext) != DHCP_STATE_BOUND) applyFallback();
  Ipv4Addr ip = IPV4_UNSPECIFIED_ADDR;
  (void)ipv4GetHostAddr(iface, &ip);
  if (ip != lastLoggedIp) {
    char text[16];
    lastLoggedIp = ip;
    (void)DebugLog_Printf("ETH: address %s (%s)\r\n", ipv4AddrToString(ip, text),
        usingFallback ? "static fallback" : "DHCP");
  }
}

void EthProbe_GetInfo(EthInfo *info)
{
  memset(info, 0, sizeof(*info));
  formatMac(info->mac);
  info->phyId1 = phyId1; info->phyId2 = phyId2;
  if (!available) return;
  Ipv4Addr ip = IPV4_UNSPECIFIED_ADDR, mask = IPV4_UNSPECIFIED_ADDR, gw = IPV4_UNSPECIFIED_ADDR;
  (void)ipv4GetHostAddr(iface, &ip);
  (void)ipv4GetSubnetMask(iface, &mask);
  (void)ipv4GetDefaultGateway(iface, &gw);
  info->available = true;
  info->link = iface->linkState;
  info->dhcp = !usingFallback && dhcpRunning && dhcpClientGetState(&dhcpContext) == DHCP_STATE_BOUND;
  info->addressValid = ip != IPV4_UNSPECIFIED_ADDR;
  if (info->addressValid) {
    ipv4AddrToString(ip, info->ip);
    ipv4AddrToString(mask, info->mask);
    if (gw != IPV4_UNSPECIFIED_ADDR) ipv4AddrToString(gw, info->gw);
  }
}
