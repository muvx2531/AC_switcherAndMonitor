#include "eth_probe.h"
#include "main.h"
#include "cmsis_os.h"
#include "debug_log.h"
#include "tcp_echo_test.h"
#include "core/net.h"
#include "ipv4/ipv4.h"
#include "drivers/mac/stm32f4xx_eth_driver.h"
#include "drivers/phy/lan8742_driver.h"

extern ETH_HandleTypeDef heth;
extern ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];
extern ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];

static bool available;
static NetInterface *testInterface;

/* CycloneTCP's weak default only sets pins for known evaluation boards. */
void stm32f4xxEthInitGpio(NetInterface *interface)
{
  (void)interface;
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
  HAL_ETH_MspInit(&heth);
}

bool EthProbe_Start(void)
{
  static uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  uint32_t id1, id2;

  /* Board LDO enable is active high per the available hardware design. */
  HAL_GPIO_WritePin(En_3v3_ETH_GPIO_Port, En_3v3_ETH_Pin, GPIO_PIN_SET);
  osDelay(100);
  heth.Instance = ETH;
  heth.Init.MACAddr = mac;
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
  testInterface = netGetDefaultInterface();
  MacAddr macAddress;
  Ipv4Addr ip, mask;
  error = macStringToAddr("02:00:00:00:00:01", &macAddress);
  if (!error) error = netSetDriver(testInterface, &stm32f4xxEthDriver);
  if (!error) error = netSetPhyDriver(testInterface, &lan8742PhyDriver);
  if (!error) error = netSetMacAddr(testInterface, &macAddress);
  if (!error) error = netConfigInterface(testInterface);
  if (!error) error = ipv4StringToAddr("192.168.0.223", &ip);
  if (!error) error = ipv4SetHostAddr(testInterface, ip);
  if (!error) error = ipv4StringToAddr("255.255.255.0", &mask);
  if (!error) error = ipv4SetSubnetMask(testInterface, mask);
  if (error) {
    (void)DebugLog_Printf("ETH: network init failed %ld\r\n", (long)error);
    return false;
  }
  (void)DebugLog_Printf("ETH: static test IP 192.168.0.223/24\r\n");
  if (!TcpEchoTest_Start()) {
    (void)DebugLog_Printf("TCP test: task creation failed\r\n");
  }
  available = true;
  return true;
}

void EthProbe_Report(void)
{
  if (!available) return;
  (void)DebugLog_Printf("ETH: link=%s\r\n", testInterface->linkState ? "up" : "down");
}
