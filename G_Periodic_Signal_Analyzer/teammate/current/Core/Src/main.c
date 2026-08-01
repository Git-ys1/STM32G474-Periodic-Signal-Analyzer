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
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "AD9833.h"
#include "arm_math.h"
#include "arm_const_structs.h"
#include "stdio.h"
#include "goertzel_sync.h"
#include "math.h"
#include "analyzer_bridge.h"
#include "display.h"
#include "goertzel_sync.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define ADC_SIZE 4096
/* float ר�� */
#define SWAP_F(a, b)  do { float _t = (a); (a) = (b); (b) = _t; } while (0)
uint16_t adc_b[ADC_SIZE/2]={0},adc_b1[ADC_SIZE/2]={0};
__IO uint8_t AdcConvEnd = 0;
#define ANALYZER_SAMPLE_RATE_HZ  2048193.0f
#define BOARD_KEY_CONTROL_ENABLE 1U
#define BOARD_KEY_DEBOUNCE_MS    30U
#define BOARD_KEY_LONG_PRESS_MS  1000U

#define PI 3.14159265358979323846f
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
float32_t input[ADC_SIZE*2+4],output[ADC_SIZE/2],VO[ADC_SIZE],V0=0;
float32_t max;uint32_t index;
float F,V,FB,VB,VC,FC,TE,Vr=0;
uint32_t valid_len;
uint32_t discard_len;
uint16_t flag=0;

extern const short bw9_lut[];  /* 你的LUT，自己声明 */

static float g_f1, g_a1, g_phi1;
static float g_f2, g_a2, g_ph2;
static float g_f3, g_a3, g_ph3;
static int   g_n;  /* 分量数 1~3 */

#if defined(__ARMCC_VERSION)
__attribute__((used)) int __ARM_use_no_argv;
#endif

#if BOARD_KEY_CONTROL_ENABLE
static volatile uint32_t s_key_press_tick;
static volatile uint8_t s_key_press_active;
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if BOARD_KEY_CONTROL_ENABLE
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if ((pin == KEY_Pin) &&
        (s_key_press_active == 0U) &&
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET))
    {
        s_key_press_tick = HAL_GetTick();
        s_key_press_active = 1U;
    }
}

static void BoardKey_Task(void)
{
    uint32_t held_ms;

    if ((s_key_press_active == 0U) ||
        (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_SET))
    {
        return;
    }

    held_ms = HAL_GetTick() - s_key_press_tick;
    s_key_press_active = 0U;

    if (held_ms < BOARD_KEY_DEBOUNCE_MS)
    {
        return;
    }

    if (held_ms >= BOARD_KEY_LONG_PRESS_MS)
    {
        Display_RequestRefresh();
    }
    else
    {
        Display_TogglePeriods();
    }
}
#endif

static float bw9_phase(float f)
{
      return 0;
}

static void set_base(float f1, float a1, float ph_meas1)
{
    g_f1 = f1;  g_a1 = a1;  g_phi1 = ph_meas1;
    g_f2 = g_a2 = g_ph2 = 0;
    g_f3 = g_a3 = g_ph3 = 0;
    g_n = 1;
}

static float add_harmonic(float fk, float ak, float ph_meas)
{
    float Phik = ph_meas  - bw9_phase(fk);
    float Phi1 = g_phi1   - bw9_phase(g_f1);
    int   mk   = (int)(fk / g_f1 + 0.5f);
    float phi  = Phik - mk * Phi1 - (mk - 1) * (PI / 2);
    phi = atan2f(sinf(phi), cosf(phi));  /* norm */

    g_n++;
    if (g_n == 2) { g_f2 = fk; g_a2 = ak; g_ph2 = phi; }
    if (g_n == 3) { g_f3 = fk; g_a3 = ak; g_ph3 = phi; }
    return phi;
}

static float get_upp(void)
{
    float dt = 1.0f / ANALYZER_SAMPLE_RATE_HZ;
    int   n  = (int)(3.0f / g_f1 / dt);
    if (n > 2048) n = 2048;
    float mx = -1e9, mn = 1e9;
    for (int i = 0; i < n; i++) {
        float t  = i * dt;
        float v  = g_a1 * sinf(2*PI*g_f1*t);
        if (g_n >= 2) v += g_a2 * sinf(2*PI*g_f2*t + g_ph2);
        if (g_n >= 3) v += g_a3 * sinf(2*PI*g_f3*t + g_ph3);
        if (v > mx) mx = v;
        if (v < mn) mn = v;
    }
    return mx - mn;
}

float getup(float f1, float v1,float f2,float v2,float p2,float f3,float v3,float p3) {
    float mx=0,mi=0,a=0;
		//if(flag==2) v3=0;
    for (int i = 0; i < 4096; i++) {
       if(flag==2)
				 a=v1*arm_sin_f32(2*PI*i/1000.0)+v2*arm_sin_f32(2*PI*i/1000.0+p2);
			 else
				 a=v1*arm_sin_f32(2*PI*f1*i/4096.0)+v2*arm_sin_f32(2*PI*f2*i/4096.0+p2)+v3*arm_sin_f32(2*PI*f3*i/4096.0+p3);
			 
			if(a > mx)
				mx=a;
			if(a<mi)
				mi=a;
					
    }
    return mx - mi;
}

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
	
	arm_cfft_f32(&arm_cfft_sR_f32_len4096,input,0,1);
	 arm_cmplx_mag_f32(input, output, ADC_SIZE / 2 );
	for(int i=0;i<1024;i++)
	{
			if(i<300)
				output[i]/=5.92;
			else if(i<420)
				output[i]/=6.1;
			else if(i<520)
				output[i]/=6.19;
			else if(i<560)
				output[i]/=6.25;
			else if(i<600)
				output[i]/=6.32;
			else if(i<680)
				output[i]/=6.43;
			else if(i<720)
				output[i]/=6.62;
			else if(i<800)
				output[i]/=6.71;
			else if(i<840)
				output[i]/=6.83;
			else if(i<880)
				output[i]/=6.92;
			else
				output[i]/=7.16;
	}
	//arm_cmplx_mag_squared_f32(input,output,ADC_SIZE/2);
	index=0;
	max=output[1];
	arm_max_f32(&output[1],1023,&max,&index);
	float sum=0,summ=0;
			
	F=(index+1)*2048000.0/4096.0;
		//V=2.0*max/4096.0;
	for(int i=-4;i<5;i++)
		{
			sum+=output[index+i+1]*output[index+i+1];output[index+i+1]=0;
		}
	V=2.0*sqrtf(sum)/4096.0;
	summ+=sum;
		
	arm_max_f32(&output[1],1023,&max,&index);
	sum=0;
	FB=(index+1)*2048000.0/4096.0;
	for(int i=-4;i<5;i++)
		{
			sum+=output[index+i+1]*output[index+i+1];output[index+i+1]=0;
		}
	//FB=(index+1)*2048000.0/4096.0;
	//VB=2.0*max/2048.0;
	VB=2.0*sqrtf(sum)/4096.0;
	summ+=sum;
	
	arm_max_f32(&output[1],1023,&max,&index);
	sum=0;
	FC=(index+1)*2048000.0/4096.0;
	for(int i=-4;i<5;i++)
		{
			sum+=output[index+i+1]*output[index+i+1];output[index+i+1]=0;
		}
	//FC=(index+1)*2048000.0/4096.0;
	//VC=2.0*max/2048.0;
	VC=2.0*sqrtf(sum)/4096.0;
	
	
	if(VC<0.00477 || (FC<FB && FC<F))
	{
		flag=2;FC=1024000;
	}
	else
	{
		float min_ab = (F < FB) ? F : FB;
		if((uint32_t)FC % (uint32_t)min_ab == 0)
		{
			flag=3;summ+=sum;
		}
		//if((uint32_t)FB % (uint32_t)F!=0 || (uint32_t)FC % (uint32_t)F!=0)
		else
		{
			flag=2;
			FC=1024000;
		}
	}
	sof();
	Vr=	sqrtf(2.0*summ)/4096.0;
	V0=output[0]/4096.0;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
	if(hadc == &hadc1)
	{
		AdcConvEnd+=1;
HAL_ADC_Stop_DMA(&hadc1);
	}
	if(hadc == &hadc2)
	{
		AdcConvEnd+=1;
HAL_ADC_Stop_DMA(&hadc2);
	}
}

/* ³�����ֵ: �ðٷ�λ�������� */
float Vpp_Robust(const float *data, uint32_t len)
{
    static float sorted[4096];
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
		for (i = 240; i < 260; i++) {
			idx_low+=sorted[i];
			idx_high+=sorted[4095-i];
		}
		idx_low/=20.0;
		idx_high/=20.0;
    return idx_high- idx_low;
}



float Vpp_R()
{
			float fm=F;
			uint32_t cycles = (uint32_t)floor(4096.0 * fm / 2048000.0);
			double exact_len = cycles *  2048000.0 / fm;
			valid_len = (uint32_t)round(exact_len);
			discard_len = 4096 - valid_len;
			for(int i=0;i<discard_len;i++)
				VO[4095-i]=0;
			for(int i=0;i<4096;i++)
				VO[i]=(VO[i]-V0);
			for (int i=0; i < 4096; i++)
			{ 
				input[i*2]=VO[i];input[i*2+1]=0;
			}
			fft();
			//arm_mean_f32(VO,valid_len,&V0);
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
  MX_USART3_UART_Init();
  MX_TIM3_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  /* USER CODE BEGIN 2 */
HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);
HAL_ADC_Start_DMA(&hadc1,(uint32_t*)adc_b,2048);
HAL_ADC_Start_DMA(&hadc2,(uint32_t*)adc_b1,2048);
__DSB();
//HAL_TIM_Base_Start(&htim3);
HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4); 
//HAL_TIM_Base_Start(&htim3);
AnalyzerBridge_Init();
Display_Init(&huart3);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
		if(AdcConvEnd == 2)
		{
			HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_4); 
			//HAL_ADC_Stop_DMA(&hadc1);
			//HAL_ADC_Stop_DMA(&hadc2);
			for (int i=0; i < 2048; i++)
			{
				VO[2 * i]     = adc_b[i]*3.3/4096.0;   // 奇数位置: ADC1 (先采样)
				VO[2 * i + 1] = adc_b1[i]*3.3/4096.0;  // 偶数位置: ADC2 (后采样)
			}
			for(int i=0;i<4096;i++)
			{
					input[i*2]=VO[i];input[i*2+1]=0;
			}
			float spectrum_frequencies_hz[ANALYZER_MAX_COMPONENTS];
			float spectrum_amplitudes_v[ANALYZER_MAX_COMPONENTS];
			uint8_t spectrum_flag;
			
			fft();
			
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

			float vpp =Vpp_Robust(VO,4096)  ;            /* ���ֵ */

			GoertzelResult r=goertzel_sync(VO,4096,2048193,F),rB=goertzel_sync(VO,4096,2048193,FB),rC=goertzel_sync(VO,4096,2048193,FC);
			
			
			set_base(F, V, r.phase);
			float phi2 = add_harmonic(FB, VB, rB.phase);
			float phi3;
			if(flag==3)
			{phi3 = add_harmonic(FC, VC, rC.phase);}

			AnalyzerBridge_PrepareReal(
        VO,
        ADC_SIZE,
        ANALYZER_SAMPLE_RATE_HZ,
        spectrum_flag,
        spectrum_frequencies_hz,
        spectrum_amplitudes_v
);

			float Vrms=Vpp_R();
			vpp=getup(F,V,FB,VB,phi2,FC,VC,phi3);
			AnalyzerBridge_PublishPreparedReal(vpp, Vr);

			
			
			AdcConvEnd=0;
			HAL_ADC_Start_DMA(&hadc1,(uint32_t*)adc_b,2048);
			HAL_ADC_Start_DMA(&hadc2,(uint32_t*)adc_b1,2048);
			__DSB();
			HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4); 
			//AdcConvEnd=0;
		}
		#if BOARD_KEY_CONTROL_ENABLE
		BoardKey_Task();
		#endif
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
