/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define ADC_CH0_Pin GPIO_PIN_0
#define ADC_CH0_GPIO_Port GPIOC
#define ADC_CH1_Pin GPIO_PIN_2
#define ADC_CH1_GPIO_Port GPIOC
#define ADC_CH8_Pin GPIO_PIN_3
#define ADC_CH8_GPIO_Port GPIOC
#define ADC_CH2_Pin GPIO_PIN_0
#define ADC_CH2_GPIO_Port GPIOA
#define ADC_A3_Pin GPIO_PIN_3
#define ADC_A3_GPIO_Port GPIOA
#define ADC_CH4_Pin GPIO_PIN_4
#define ADC_CH4_GPIO_Port GPIOA
#define ADC_CH5_Pin GPIO_PIN_5
#define ADC_CH5_GPIO_Port GPIOA
#define ADC_CH9_Pin GPIO_PIN_6
#define ADC_CH9_GPIO_Port GPIOA
#define ADC_CH6_Pin GPIO_PIN_0
#define ADC_CH6_GPIO_Port GPIOB
#define ADC_CH7_Pin GPIO_PIN_1
#define ADC_CH7_GPIO_Port GPIOB
#define Clr_wdt_Pin GPIO_PIN_7
#define Clr_wdt_GPIO_Port GPIOE
#define Batt_FAULT_Pin GPIO_PIN_11
#define Batt_FAULT_GPIO_Port GPIOE
#define Batt_FAULT_EXTI_IRQn EXTI15_10_IRQn
#define Batt_status_Pin GPIO_PIN_13
#define Batt_status_GPIO_Port GPIOE
#define DIAG_S1_Pin GPIO_PIN_8
#define DIAG_S1_GPIO_Port GPIOD
#define DIAG_S1_EXTI_IRQn EXTI9_5_IRQn
#define DIAG_S2_Pin GPIO_PIN_10
#define DIAG_S2_GPIO_Port GPIOD
#define DIAG_S2_EXTI_IRQn EXTI15_10_IRQn
#define DIAG_S3_Pin GPIO_PIN_12
#define DIAG_S3_GPIO_Port GPIOD
#define DIAG_S3_EXTI_IRQn EXTI15_10_IRQn
#define SPI_CS2_Pin GPIO_PIN_13
#define SPI_CS2_GPIO_Port GPIOD
#define DIAG_S4_Pin GPIO_PIN_14
#define DIAG_S4_GPIO_Port GPIOD
#define DIAG_S4_EXTI_IRQn EXTI15_10_IRQn
#define LTE_DTR_Pin GPIO_PIN_15
#define LTE_DTR_GPIO_Port GPIOD
#define RS485_DIR_Pin GPIO_PIN_9
#define RS485_DIR_GPIO_Port GPIOC
#define SW1_Pin GPIO_PIN_15
#define SW1_GPIO_Port GPIOA
#define SW1_EXTI_IRQn EXTI15_10_IRQn
#define En_3v3_AMC_Pin GPIO_PIN_10
#define En_3v3_AMC_GPIO_Port GPIOC
#define LED_Blue_Pin GPIO_PIN_11
#define LED_Blue_GPIO_Port GPIOC
#define LTE_POW_Pin GPIO_PIN_1
#define LTE_POW_GPIO_Port GPIOD
#define LTE_RST_Pin GPIO_PIN_2
#define LTE_RST_GPIO_Port GPIOD
#define LTE_WDIS_Pin GPIO_PIN_3
#define LTE_WDIS_GPIO_Port GPIOD
#define SPI_CS1_Pin GPIO_PIN_7
#define SPI_CS1_GPIO_Port GPIOD
#define Vin_Detect_Pin GPIO_PIN_3
#define Vin_Detect_GPIO_Port GPIOB
#define SW2_Pin GPIO_PIN_5
#define SW2_GPIO_Port GPIOB
#define SW2_EXTI_IRQn EXTI9_5_IRQn
#define eFuse_iLim_Pin GPIO_PIN_9
#define eFuse_iLim_GPIO_Port GPIOB
#define eFuse_iLim_EXTI_IRQn EXTI9_5_IRQn
#define En_3v3_ETH_Pin GPIO_PIN_0
#define En_3v3_ETH_GPIO_Port GPIOE
#define En_3v3_Per_Pin GPIO_PIN_1
#define En_3v3_Per_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
