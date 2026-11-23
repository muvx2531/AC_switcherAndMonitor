#ifndef DEBUG_PROTOCOL_H
#define DEBUG_PROTOCOL_H
#include <stdbool.h>
#include "stm32f4xx_hal.h"
bool DebugProtocol_Start(UART_HandleTypeDef *uart);
#endif
