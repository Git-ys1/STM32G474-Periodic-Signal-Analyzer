/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dac.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "AD9833.h"
#include "arm_math.h"
#include "arm_const_structs.h"
#include "analyzer_bridge.h"
#include "display.h"
#include "math.h"
#include "stdio.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define IN1_O() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_0,GPIO_PIN_RESET)
#define IN1_C() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_0,GPIO_PIN_SET)
#define IN2_O() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_RESET)
#define IN2_C() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_1,GPIO_PIN_SET)
#define IN3_O() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_2,GPIO_PIN_RESET)
#define IN3_C() HAL_GPIO_WritePin(GPIOB,GPIO_PIN_2,GPIO_PIN_SET)
#define IN4_O() HAL_GPIO_WritePin(GPIOC,GPIO_PIN_12,GPIO_PIN_RESET)
#define IN4_C() HAL_GPIO_WritePin(GPIOC,GPIO_PIN_12,GPIO_PIN_SET)
#define ADC_SIZE 2048
#define ANALYZER_SAMPLE_RATE_HZ 1024000.0f
#define ADC_VOLTS_PER_CODE      (3.3f / 4096.0f)
/* float ר�� */
#define SWAP_F(a, b)  do { float _t = (a); (a) = (b); (b) = _t; } while (0)
uint16_t adc_b[ADC_SIZE]={0};
__IO uint8_t AdcConvEnd = 0;
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/*
 * ArmClang 6.7 does not automatically emit this marker for the current
 * main(void).  Without it, the C runtime tries to obtain argc/argv through
 * semihosting before main() and executes BKPT 0xAB on standalone hardware.
 */
#if defined(__ARMCC_VERSION)
__attribute__((used)) int __ARM_use_no_argv;
#endif

float32_t input[ADC_SIZE*2+4],output[ADC_SIZE/2],VO[ADC_SIZE],V0=0;
float32_t max;uint32_t index;
float F,V,FB,VB,VC,FC,TE;
uint32_t valid_len;
uint32_t discard_len;
uint16_t flag=0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void sof()
{
	if(F>FB) 
	{
		SWAP_F(F,FB);
		SWAP_F(V,VB);
	}
	if(FB>FC) 
	{
		SWAP_F(FB,FC);
		SWAP_F(VB,VC);
	}
	if(F>FB) 
	{
		SWAP_F(F,FB);
		SWAP_F(V,VB);
	}
}

void fft()
{
	
	arm_cfft_f32(&arm_cfft_sR_f32_len2048,input,0,1);
	 arm_cmplx_mag_f32(input, output, ADC_SIZE / 2 );
	//arm_cmplx_mag_squared_f32(input,output,ADC_SIZE/2);
	index=0;
	max=output[1];
	arm_max_f32(&output[1],ADC_SIZE/2-1,&max,&index);
	
	for(int i=-8;i<9;i++)
		{
			output[index+i+1]=0;
		}
	F=(index+1)*1024000.0/2048.0;
	V=2.0*max/2048.0;
	
	arm_max_f32(&output[1],ADC_SIZE/2-1,&max,&index);
		
	for(int i=-8;i<9;i++)
		{
			output[index+i+1]=0;
		}
	FB=(index+1)*1024000.0/2048.0;
	VB=2.0*max/2048.0;
	
	arm_max_f32(&output[1],ADC_SIZE/2-1,&max,&index);
		
	for(int i=-8;i<9;i++)
		{
			output[index+i+1]=0;
		}
	FC=(index+1)*1024000.0/2048.0;
	VC=2.0*max/2048.0;
	
	if(VC<0.004)
		flag=2;
	else
	{
		sof();
		//if((uint32_t)FB % (uint32_t)F!=0 || (uint32_t)FC % (uint32_t)F!=0)
			flag=3;
		
	}
		
	V0=output[0]/2048.0;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
	if(hadc == &hadc2)
	{
		AdcConvEnd=1;
HAL_ADC_Stop_DMA(&hadc2);
	}
}

/* ³�����ֵ: �ðٷ�λ�������� */
float Vpp_Robust(const float *data, uint32_t len)
{
    static float sorted[2048];
    uint32_t i, j;
    float key;

    /* 1. ���� */
    for (i = 0; i < len; i++) sorted[i] = data[i];

    /* 2. ��������(2048���Cortex-M4����)
       Ҳ���� arm_sort_f32 (CMSIS-DSP 1.10+) */
    for (i = 1; i < len; i++) {
        key = sorted[i];
        j = i;
        while (j > 0 && sorted[j-1] > key) {
            sorted[j] = sorted[j-1];
            j--;
        }
        sorted[j] = key;
    }

    /* 3. ȡ 1% �� 99% ��λ��, ���˼������� */
		float idx_low=0 ;           /* 1% λ�� */
    float idx_high=0; /* 99% λ�� */
		for (i = 0; i < 5; i++) {
			idx_low+=sorted[i];
			idx_high+=sorted[2047-i];
		}
		idx_low/=5.0;
		idx_high/=5.0;
    return idx_high- idx_low;
}



float Vpp_R()
{
			float fm=F;
			uint32_t cycles = (uint32_t)floor(2048.0 * fm / 1024000.0);
			double exact_len = cycles *  1024000.0 / fm;
			valid_len = (uint32_t)floor(exact_len);
			discard_len = 2048 - valid_len;
			for(int i=0;i<discard_len;i++)
				VO[2047-i]=0;
			for (int i=0; i < 2048; i++)
			{
				input[i*2]=VO[i];input[i*2+1]=0;
			}
			fft();
			for(int i=0;i<2048;i++)
				VO[i]-=V0;
			float Vrms=0;
			arm_rms_f32(VO, valid_len, &Vrms);
			return Vrms;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_DAC1_Init();
  MX_USART3_UART_Init();
  MX_ADC2_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);
HAL_TIM_Base_Start(&htim3);
HAL_ADC_Start_DMA(&hadc2,(uint32_t*)adc_b,2048);
AnalyzerBridge_Init();
Display_Init(&huart3);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
		if(AdcConvEnd == 1)
		{
			float spectrum_frequencies_hz[ANALYZER_MAX_COMPONENTS];
			float spectrum_amplitudes_v[ANALYZER_MAX_COMPONENTS];
			uint8_t spectrum_flag;

			for (int i=0; i < 2048; i++)
			{
				input[i*2]=adc_b[i]*3.3/4096;VO[i]=input[i*2];input[i*2+1]=0;
			}
			fft();

			/*
			 * Vpp_R()内部会再次执行fft()并覆盖F/V等全局结果。
			 * 因此在保持队友算法不变的前提下，先保存第一次FFT的最终谱峰。
			 */
			spectrum_frequencies_hz[0] = F;
			spectrum_frequencies_hz[1] = FB;
			spectrum_frequencies_hz[2] = FC;
			spectrum_amplitudes_v[0] = V;
			spectrum_amplitudes_v[1] = VB;
			spectrum_amplitudes_v[2] = VC;
			spectrum_flag = (uint8_t)flag;
			
			//float vmax,vmin;
			//uint32_t maxid,minid;
			//arm_max_f32(VO, 2048, &vmax, &maxid);   /* �����ֵ */
			//arm_min_f32(VO, 2048, &vmin, &minid);   /* ����Сֵ */

			float vpp =Vpp_Robust(VO,2048)  ;            /* ���ֵ */

			
			float Vrms=Vpp_R();

			/*
			 * 在一次真实分析全部完成后发布稳定快照。
			 * 单位换算、谱峰排序和一个周期显示波形提取均由桥接层完成。
			 */
			AnalyzerBridge_PublishReal(
				adc_b,
				ADC_SIZE,
				ADC_VOLTS_PER_CODE,
				ANALYZER_SAMPLE_RATE_HZ,
				vpp,
				Vrms,
				spectrum_flag,
				spectrum_frequencies_hz,
				spectrum_amplitudes_v
			);

			AdcConvEnd=0;
			HAL_ADC_Start_DMA(&hadc2,(uint32_t*)adc_b,2048);
			//AdcConvEnd=0;
		}

		Display_Task();
		
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
