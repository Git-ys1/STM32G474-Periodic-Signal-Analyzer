/*
 * AD9833.h
 *
 *  Created on: Apr 29, 2025
 *      Author: 醉在风里
 */

#ifndef INC_AD9833_H_
#define INC_AD9833_H_



#endif /* INC_AD9833_H_ */
#include "main.h"



#define CS_9833_0() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_9,GPIO_PIN_RESET)
#define CS_9833_00() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12,GPIO_PIN_RESET)
#define CS_9833_11() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12,GPIO_PIN_SET)

#define CS_9833_1() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_9,GPIO_PIN_SET)
#define PHASE0 0xC000
#define PHASE1 0xE000
#define B28         (1<<13)
#define PSELECT     (1<<10)
#define PIN_SW    (1 << 9)    // 软件控制 PSELECT/FSELECT 的使能位
void AD9833_GPIOinit(void);

void AD9833_Write(unsigned short TxData,unsigned short n);

void AD9833_CtrlSet(unsigned char Reset,unsigned char SleeppMode,unsigned char optionbit,unsigned char modebit,unsigned char Fselect,unsigned char n);

void AD9833_FreqSet(double Freq,int n,unsigned char nn);
void AD9833_SetPhase(uint8_t reg, float angle_deg,unsigned char n);
void AD9833_SwitchPhase(uint8_t select,unsigned char n);