#include "AD9833.h"
#include "main.h"
extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;

void AD9833_GPIOinit(void)

{

CS_9833_1(); //首先拉高片选，不使能，防止输入错误数据
CS_9833_11();
}



/*

*******************************************************

向AD9833发送一个16bit的数据

*******************************************************

*/

void AD9833_Write(unsigned short TxData,unsigned short n) //TxData是2字节

{

unsigned char data[2] ; //一个char一个字节，数组为2个字节

data[0] = (unsigned char)((TxData>>8) &0xff); //data[0]存储高位

data[1] = (unsigned char)(TxData&0xff); //data[1]存储低位


	if(n==0)
	{CS_9833_0(); //拉低片选，准备写入
		//HAL_Delay(1);
HAL_SPI_Transmit (&hspi1 , data, 2, 0x02) ; //用HAL库的SPI发送函数发送数据
		CS_9833_1();}
	else
	{	CS_9833_00();//HAL_Delay(1);
HAL_SPI_Transmit (&hspi2, data, 2, 0x02) ;
CS_9833_11(); //发送完毕，拉高片选
	}
}



/*

*******************************************************

Reset: 0为有输出，1为没输出，此位只控制有无输出，不复位寄存器

SleeppMode: 3为关闭内部DAC和时钟，0为不关闭

optionbit|modebit: 00正弦01三角10方波11保留

*******************************************************

*/

void AD9833_CtrlSet(unsigned char Reset,unsigned char SleeppMode,unsigned char optionbit,unsigned char modebit,unsigned char Fselect,unsigned char n)

{

unsigned short regtemp = 0; //对输出模式的一些选择

regtemp = regtemp|(((unsigned short)Reset&0x01)<<8); //以下就是把每个位对应到相应的寄存器上，不过好像少了个DIV2,DIV2默认为0了？

regtemp = regtemp|((SleeppMode&0x03)<<6);

regtemp = regtemp|((optionbit&0x01)<<5);

regtemp = regtemp|((modebit&0x01)<<1);
regtemp |= ((Fselect & 0x01) << 11);

regtemp = regtemp | PIN_SW;
AD9833_Write(regtemp,n); //写入数据，不过不需要先声明将要写入吗？

}



/*

*******************************************************

设置频率，设置完后暂不输出，用CtrlSet函数设置输出

频率值：0.1Hz-12.5MHz（最大值为25M晶振时钟的一半）

单位：Hz;例如，输出1M，则输入1000000

*******************************************************

*/

void AD9833_FreqSet(double Freq,int n,unsigned char nn) //Freq是用户输入的以Hz为单位的频率

{

int frequence_LSB,frequence_MSB; //LSB和MSB分别对应频率寄存器里的LSB和MSB

double frequence_mid,frequence_DATA; //mid是一个中间值，用于将输入频率转换为AD9833可以接受的格式；DATA为输入进去的频率的十进制

long int frequence_hex; //hex为输入进去的频率的16进制



frequence_mid = 268435456/25; //f_{out}=dds输出频率；F_{cw}=f_{DATA}；f_{ref}=25M；2^28=268435456；1000000=1M

frequence_DATA = Freq; //公式：f_{out}=(F_{cw})*(f_{ref})/(2^28)=(f_{DATA})*25M/(2^28)

frequence_DATA = frequence_DATA/1000000; //这里的f_{DATA}(化作以M为单位)=(Freq/1000000)*[(2^28)/(25)]，;f_{DATA}作为f_{cw}输入进公式，刚好得到f_{out}=Freq，且以Hz为单位

frequence_DATA = frequence_DATA*frequence_mid;

frequence_hex = frequence_DATA;

frequence_LSB = frequence_hex;

frequence_LSB = frequence_LSB&0x3fff; //以下就是一些与或操作，用来输入寄存器

frequence_MSB = frequence_hex>>14;

frequence_MSB = frequence_MSB&0x3fff;


if(n==0)
{
frequence_LSB = frequence_LSB|0x4000;

frequence_MSB = frequence_MSB|0x4000; }//4000==0100 0000 0000 0000,把前两位变成01，代表freq0寄存器}
else
{

frequence_LSB = frequence_LSB|0x8000;

frequence_MSB = frequence_MSB|0x8000; }//4000==0100 0000 0000 0000,把前两位变成01，代表freq0寄存器}


//AD9833_Write(0x2100); //2100==0010 0001 0000 0000;bit8为1->不reset;0010->连续写LSB、MSB标志
AD9833_Write(B28 | PIN_SW,nn);
AD9833_Write(frequence_LSB,nn);

AD9833_Write(frequence_MSB,nn);



}

/*
 * 功能说明: 向指定相位寄存器写入角度值
 * 参数说明: reg - 相位寄存器选择: 0=PHASE0, 1=PHASE1
 *           angle_deg - 相位角度(0~360°)
 */
void AD9833_SetPhase(uint8_t reg, float angle_deg,unsigned char n) {
    uint16_t phase_word = (uint16_t)((angle_deg / 360.0f) * 4096.0f);
    phase_word &= 0x0FFF; // 确保只有低12位有效
    
    uint16_t cmd = 0;
    if (reg == 0) {
        cmd = PHASE0 | phase_word; // 写入PHASE0
    } else {
        cmd = PHASE1 | phase_word; // 写入PHASE1
    }
    AD9833_Write(cmd,n);
}

/*
 * 功能说明: 在两个预置相位之间切换输出信号
 * 参数说明: select - 目标相位: 0=PHASE0, 1=PHASE1
 */
void AD9833_SwitchPhase(uint8_t select,unsigned char n) {
    uint16_t ctrl_word = B28 | PIN_SW;   // 关键：PIN_SW=1，使软件选择相位有效
    if (select == 1) {
        ctrl_word |= PSELECT;            // PSELECT=1 选择 PHASE1
    }
    // 否则 PSELECT=0，选择 PHASE0
    AD9833_Write(ctrl_word,n);
}