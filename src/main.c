/********************************** (C) COPYRIGHT *******************************
* File Name		  : CDC.C
* Author			 : Kongou Hikari
* Version			: V1.0
* Date			   : 2019/02/16
* Description		: CH552 USB to Serial with FTDI Protocol
*******************************************************************************/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ch554.h>
#include <ch554_usb.h>
#include <debug.h>

/*
 * Use T0 to count the SOF_Count every 1ms
 * If you doesn't like this feature, define SOF_NO_TIMER
 * Background: The usb host must to send SOF every 1ms, but some USB host don't really do that
 * FTDI's driver has some bug, if it doesn't received empty packet with modem status,
 * it will causes BSoD, so highly recommended use T0 instead of SOF packet to generate empty packet report.
 */
//#define SOF_NO_TIMER

/*
Memory map:
EP0 Buf		00 - 3f
EP4 Buf 	40 - 7f
EP1 Buf		80 - bf
RingBuf1	100 - 1ff
RingBuf2	200 - 2ff
EP2 Buf		300 - 33f
EP3 Buf 	380 - 3bf
*/

__xdata __at (0x0000) uint8_t  Ep0Buffer[DEFAULT_ENDP0_SIZE];	   //端点0 OUT&IN缓冲区，必须是偶地址
__xdata __at (0x0040) uint8_t  Ep4Buffer[MAX_PACKET_SIZE];	  //端点4 OUT接收缓冲区
__xdata __at (0x0080) uint8_t  Ep1Buffer[MAX_PACKET_SIZE];		//端点1 IN 发送缓冲区
__xdata __at (0x0300) uint8_t  Ep2Buffer[MAX_PACKET_SIZE * 2];	  //端点2 OUT接收缓冲区

__xdata __at (0x0380) uint8_t  Ep3Buffer[MAX_PACKET_SIZE];		//端点3 IN 发送缓冲区


__xdata __at (0x0100) uint8_t  RingBuf[128];
__xdata __at (0x0200) uint8_t  RingBuf_1[128];


uint16_t SetupLen;
uint8_t   SetupReq, Count, UsbConfig;
uint8_t   VendorControl;

__code uint8_t *  pDescr;													   //USB配置标志
uint8_t pDescr_Index = 0;
USB_SETUP_REQ   SetupReqBuf;												   //暂存Setup包
#define UsbSetupBuf	 ((PUSB_SETUP_REQ)Ep0Buffer)


/*设备描述符*/
__code uint8_t DevDesc[] = {0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, DEFAULT_ENDP0_SIZE,
							0x03, 0x04, 0x10, 0x60, 0x00, 0x05, 0x01, 0x02,
							0x03, 0x01
						   };
__code uint8_t CfgDesc[] =
{
	0x09, 0x02, sizeof(CfgDesc) & 0xff, sizeof(CfgDesc) >> 8,
	0x02, 0x01, 0x00, 0x80, 0x32,		 //配置描述符（1个接口）
	//以下为接口0（数据接口）描述符
	0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0x00,	 //数据接口描述符
	0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,				 //端点描述符 EP1 BULK IN
	0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,				 //端点描述符 EP2 BULK OUT
	//以下为接口01（数据接口）描述符
	0x09, 0x04, 0x01, 0x00, 0x02, 0xff, 0xff, 0xff, 0x00,	 //数据接口描述符
	0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,				 //端点描述符 EP3 BULK IN
	0x07, 0x05, 0x04, 0x02, 0x40, 0x00, 0x00,				 //端点描述符 EP4 BULK OUT
};
/*字符串描述符*/
unsigned char  __code LangDes[] = {0x04, 0x03, 0x09, 0x04};	  //语言描述符

unsigned char  __code Prod_Des[] =								//产品字符串描述符
{
	sizeof(Prod_Des), 0x03,
	'S', 0x00, 'i', 0x00, 'p', 0x00, 'e', 0x00, 'e', 0x00, 'd', 0x00,
	'-', 0x00, 'D', 0x00, 'e', 0x00, 'b', 0x00, 'u', 0x00, 'g', 0x00
};
unsigned char  __code Manuf_Des[] =
{
	sizeof(Manuf_Des), 0x03,
	'K', 0x00, 'o', 0x00, 'n', 0x00, 'g', 0x00, 'o', 0x00, 'u', 0x00,
	' ', 0x00, 'H', 0x00, 'i', 0x00, 'k', 0x00, 'a', 0x00, 'r', 0x00, 'i', 0x00
};

/* 下载控制 */
volatile __idata uint8_t USBOutLength = 0;
volatile __idata uint8_t USBOutPtr = 0;
volatile __idata uint8_t USBReceived = 0;

volatile __idata uint8_t USBRecvLen_A = 0;
volatile __idata uint8_t USBRecvLen_B = 0;
volatile __idata uint8_t USBRecvBuf = 0;
volatile __idata uint8_t Serial_Done = 0;
volatile __idata uint8_t USBBufState = 0;
volatile __idata uint8_t SerialSendBuf = 0;
volatile __idata uint8_t USB_Require_Data = 0;

volatile __idata uint8_t USBOutLength_1 = 0;
volatile __idata uint8_t USBOutPtr_1 = 0;
volatile __idata uint8_t USBReceived_1 = 0;
/* 上传控制 */
volatile __idata uint8_t UpPoint1_Busy = 0;   //上传端点是否忙标志
volatile __idata uint8_t UpPoint1_LenA = 2;
volatile __idata uint8_t UpPoint1_LenB = 2;

volatile __idata uint8_t UpPoint3_Busy = 0;   //上传端点是否忙标志

/* 杂项 */
volatile __idata uint16_t SOF_Count = 0;
volatile __idata uint8_t Latency_Timer = 4; //Latency Timer
volatile __idata uint8_t Latency_Timer1 = 4;
volatile __idata uint8_t Require_DFU = 0;

/* 流控 */
volatile __idata uint8_t soft_dtr = 0;
volatile __idata uint8_t soft_rts = 0;

volatile __idata uint8_t soft_dtr_1 = 0;
volatile __idata uint8_t soft_rts_1 = 0;

#define HARD_ESP_CTRL 1

#ifndef HARD_ESP_CTRL
volatile __idata uint8_t Esp_Boot_Chk = 0;
volatile __idata uint8_t Esp_Require_Reset = 0;
#endif

/*******************************************************************************
* Function Name  : USBDeviceCfg()
* Description	: USB设备模式配置
* Input		  : None
* Output		 : None
* Return		 : None
*******************************************************************************/
void USBDeviceCfg()
{
	USB_CTRL = 0x00;														   //清空USB控制寄存器
	USB_CTRL &= ~bUC_HOST_MODE;												//该位为选择设备模式
	USB_CTRL |=  bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;					//USB设备和内部上拉使能,在中断期间中断标志未清除前自动返回NAK
	USB_DEV_AD = 0x00;														 //设备地址初始化
	//	 USB_CTRL |= bUC_LOW_SPEED;
	//	 UDEV_CTRL |= bUD_LOW_SPEED;												//选择低速1.5M模式
	USB_CTRL &= ~bUC_LOW_SPEED;
	UDEV_CTRL &= ~bUD_LOW_SPEED;											 //选择全速12M模式，默认方式
	UDEV_CTRL = bUD_PD_DIS;  // 禁止DP/DM下拉电阻
	UDEV_CTRL |= bUD_PORT_EN;												  //使能物理端口
}

void Jump_to_BL()
{
	ES = 0;
	PS = 0;

	USB_INT_EN = 0;
	USB_CTRL = 0x06;
	//UDEV_CTRL = 0x80;

	mDelaymS(100);

	EA = 0;

	while(1)
	{
		__asm
		LJMP 0x3800
		__endasm;
	}
}
/*******************************************************************************
* Function Name  : USBDeviceIntCfg()
* Description	: USB设备模式中断初始化
* Input		  : None
* Output		 : None
* Return		 : None
*******************************************************************************/
void USBDeviceIntCfg()
{
	USB_INT_EN |= bUIE_SUSPEND;											   //使能设备挂起中断
	USB_INT_EN |= bUIE_TRANSFER;											  //使能USB传输完成中断
	USB_INT_EN |= bUIE_BUS_RST;											   //使能设备模式USB总线复位中断
	USB_INT_EN |= bUIE_DEV_SOF;													//打开SOF中断
	USB_INT_FG |= 0x1F;													   //清中断标志
	IE_USB = 1;															   //使能USB中断
	EA = 1;																   //允许单片机中断
}
/*******************************************************************************
* Function Name  : USBDeviceEndPointCfg()
* Description	: USB设备模式端点配置，模拟兼容HID设备，除了端点0的控制传输，还包括端点2批量上下传
* Input		  : None
* Output		 : None
* Return		 : None
*******************************************************************************/
void USBDeviceEndPointCfg()
{
	// TODO: Is casting the right thing here? What about endianness?
	UEP2_DMA = (uint16_t) Ep2Buffer;											//端点2 OUT接收数据传输地址
	UEP3_DMA = (uint16_t) Ep3Buffer;
	//UEP2_3_MOD = 0x48;															//端点2 单缓冲接收, 端点3单缓冲发送
	UEP2_3_MOD = 0x49;				//端点3单缓冲发送,端点2双缓冲接收

	UEP2_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK;									//端点2 自动翻转同步标志位，OUT返回ACK
	UEP3_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK; //端点3发送返回NAK

	//UEP4_DMA = (uint16_t) Ep4Buffer; //Ep4Buffer = Ep0Buffer + 64
	UEP1_DMA = (uint16_t) Ep1Buffer;										   //端点1 IN 发送数据传输地址
	UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;								 //端点1 自动翻转同步标志位，IN事务返回NAK
	UEP4_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK; //端点4接收返回ACK, 无法自动翻转
	UEP4_1_MOD = 0x48;														 //端点1 单缓冲发送, 端点4单缓冲接收

	UEP0_DMA = (uint16_t) Ep0Buffer;													  //端点0数据传输地址
	UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;								 //手动翻转，OUT事务返回ACK，IN事务返回NAK
}

__code uint8_t HexToAscTab[] = "0123456789ABCDEF";

void uuidcpy(__xdata uint8_t *dest, uint8_t index, uint8_t len) /* 使用UUID生成USB Serial Number */
{
	uint8_t i;
	uint8_t p = 0; /* UUID格式, 十位十六进制数 */
	__code uint8_t *puuid;
	for(i = index; i < (index + len); i++)
	{
		if(i == 0)
			dest[p++] = 22; //10 * 2 + 2
		else if(i == 1)
			dest[p++] = 0x03;
		else
		{
			if(i & 0x01) //奇数
			{
				dest[p++] = 0x00;
			}
			else
			{
				puuid = (__code uint8_t *) (0x3ffa + (i - 2) / 4);
				if(i & 0x02)
					dest[p++] = HexToAscTab[(*puuid) >> 4];
				else
					dest[p++] = HexToAscTab[(*puuid) & 0x0f];
			}
		}
	}
}

#define INTF1_DTR	TIN1
#define INTF1_RTS	TIN0

#define INTF2_DTR	TIN3
#define INTF2_RTS	TIN2

volatile __idata uint8_t DTR_State = 0;
volatile __idata uint8_t Modem_Count = 0;

/*******************************************************************************
* Function Name  : DeviceInterrupt()
* Description	: CH559USB中断处理函数
*******************************************************************************/
void DeviceInterrupt(void) __interrupt (INT_NO_USB)					   //USB中断服务程序,使用寄存器组1
{
	uint16_t len;
	uint16_t divisor;
	if ((USB_INT_ST & MASK_UIS_TOKEN) == UIS_TOKEN_SOF)
	{
#ifdef SOF_NO_TIMER
		SOF_Count ++;
		if(Modem_Count)
			Modem_Count --;
        if(Modem_Count == 1)
		{
			if(soft_dtr == 0 && soft_rts == 1)
			{
				INTF1_RTS = 1;
				INTF1_DTR = 0;
			}
			if(soft_dtr == 1 && soft_rts == 0)
			{
				INTF1_RTS = 0;
				INTF1_DTR = 1;
			}
			if(soft_dtr == soft_rts)
			{
				INTF1_DTR = 1;
				INTF1_RTS = 0;
				INTF1_RTS = 1;
			}
		}
#endif /* SOF_NO_TIMER */
	}
	if(UIF_TRANSFER)															//USB传输完成标志
	{
		switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
		{
		case UIS_TOKEN_IN | 1:												  //endpoint 1# 端点批量上传
			UEP1_T_LEN = 0;
			UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;		   //默认应答NAK
			UpPoint1_Busy = 0;												  //清除忙标志
			break;
		case UIS_TOKEN_OUT | 2:												 //endpoint 2# 端点批量下传
			if ( U_TOG_OK )													 // 不同步的数据包将丢弃
			{
				UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_NAK;	   //收到一包数据就NAK，主函数处理完，由主函数修改响应方式
				USBReceived = 1;
				if(UEP2_CTRL & bUEP_R_TOG)
				{
					USBRecvBuf = 0; //缓冲2
                    USBRecvLen_A = USB_RX_LEN;
				}
				else
				{
					USBRecvBuf = 1; //缓冲1
					USBRecvLen_B = USB_RX_LEN;
				}
				USB_Require_Data = 0;
			}
			break;
		case UIS_TOKEN_IN | 3:												  //endpoint 3# 端点批量上传
			UEP3_T_LEN = 0;
			UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;		   //默认应答NAK
			UpPoint3_Busy = 0;												  //清除忙标志
			break;
		case UIS_TOKEN_OUT | 4:												 //endpoint 4# 端点批量下传
			if ( U_TOG_OK )													 // 不同步的数据包将丢弃
			{
				UEP4_CTRL ^= bUEP_R_TOG;	//同步标志位翻转
				UEP4_CTRL = UEP4_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_NAK;	   //收到一包数据就NAK，主函数处理完，由主函数修改响应方式
				USBOutPtr_1 = 64; //TODO: Nasty Solution
				USBOutLength_1 = USB_RX_LEN + 64;
				USBReceived_1 = 1;
			}
			break;
		case UIS_TOKEN_SETUP | 0:												//SETUP事务
			len = USB_RX_LEN;
			if(len == (sizeof(USB_SETUP_REQ)))
			{
				SetupLen = ((uint16_t)UsbSetupBuf->wLengthH << 8) | (UsbSetupBuf->wLengthL);
				len = 0;													  // 默认为成功并且上传0长度
				VendorControl = 0;
				SetupReq = UsbSetupBuf->bRequest;
				if ( ( UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )//非标准请求
				{
					//TODO: 重写
					VendorControl = 1;
					if(UsbSetupBuf->bRequestType & USB_REQ_TYP_READ)
					{
						//读
						switch( SetupReq )
						{
						case 0x90: //READ EEPROM
							Ep0Buffer[0] = 0xff;
							Ep0Buffer[1] = 0xff;
							len = 2;
							break;
						case 0x0a:
							if(UsbSetupBuf->wIndexL == 2)
								Ep0Buffer[0] = Latency_Timer1;
							else
								Ep0Buffer[0] = Latency_Timer;
							len = 1;
							break;
						case 0x05:
							Ep0Buffer[0] = 0x01;
							Ep0Buffer[1] = 0x60;
							len = 2;
							break;
						default:
							len = 0xFF;	 /*命令不支持*/
							break;
						}
					}
					else
					{
						//写
						switch( SetupReq )
						{
						case 0x02:
						case 0x04:
						case 0x06:
						case 0x07:
						case 0x0b:
						case 0x92:
							len = 0;
							break;
						case 0x91: //WRITE EEPROM, FT_PROG动作,直接跳转BL
							Require_DFU = 1;
							len = 0;
							break;
						case 0x00:
							if(UsbSetupBuf->wIndexL == 1)
								UpPoint1_Busy = 0;
							if(UsbSetupBuf->wIndexL == 2)
							{
								UpPoint3_Busy = 0;
								UEP4_CTRL &= ~(bUEP_R_TOG);
							}
							len = 0;
							break;
						case 0x09: //SET LATENCY TIMER
							if(UsbSetupBuf->wIndexL == 1)
								Latency_Timer = UsbSetupBuf->wValueL;
							else
								Latency_Timer1 = UsbSetupBuf->wValueL;
							len = 0;
							break;
						case 0x03:
							//divisor = wValue
							U1SMOD = 1;
							PCON |= SMOD; //波特率加倍
							T2MOD |= bTMR_CLK; //最高计数时钟

							divisor = UsbSetupBuf->wValueL |
									  (UsbSetupBuf->wValueH << 8);
							divisor &= 0x3fff; //没法发生小数取整数部分，baudrate = 48M/16/divisor


							if(divisor == 0 || divisor == 1)
							{
								if(UsbSetupBuf->wIndexL == 1)
									TH1 = 0xff; //实在憋不出来1.5M
								else
									SBAUD1 = 0xff;
							}
							else
							{
								divisor = divisor / 2; //24M CPU时钟
								if(divisor > 256)
								{
									//TH1 = 0 - 13; //统统115200
									if(UsbSetupBuf->wIndexL == 1)
									{
										divisor /= 8;
										if(divisor > 256)
										{
											TH1 = 0 - 13;
										}
										else
										{
											PCON &= ~(SMOD);
											T2MOD &= ~(bTMR_CLK); //低波特率
											TH1 = 0 - divisor;
										}
									}
									else //intf2
									{
										divisor /= 2;
										if(divisor > 256)
										{
											SBAUD1 = 0 - 13;
										}
										else
										{
											U1SMOD = 0;
											SBAUD1 = 0 - divisor;
										}
									}
								}
								else
								{
									if(UsbSetupBuf->wIndexL == 1)
										TH1 = 0 - divisor;
									else //intf2
										SBAUD1 = 0 - divisor;
								}
							}
							len = 0;
							break;
						case 0x01: //MODEM Control
#if HARD_ESP_CTRL
							if(UsbSetupBuf->wIndexL == 1)
							{
								if(UsbSetupBuf->wValueH & 0x01)
								{
									if(UsbSetupBuf->wValueL & 0x01) //DTR
									{
										soft_dtr = 1;
										//INTF1_DTR = 0;
									}
									else
									{
										soft_dtr = 0;
										//INTF1_DTR = 1;
									}
								}
								if(UsbSetupBuf->wValueH & 0x02)
								{
									if(UsbSetupBuf->wValueL & 0x02) //RTS
									{
										soft_rts = 1;
										//INTF1_RTS = 0;
									}
									else
									{
										soft_rts = 0;
										//INTF1_RTS = 1;
									}
								}
								Modem_Count = 20;
								/*
								if(soft_dtr == soft_rts) // Taken from Open-EC
								{
									INTF1_DTR = 1;
									INTF1_RTS = 1;
								}
								if(soft_dtr == 1 && soft_rts == 0)
								{
									INTF1_RTS = 1;
									INTF1_DTR = 0;
								}
								if(soft_dtr == 0 && soft_rts == 1)
								{
									INTF1_RTS = 0;
									INTF1_DTR = 1;
								}*/
							}
							else //intf2
							{
								if(UsbSetupBuf->wValueH & 0x01)
								{
									if(UsbSetupBuf->wValueL & 0x01) //DTR
									{
										soft_dtr_1 = 1;
										//INTF2_DTR = 0;
									}
									else
									{
										soft_dtr_1 = 0;
										//INTF2_DTR = 1;
									}
								}
								if(UsbSetupBuf->wValueH & 0x02)
								{
									if(UsbSetupBuf->wValueL & 0x02) //RTS
									{
										soft_rts_1 = 1;
										//INTF2_RTS = 0;
									}
									else
									{
										soft_rts_1 = 0;
										//INTF2_RTS = 1;
									}

									if(soft_dtr_1 == 1 && soft_rts_1 == 1)
									{
										INTF2_DTR = 1;
										INTF2_RTS = 1;
									}
									if(soft_dtr_1 == 0 && soft_rts_1 == 0)
									{
										INTF2_DTR = 1;
										INTF2_RTS = 1;
									}
									if(soft_dtr_1 == 0 && soft_rts_1 == 1)
									{
										INTF2_DTR = 1;
										INTF2_RTS = 0;
									}
									if(soft_dtr_1 == 1 && soft_rts_1 == 0)
									{
										INTF2_DTR = 0;
										INTF2_RTS = 1;
									}

								}
							}
#else
							if(Esp_Require_Reset == 3)
							{
								CAP1 = 0;
								Esp_Require_Reset = 4;
							}
#endif
							len = 0;
							break;
						default:
							len = 0xFF;		 /*命令不支持*/
							break;
						}
					}

				}
				else															 //标准请求
				{
					switch(SetupReq)											 //请求码
					{
					case USB_GET_DESCRIPTOR:
						switch(UsbSetupBuf->wValueH)
						{
						case 1:													   //设备描述符
							pDescr = DevDesc;										 //把设备描述符送到要发送的缓冲区
							len = sizeof(DevDesc);
							break;
						case 2:														//配置描述符
							pDescr = CfgDesc;										  //把设备描述符送到要发送的缓冲区
							len = sizeof(CfgDesc);
							break;
						case 3:
							if(UsbSetupBuf->wValueL == 0)
							{
								pDescr = LangDes;
								len = sizeof(LangDes);
							}
							else if(UsbSetupBuf->wValueL == 1)
							{
								pDescr = Manuf_Des;
								len = sizeof(Manuf_Des);
							}
							else if(UsbSetupBuf->wValueL == 2)
							{
								pDescr = Prod_Des;
								len = sizeof(Prod_Des);
							}
							else
							{
								pDescr = (__code uint8_t *)0xffff;
								len = 22; /* 10位ASCII序列号 */
							}
							break;
						default:
							len = 0xff;												//不支持的命令或者出错
							break;
						}

						if ( SetupLen > len )
						{
							SetupLen = len;	//限制总长度
						}
						len = SetupLen >= DEFAULT_ENDP0_SIZE ? DEFAULT_ENDP0_SIZE : SetupLen;							//本次传输长度
						if(pDescr == (__code uint8_t *) 0xffff) /* 取序列号的话 */
						{
							uuidcpy(Ep0Buffer, 0, len);
						}
						else
						{
							memcpy(Ep0Buffer, pDescr, len);								//加载上传数据
						}
						SetupLen -= len;
						pDescr_Index = len;
						break;
					case USB_SET_ADDRESS:
						SetupLen = UsbSetupBuf->wValueL;							  //暂存USB设备地址
						break;
					case USB_GET_CONFIGURATION:
						Ep0Buffer[0] = UsbConfig;
						if ( SetupLen >= 1 )
						{
							len = 1;
						}
						break;
					case USB_SET_CONFIGURATION:
						UsbConfig = UsbSetupBuf->wValueL;
						break;
					case USB_GET_INTERFACE:
						break;
					case USB_CLEAR_FEATURE:											//Clear Feature
						if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_DEVICE )				  /* 清除设备 */
						{
							if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x01 )
							{
								if( CfgDesc[ 7 ] & 0x20 )
								{
									/* 唤醒 */
								}
								else
								{
									len = 0xFF;										/* 操作失败 */
								}
							}
							else
							{
								len = 0xFF;											/* 操作失败 */
							}
						}
						else if ( ( UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )// 端点
						{
							switch( UsbSetupBuf->wIndexL )
							{
							case 0x83:
								UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
								break;
							case 0x03:
								UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
								break;
							case 0x82:
								UEP2_CTRL = UEP2_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
								break;
							case 0x02:
								UEP2_CTRL = UEP2_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
								break;
							case 0x81:
								UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
								break;
							case 0x01:
								UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
								break;
							default:
								len = 0xFF;										 // 不支持的端点
								break;
							}
							UpPoint1_Busy = 0;
							UpPoint3_Busy = 0;
						}
						else
						{
							len = 0xFF;												// 不是端点不支持
						}
						break;
					case USB_SET_FEATURE:										  /* Set Feature */
						if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_DEVICE )				  /* 设置设备 */
						{
							if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x01 )
							{
								if( CfgDesc[ 7 ] & 0x20 )
								{
									/* 休眠 */
#ifdef DE_PRINTF
									printf( "suspend\n" );															 //睡眠状态

									while ( XBUS_AUX & bUART0_TX )
									{
										;	//等待发送完成
									}
#endif
									SAFE_MOD = 0x55;
									SAFE_MOD = 0xAA;
									WAKE_CTRL = bWAK_BY_USB | bWAK_RXD0_LO | bWAK_RXD1_LO;					  //USB或者RXD0/1有信号时可被唤醒
									PCON |= PD;																 //睡眠
									SAFE_MOD = 0x55;
									SAFE_MOD = 0xAA;
									WAKE_CTRL = 0x00;
								}
								else
								{
									len = 0xFF;										/* 操作失败 */
								}
							}
							else
							{
								len = 0xFF;											/* 操作失败 */
							}
						}
						else if( ( UsbSetupBuf->bRequestType & 0x1F ) == USB_REQ_RECIP_ENDP )			 /* 设置端点 */
						{
							if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x00 )
							{
								switch( ( ( uint16_t )UsbSetupBuf->wIndexH << 8 ) | UsbSetupBuf->wIndexL )
								{
								case 0x83:
									UEP3_CTRL = UEP3_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;/* 设置端点3 IN STALL */
									break;
								case 0x03:
									UEP3_CTRL = UEP3_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL;/* 设置端点3 OUT Stall */
									break;
								case 0x82:
									UEP2_CTRL = UEP2_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;/* 设置端点2 IN STALL */
									break;
								case 0x02:
									UEP2_CTRL = UEP2_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL;/* 设置端点2 OUT Stall */
									break;
								case 0x81:
									UEP1_CTRL = UEP1_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;/* 设置端点1 IN STALL */
									break;
								case 0x01:
									UEP1_CTRL = UEP1_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL;/* 设置端点1 OUT Stall */
								default:
									len = 0xFF;									/* 操作失败 */
									break;
								}
							}
							else
							{
								len = 0xFF;									  /* 操作失败 */
							}
						}
						else
						{
							len = 0xFF;										  /* 操作失败 */
						}
						break;
					case USB_GET_STATUS:
						Ep0Buffer[0] = 0x00;
						Ep0Buffer[1] = 0x00;
						if ( SetupLen >= 2 )
						{
							len = 2;
						}
						else
						{
							len = SetupLen;
						}
						break;
					default:
						len = 0xff;													//操作失败
						break;
					}
				}
			}
			else
			{
				len = 0xff;														 //包长度错误
			}
			if(len == 0xff)
			{
				SetupReq = 0xFF;
				UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;//STALL
			}
			else if(len <= DEFAULT_ENDP0_SIZE)													   //上传数据或者状态阶段返回0长度包
			{
				UEP0_T_LEN = len;
				UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//默认数据包是DATA1，返回应答ACK
			}
			else
			{
				UEP0_T_LEN = 0;  //虽然尚未到状态阶段，但是提前预置上传0长度数据包以防主机提前进入状态阶段
				UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//默认数据包是DATA1,返回应答ACK
			}
			break;
		case UIS_TOKEN_IN | 0:													  //endpoint0 IN
			switch(SetupReq)
			{
			case USB_GET_DESCRIPTOR:
				len = SetupLen >= DEFAULT_ENDP0_SIZE ? DEFAULT_ENDP0_SIZE : SetupLen;			  //本次传输长度
				if(pDescr == (__code uint8_t *)0xffff)
				{
					uuidcpy(Ep0Buffer, pDescr_Index, len);
				}
				else
				{
					memcpy( Ep0Buffer, pDescr + pDescr_Index, len );								   //加载上传数据
				}
				SetupLen -= len;
				pDescr_Index += len;
				UEP0_T_LEN = len;
				UEP0_CTRL ^= bUEP_T_TOG;											 //同步标志位翻转
				break;
			case USB_SET_ADDRESS:
				if(VendorControl == 0)
				{
					USB_DEV_AD = USB_DEV_AD & bUDA_GP_BIT | SetupLen;
					UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
				}
				break;
			default:
				UEP0_T_LEN = 0;													  //状态阶段完成中断或者是强制上传0长度数据包结束控制传输
				UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
				break;
			}
			break;
		case UIS_TOKEN_OUT | 0:  // endpoint0 OUT
			if(SetupReq == 0x22) //设置串口属性
			{

			}
			else
			{
				UEP0_T_LEN = 0;
				UEP0_CTRL |= UEP_R_RES_ACK | UEP_T_RES_NAK;  //状态阶段，对IN响应NAK
			}
			break;

		default:
			break;
		}
		UIF_TRANSFER = 0;														   //写0清空中断
	}
	if(UIF_BUS_RST)																 //设备模式USB总线复位中断
	{
#ifdef DE_PRINTF
		printf( "reset\n" );															 //睡眠状态
#endif
		UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
		UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
		UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
		USB_DEV_AD = 0x00;
		UIF_SUSPEND = 0;
		UIF_TRANSFER = 0;
		UIF_BUS_RST = 0;															 //清中断标志
		UsbConfig = 0;		  //清除配置值
		UpPoint1_Busy = 0;
		UpPoint3_Busy = 0;

		USBOutLength = 0;
		USBOutPtr = 0;
		USBReceived = 0;

		USBOutLength_1 = 0;
		USBOutPtr_1 = 0;
		USBReceived_1 = 0;

		UpPoint1_LenA = 2;
		UpPoint1_LenB = 2;

		USBRecvLen_A = 0;
		USBRecvLen_B = 0;
		USBRecvBuf = 0;
		SerialSendBuf = 0;
		USBBufState = 0;
		Serial_Done = 0;
		USB_Require_Data = 0;
	}
	if (UIF_SUSPEND)																 //USB总线挂起/唤醒完成
	{
		UIF_SUSPEND = 0;
		if ( USB_MIS_ST & bUMS_SUSPEND )											 //挂起
		{
#ifdef DE_PRINTF
			printf( "suspend\n" );															 //睡眠状态
#endif
			while ( XBUS_AUX & bUART0_TX )
			{
				;	//等待发送完成
			}
			SAFE_MOD = 0x55;
			SAFE_MOD = 0xAA;
			WAKE_CTRL = bWAK_BY_USB | bWAK_RXD0_LO | bWAK_RXD1_LO;					  //USB或者RXD0/1有信号时可被唤醒
			PCON |= PD;																 //睡眠
			SAFE_MOD = 0x55;
			SAFE_MOD = 0xAA;
			WAKE_CTRL = 0x00;
		}
	}
	else																			   //意外的中断,不可能发生的情况
	{
		USB_INT_FG = 0xFF;															 //清中断标志

	}
}

void SerialPort_Config()
{
	volatile uint32_t x;
	volatile uint8_t x2;

    P1_MOD_OC = 0x7f;
    P1_DIR_PU = 0xcc; //P1.4 P1.5 P1.0 P1.1开漏
    P3_MOD_OC = 0xfd;

	SM0 = 0;
	SM1 = 1;
	SM2 = 0;																   //串口0使用模式1
	//使用Timer1作为波特率发生器
	RCLK = 0;																  //UART0接收时钟
	TCLK = 0;																  //UART0发送时钟
	PCON |= SMOD;
	x = 10 * FREQ_SYS / 115200 / 16;									   //如果更改主频，注意x的值不要溢出
	x2 = x % 10;
	x /= 10;
	if ( x2 >= 5 ) x ++;													   //四舍五入

	TMOD = TMOD & ~ bT1_GATE & ~ bT1_CT & ~ MASK_T1_MOD | bT1_M1;			  //0X20，Timer1作为8位自动重载定时器
	T2MOD = T2MOD | bTMR_CLK | bT1_CLK;										//Timer1时钟选择
	TH1 = 0 - x;															   //12MHz晶振,buad/12为实际需设置波特率
	TR1 = 1;																   //启动定时器1
	TI = 0;
	REN = 1;																   //串口0接收使能
	ES = 1; //开串口中断
	PS = 1; //中断优先级最高

	//串口2配置
    SCON1 = 0x70; //8bit, fast, receive enable
	SBAUD1 = 0 - x;
	IE_UART1 = 1;
	IP_EX |= bIP_UART1;
}


/*******************************************************************************
* Function Name  : Uart0_ISR()
* Description	: 串口接收中断函数，实现循环缓冲接收
*******************************************************************************/

//Ring Buf

volatile __data uint8_t WritePtr = 0;
volatile __data uint8_t ReadPtr = 0;

volatile __data uint8_t WritePtr_1 = 0;
volatile __data uint8_t ReadPtr_1 = 0;
#ifndef HARD_ESP_CTRL
__code uint8_t ESP_Boot_Sequence[] =
{
	0x07, 0x07, 0x12, 0x20,
	0x55, 0x55, 0x55, 0x55,
	0x55, 0x55, 0x55, 0x55,
	0x55, 0x55, 0x55, 0x55,
	0x55, 0x55, 0x55, 0x55
};
#endif

#define FAST_RECEIVE

#ifndef FAST_RECEIVE /* 年久失修的代码,不要维护了 */
void Uart0_ISR(void) __interrupt (INT_NO_UART0) __using 1
{
	if(RI)   //收到数据
	{
		if((WritePtr + 1) % sizeof(RingBuf) != ReadPtr)
		{
			//环形缓冲写
			RingBuf[WritePtr++] = SBUF;
			WritePtr %= sizeof(RingBuf);
		}
		RI = 0;
	}
	if (TI)
	{
		if(USBOutPtr >= USBOutLength)
		{
			UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
			TI = 0;
		}
		else
		{
			uint8_t ch = Ep2Buffer[USBOutPtr];
			SBUF = ch;
			TI = 0;
#ifndef HARD_ESP_CTRL
			if(ESP_Boot_Sequence[Esp_Boot_Chk] == ch)
				Esp_Boot_Chk ++;
			else
				Esp_Boot_Chk = 0;

			if(Esp_Boot_Chk >= (sizeof(ESP_Boot_Sequence) - 1))
			{
				if(Esp_Require_Reset == 0)
					Esp_Require_Reset = 1;
				Esp_Boot_Chk = 0;
			}
#endif
			USBOutPtr++;
		}
	}

}
#else
//汇编接收数据，选择寄存器组1，DPTR1 1.5M~150kHz~160 cycles

void Uart0_ISR(void) __interrupt (INT_NO_UART0) __using 1 __naked
{
	__asm
	push psw ;2
	push a
	push dph
	push dpl

ReadFromSerial:
	jnb _RI, SendToSerial ;7

	mov a, _WritePtr ;2
	mov dpl, _ReadPtr

	inc a ;1
	anl dpl, #0x7f
	anl a, #0x7f ;2

	xrl a, dpl
	jz SendToSerial

	mov dph, #(_RingBuf >> 8) ;3
	mov dpl, _WritePtr ;3
	mov a, _SBUF ;2
	movx @dptr, a ;1

	inc _WritePtr ;1
	anl _WritePtr, #0x7f ;2


SendToSerial:
	clr _RI ;2

	jnb _TI, ISR_End

	clr c
	mov a, _USBOutPtr
	subb a, _USBOutLength
	jc SerialTx

UsbEpAck:
	mov _Serial_Done, #1
	sjmp Tx_End
SerialTx:
	mov dph, #(_Ep2Buffer >> 8)
	mov dpl, _USBOutPtr
	movx a, @dptr
	mov _SBUF, a
	inc _USBOutPtr

Tx_End:
	clr _TI

ISR_End:

	pop dpl
	pop dph
	pop a
	pop psw
	reti
	__endasm;
}

//汇编接收数据，选择寄存器组1，DPTR1 1.5M~150kHz~160 cycles
void Uart1_ISR(void) __interrupt (INT_NO_UART1) __using 1 __naked
{
	__asm
	push psw ;2
	push a
	push dph
	push dpl

ReadFromSerial_1:
	jnb _U1RI, SendToSerial_1 ;7

	mov a, _WritePtr_1 ;2
	mov dpl, _ReadPtr_1

	inc a ;1
	anl dpl, #0x7f
	anl a, #0x7f ;2

	xrl a, dpl
	jz SendToSerial_1

	mov dph, #(_RingBuf_1 >> 8) ;3
	mov dpl, _WritePtr_1 ;3
	mov a, _SBUF1 ;2
	movx @dptr, a ;1

	inc _WritePtr_1 ;1
	anl _WritePtr_1, #0x7f ;2


SendToSerial_1:
	clr _U1RI ;2

	jnb _U1TI, ISR_End_1

	clr c
	mov a, _USBOutPtr_1
	subb a, _USBOutLength_1
	jc SerialTx_1

UsbEpAck_1:
	anl	_UEP4_CTRL, #0xf3
	sjmp Tx_End_1
SerialTx_1:
	mov dph, #(_Ep4Buffer >> 8)
	mov dpl, _USBOutPtr_1
	movx a, @dptr
	mov _SBUF1, a
	inc _USBOutPtr_1

Tx_End_1:
	clr _U1TI

ISR_End_1:

	pop dpl
	pop dph
	pop a
	pop psw
	reti
	__endasm;
}
#endif

//#define FAST_COPY_2
//#define FAST_COPY_1
void CLKO_Enable(void) //打开T2输出
{
	#if 0
	ET2 = 0;
	T2CON = 0;
	T2MOD = 0;
	T2MOD |= bTMR_CLK | bT2_CLK | T2OE;
	RCAP2H = 0xff;
	RCAP2L = 0xfe;
	TH2 = 0xff;
	TL2 = 0xfe;
	TR2 = 1;

	P1_MOD_OC &= ~(0x01 << 4); //P1.4推挽输出
	P1_DIR_PU |= (0x01 << 4);

	P3_MOD_OC &= ~(0x01 << 2); //P3.2高阻
	P3_DIR_PU &= ~(0x01 << 2); 

	P3_MOD_OC &= ~(0x01 << 5); //P1.4高阻
	P3_DIR_PU &= ~(0x01 << 5); 

	PIN_FUNC |= bT2_PIN_X;
	#endif
	P3_MOD_OC &= ~(0x01 << 3);
	P3_DIR_PU &= ~(0x01 << 3); 
	P3_MOD_OC &= ~(0x01 << 5);
	P3_DIR_PU &= ~(0x01 << 5);

	P3_MOD_OC &= ~(0x01 << 4); //P3.4推挽输出
	P3_DIR_PU |= (0x01 << 4);

	PWM_CK_SE = 1;
	PWM_CTRL |= bPWM_CLR_ALL;
	PWM_CTRL &= ~bPWM_CLR_ALL;

	PIN_FUNC &= ~bPWM2_PIN_X;
	PWM_DATA2 = 128;
	PWM_CTRL |= bPWM2_OUT_EN;
	
}

//定义函数返回值
#ifndef  SUCCESS
#define  SUCCESS  0
#endif
#ifndef  FAIL
#define  FAIL    0xFF
#endif

//定义定时器起始
#ifndef  START
#define  START  1
#endif
#ifndef  STOP
#define  STOP    0
#endif

//CH554 Timer0时钟选择   
//bTMR_CLK同时影响Timer0&1&2,使用时要注意 (除定时使用标准时钟)            
#define mTimer0Clk12DivFsys( ) (T2MOD &= ~bT0_CLK)                          //定时器,时钟=Fsys/12 T0标准时钟
#define mTimer0ClkFsys( )      (T2MOD |= bTMR_CLK | bT0_CLK)                //定时器,时钟=Fsys
#define mTimer0Clk4DivFsys( )  (T2MOD &= ~bTMR_CLK;T2MOD |=  bT0_CLK)       //定时器,时钟=Fsys/4
#define mTimer0CountClk( )     (TMOD |= bT0_CT)                             //计数器,T0引脚的下降沿有效

//CH554 Timer0 开始(SS=1)/结束(SS=0)
#define mTimer0RunCTL( SS )    (TR0 = SS ? START : STOP)


#define mTimer1Clk12DivFsys( ) (T2MOD &= ~bT1_CLK)                          //定时器,时钟=Fsys/12  T1标准时钟
#define mTimer1ClkFsys( )      (T2MOD |= bTMR_CLK | bT1_CLK)                //定时器,时钟=Fsys
#define mTimer1Clk4DivFsys( )  (T2MOD &= ~bTMR_CLK;T2MOD |=  bT1_CLK)       //定时器,时钟=Fsys/4
#define mTimer1CountClk( )     (TMOD |= bT1_CT)                             //计数器,T0引脚的下降沿有效

//CH554 Timer1 开始(SS=1)/结束(SS=0)
#define mTimer1RunCTL( SS )    (TR1 = SS ? START : STOP)


#define mTimer2Clk12DivFsys( ) {T2MOD &= ~ bT2_CLK;C_T2 = 0;}      //定时器,时钟=Fsys/12 T2标准时钟
#define mTimer2ClkFsys( )      {T2MOD |= (bTMR_CLK | bT2_CLK);C_T2=0;}         //定时器,时钟=Fsys
#define mTimer2Clk4DivFsys( )  {T2MOD &= ~bTMR_CLK;T2MOD |=  bT2_CLK;C_T2 = 0;}//定时器,时钟=Fsys/4
#define mTimer2CountClk( )     {C_T2 = 1;}                                     //计数器,T2引脚的下降沿有效

//CH554 Timer2 开始(SS=1)/结束(SS=0)
#define mTimer2RunCTL( SS )    {TR2 = SS ? START : STOP;}
#define mTimer2OutCTL( )       (T2MOD |= T2OE)                               //T2输出  频率TF2/2   
#define CAP1Alter( )           (PIN_FUNC |= bT2_PIN_X;)                      //CAP1由P10 映射到P14
#define CAP2Alter( )           (PIN_FUNC |= bT2EX_PIN_X;)                    //CAP2由P11 映射RST

/*******************************************************************************
* Function Name  : mTimer_x_ModInit(uint8_t x ,uint8_t mode)
* Description    : CH554定时计数器x模式设置
* Input          : uint8_t mode,Timer模式选择
                   0：模式0，13位定时器，TLn的高3位无效
                   1：模式1，16位定时器
                   2：模式2，8位自动重装定时器
                   3：模式3，两个8位定时器  Timer0
                   3：模式3，Timer1停止		
                   uint8_t x 定时器  0 1 2
* Output         : None
* Return         : 成功  SUCCESS
                   失败  FAIL
*******************************************************************************/
uint8_t mTimer_x_ModInit(uint8_t x ,uint8_t mode);

/*******************************************************************************
* Function Name  : mTimer_x_SetData(uint8_t x,uint16_t dat)
* Description    : CH554Timer 
* Input          : uint16_t dat;定时器赋值
                   uint8_t x 定时器  0 1 2
* Output         : None
* Return         : None
*******************************************************************************/
void mTimer_x_SetData(uint8_t x,uint16_t dat);

/*******************************************************************************
* Function Name  : CAP2Init(uint8_t mode)
* Description    : CH554定时计数器2 T2EX引脚捕捉功能初始化
                   uint8_t mode,边沿捕捉模式选择
                   0:T2ex从下降沿到下一个下降沿
                   1:T2ex任意边沿之间
                   3:T2ex从上升沿到下一个上升沿
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
void CAP2Init(uint8_t mode);

/*******************************************************************************
* Function Name  : CAP1Init(uint8_t mode)
* Description    : CH554定时计数器2 T2引脚捕捉功能初始化T2
                   uint8_t mode,边沿捕捉模式选择
                   0:T2ex从下降沿到下一个下降沿
                   1:T2ex任意边沿之间
                   3:T2ex从上升沿到下一个上升沿
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
void CAP1Init(uint8_t mode);

/*******************************************************************************
* Function Name  : mTimer_x_ModInit(uint8_t x ,uint8_t mode)
* Description    : CH554定时计数器x模式设置
* Input          : uint8_t mode,Timer模式选择
                   0：模式0，13位定时器，TLn的高3位无效
                   1：模式1，16位定时器
                   2：模式2，8位自动重装定时器
                   3：模式3，两个8位定时器  Timer0
                   3：模式3，Timer1停止									 
* Output         : None
* Return         : 成功  SUCCESS
                   失败  FAIL
*******************************************************************************/
uint8_t mTimer_x_ModInit(uint8_t x ,uint8_t mode)
{
    if(x == 0){TMOD = TMOD & 0xf0 | mode;}
    else if(x == 1){TMOD = TMOD & 0x0f | (mode<<4);}
    else if(x == 2){RCLK = 0;TCLK = 0;CP_RL2 = 0;}                               //16位自动重载定时器
    else return FAIL;
    return SUCCESS;
}

/*******************************************************************************
* Function Name  : mTimer_x_SetData(uint8_t x,uint16_t dat)
* Description    : CH554Timer0 TH0和TL0赋值
* Input          : uint16_t dat;定时器赋值
* Output         : None
* Return         : None
*******************************************************************************/
void mTimer_x_SetData(uint8_t x,uint16_t dat)
{
    uint16_t tmp;
    tmp = 65536 - dat;	
		if(x == 0){TL0 = tmp & 0xff;TH0 = (tmp>>8) & 0xff;}
		else if(x == 1){TL1 = tmp & 0xff;TH1 = (tmp>>8) & 0xff;}
		else if(x == 2){
      RCAP2L = TL2 = tmp & 0xff;                                               //16位自动重载定时器
      RCAP2H = TH2 = (tmp>>8) & 0xff;
    }                                                 
}

/*******************************************************************************
* Function Name  : CAP2Init(uint8_t mode)
* Description    : CH554定时计数器2 T2EX引脚捕捉功能初始化
                   uint8_t mode,边沿捕捉模式选择
                   0:T2ex从下降沿到下一个下降沿
                   1:T2ex任意边沿之间
                   3:T2ex从上升沿到下一个上升沿
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
void CAP2Init(uint8_t mode)
{
    RCLK = 0;
    TCLK = 0;	
    C_T2  = 0;
    EXEN2 = 1; 
    CP_RL2 = 1;                                                                //启动T2ex的捕捉功能
    T2MOD |= mode << 2;                                                        //边沿捕捉模式选择
}

/*******************************************************************************
* Function Name  : CAP1Init(uint8_t mode)
* Description    : CH554定时计数器2 T2引脚捕捉功能初始化T2
                   uint8_t mode,边沿捕捉模式选择
                   0:T2ex从下降沿到下一个下降沿
                   1:T2ex任意边沿之间
                   3:T2ex从上升沿到下一个上升沿
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
void CAP1Init(uint8_t mode)
{
    RCLK = 0;
    TCLK = 0;
    CP_RL2 = 1;
    C_T2 = 0;
    T2MOD = T2MOD & ~T2OE | (mode << 2) | bT2_CAP1_EN;                         //使能T2引脚捕捉功能,边沿捕捉模式选择
}

/*******************************************************************************
* Function Name  : mTimer0Interrupt()
* Description    : CH554定时计数器0定时计数器中断处理函数
*******************************************************************************/
void mTimer0Interrupt(void) __interrupt (INT_NO_TMR0)                          //timer0中断服务程序
{
    mTimer_x_SetData(0,1000);                                                  //非自动重载方式需重新给TH0和TL0赋值,1MHz/1000=1000Hz, 1ms
    SOF_Count ++;
	if(Modem_Count)
		Modem_Count --;
    if(Modem_Count == 1)
    {
	    if(soft_dtr == 0 && soft_rts == 1)
		{
			INTF1_RTS = 1;
			INTF1_DTR = 0;
		}
		if(soft_dtr == 1 && soft_rts == 0)
		{
			INTF1_RTS = 0;
			INTF1_DTR = 1;
		}
		if(soft_dtr == soft_rts)
		{
			INTF1_DTR = 1;
			INTF1_RTS = 0;
			INTF1_RTS = 1;
		}
	}
}

void init_timer() {
    mTimer0Clk12DivFsys();	                                                   //T0定时器时钟设置,12MHz/12=1MHz
    mTimer_x_ModInit(0,1);                                                     //T0 定时器模式设置
    mTimer_x_SetData(0,1000);	                                               //T0定时器赋值,1MHz/1000=1000Hz, 1ms
    mTimer0RunCTL(1);                                                          //T0定时器启动	
    ET0 = 1;                                                                   //T0定时器中断开启		
    EA = 1;

	SOF_Count = 0;
}

//主函数
void main()
{
	uint8_t i;
	volatile uint16_t Uart_Timeout = 0;
	volatile uint16_t Uart_Timeout1 = 0;
	uint16_t Esp_Stage = 0;
	int8_t size;

	CfgFsys( );														   //CH559时钟选择配置
	mDelaymS(5);														  //修改主频等待内部晶振稳定,必加
	SerialPort_Config();
#ifdef DE_PRINTF
	printf("start ...\n");
#endif
	USBDeviceCfg();
	USBDeviceEndPointCfg();											   //端点配置
	USBDeviceIntCfg();													//中断初始化
	UEP0_T_LEN = 0;
	UEP1_T_LEN = 0;													   //预使用发送长度一定要清空
	UEP2_T_LEN = 0;													   //预使用发送长度一定要清空
	CLKO_Enable();
	/* 预先填充 Modem Status */
	Ep1Buffer[0] = 0x01;
	Ep1Buffer[1] = 0x60;
	Ep3Buffer[0] = 0x01;
	Ep3Buffer[1] = 0x60;
	XBUS_AUX = 0;
#ifndef SOF_NO_TIMER
	init_timer();                                                              // 每1ms SOF_Count加1
#endif 
	while(1)
	{
		if(UsbConfig)
		{
			if(UpPoint1_Busy == 0)
			{
				size = WritePtr - ReadPtr;
				if(size < 0) size = size + sizeof(RingBuf); //求余数

				if(size >= 62)
				{
					//i ~ r6, size ~ r7
#ifndef	FAST_COPY_1
					for(i = 0; i < 62; i++)
					{
						Ep1Buffer[2 + i] = RingBuf[ReadPtr++];
						ReadPtr %= sizeof(RingBuf);
					}
#else
__asm
	mov _XBUS_AUX, #1
	mov dph, #0x00
	mov dpl, #0x82
	dec _XBUS_AUX
	mov dph, #(_RingBuf >> 8)
	mov dpl, _ReadPtr

	mov r6, #62
114514$:

	movx a, @dptr
	inc _ReadPtr
	anl _ReadPtr, #0x7f
	mov dpl, _ReadPtr

	inc _XBUS_AUX
	movx @dptr, a
	inc dpl
	dec _XBUS_AUX

	djnz r6, 114514$
__endasm;
#endif
					UpPoint1_Busy = 1;
					UEP1_T_LEN = 64;
					UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;


				}
				else if((uint16_t) (SOF_Count - Uart_Timeout) >= Latency_Timer) //超时
				{
					Uart_Timeout = SOF_Count;
					if(size > 62) size = 62;
#ifndef	FAST_COPY_1
					for(i = 0; i < (uint8_t)size; i++)
					{
						Ep1Buffer[2 + i] = RingBuf[ReadPtr++];
						ReadPtr %= sizeof(RingBuf);
					}
#else
__asm
	mov _XBUS_AUX, #1
	mov dph, #0x00
	mov dpl, #0x82
	dec _XBUS_AUX
	mov dph, #(_RingBuf >> 8)
	mov dpl, _ReadPtr

	mov a, r7
	mov r6, a
1919810$:

	movx a, @dptr
	inc _ReadPtr
	anl _ReadPtr, #0x7f
	mov dpl, _ReadPtr

	inc _XBUS_AUX
	movx @dptr, a
	inc dpl
	dec _XBUS_AUX

	djnz r6, 1919810$
__endasm;
#endif
					UpPoint1_Busy = 1;
					UEP1_T_LEN = 2 + size;
					UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;			//应答ACK
				}
			}
#if 1 //IF2
			if(UpPoint3_Busy == 0)
			{
				size = WritePtr_1 - ReadPtr_1;
				if(size < 0) size = size + sizeof(RingBuf_1); //求余数

				if(size >= 62)
				{
#ifndef	FAST_COPY_2
					for(i = 0; i < 62; i++)
					{
						Ep3Buffer[2 + i] = RingBuf_1[ReadPtr_1++];
						ReadPtr_1 %= sizeof(RingBuf_1);
					}
#else
__asm
	mov _XBUS_AUX, #1
	mov dph, #0x03
	mov dpl, #0x82
	dec _XBUS_AUX
	mov dph, #(_RingBuf_1 >> 8)
	mov dpl, _ReadPtr_1

	mov r6, #62
8101919$:

	movx a, @dptr
	inc _ReadPtr_1
	anl _ReadPtr_1, #0x7f
	mov dpl, _ReadPtr_1

	inc _XBUS_AUX
	movx @dptr, a
	inc dpl
	dec _XBUS_AUX

	djnz r6, 8101919$
__endasm;
#endif
					UpPoint3_Busy = 1;
					UEP3_T_LEN = 64;
					UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;


				}
				else if((uint16_t) (SOF_Count - Uart_Timeout1) >= Latency_Timer1) //超时
				{
					Uart_Timeout1 = SOF_Count;
					if(size > 62) size = 62;
#ifndef	FAST_COPY_2
					for(i = 0; i < (uint8_t)size; i++)
					{
						Ep3Buffer[2 + i] = RingBuf_1[ReadPtr_1++];
						ReadPtr_1 %= sizeof(RingBuf_1);
					}

#else
__asm
	mov _XBUS_AUX, #1
	mov dph, #0x03
	mov dpl, #0x82
	dec _XBUS_AUX
	mov dph, #(_RingBuf_1 >> 8)
	mov dpl, _ReadPtr_1

	mov a, r7
	mov r6, a
114810$:

	movx a, @dptr
	inc _ReadPtr_1
	anl _ReadPtr_1, #0x7f
	mov dpl, _ReadPtr_1

	inc _XBUS_AUX
	movx @dptr, a
	inc dpl
	dec _XBUS_AUXUSBBufState

	djnz r6, 114810$
__endasm;
/*

	inc _XBUS_AUX
	movx @dptr, a
	inc dpl
	dec _XBUS_AUX

	.db #0xa5
*/
#endif
					UpPoint3_Busy = 1;
					UEP3_T_LEN = 2 + size;
					UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;			//应答ACK
				}
			}
#endif
			if(USBReceived) //IDLE状态
			{
				if(USBRecvBuf == 0)
				{
					USBBufState |= 0x01;
					if(Serial_Done == 0) //串口IDLE
					{
						Serial_Done = 2; //串口发送中
						SerialSendBuf = 0;
						EA = 0;
						USBOutPtr = 0;
						USBOutLength = USBRecvLen_A;
						EA = 1;
						TI = 1;
					}
					if((USBBufState & 0x02) == 0)
					{
						if(UEP2_CTRL & MASK_UEP_R_RES != UEP_R_RES_ACK)
							UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
					}
				}
				if(USBRecvBuf == 1)
				{
					USBBufState |= 0x02;
					if(Serial_Done == 0) //串口IDLE
					{
						Serial_Done = 2; //串口发送中
						SerialSendBuf = 1;
						EA = 0;
						USBOutPtr = 64;
						USBOutLength = USBRecvLen_B + 64;
						EA = 1;
						TI = 1;
					}
					if((USBBufState & 0x01) == 0)
					{
						if(UEP2_CTRL & MASK_UEP_R_RES != UEP_R_RES_ACK)
							UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
					}
				}
				USBReceived = 0;
			}
			if(Serial_Done == 1)
			{
                 if(SerialSendBuf == 0)
				 {
				 	if((USBBufState & 0x02) != 0) //B缓冲区有数据
					{
						Serial_Done = 2; //串口发送中
						SerialSendBuf = 1;
						EA = 0;
						USBOutPtr = 64;
						USBOutLength = USBRecvLen_B + 64;
						EA = 1;
						TI = 1;
					}
					USBBufState &= ~(0x01); //A缓冲区数据清除
				 }
				 if(SerialSendBuf == 1)
				 {
				 	if((USBBufState & 0x01) != 0) //A缓冲区有数据
					{
						Serial_Done = 2; //串口发送中
						SerialSendBuf = 0;
						EA = 0;
						USBOutPtr = 0;
						USBOutLength = USBRecvLen_A;
						EA = 1;
						TI = 1;
					}
				 	USBBufState &= ~(0x02);
				 }
				Serial_Done = 0;
				//if(UEP2_CTRL & MASK_UEP_R_RES != UEP_R_RES_ACK)
				UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
				//
			}
			/*
			if(Serial_Done == 2 && USB_Require_Data == 0)
			{
				if(SerialSendBuf == 0)
				{
					if((USBBufState & 0x02) == 0)
					{
						UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
					}
				}
				if(SerialSendBuf == 1)
				{
					if((USBBufState & 0x01) == 0)
					{
						UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_R_RES | UEP_R_RES_ACK;
					}
				}
			}
			*/
			if(USBReceived_1)
			{
				USBReceived_1 = 0;
				U1TI = 1;
			}

			if(Require_DFU)
			{
				Require_DFU = 0;
				Jump_to_BL();
			}
#ifndef HARD_ESP_CTRL
			if(Esp_Require_Reset == 1)
			{
				//if(TH1 == 13)
				//{
				Esp_Require_Reset = 2;
				Esp_Stage = SOF_Count;
				//}
				//else
				//{
				//	Esp_Require_Reset = 0;
				//}
			}

			if(Esp_Require_Reset == 2)
			{
				if((uint16_t)(SOF_Count - Esp_Stage) == 1)
				{
					TXD1 = 1; //IO0
					CAP1 = 0;
				}
				if((uint16_t)(SOF_Count - Esp_Stage) == 2)
				{
					TXD1 = 0;
					CAP1 = 1; //EN high
				}
				if((uint16_t)(SOF_Count - Esp_Stage) >= 3)
				{
					TXD1 = 1;
				}
				if((uint16_t)(SOF_Count - Esp_Stage) >= 1000)
				{
					Esp_Require_Reset = 3;
				}
			}
			if(Esp_Require_Reset == 4)
			{RingBuf
				Esp_Require_Reset = 0;
				CAP1 = 1;
			}
#endif



		}

#if 0
		if(UartByteCount)
			Uart_Timeout++;
		if(!UpPoint2_Busy)   //端点不繁忙（空闲后的第一包数据，只用作触发上传）
		{
			length = UartByteCount;
			if(length > 0)
			{
				if(length > 39 || Uart_Timeout > 100)
				{
					Uart_Timeout = 0;
					if(Uart_Output_Point + length > UART_REV_LEN)
						length = UART_REV_LEN - Uart_Output_Point;
					UartByteCount -= length;
					//写上传端点
					memcpy(Ep2Buffer + MAX_PACKET_SIZE, &Receive_Uart_Buf[Uart_Output_Point], length);
					Uart_Output_Point += length;
					if(Uart_Output_Point >= UART_REV_LEN)
						Uart_Output_Point = 0;
					UEP2_T_LEN = length;													//预使用发送长度一定要清空
					UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;			//应答ACK
					UpPoint2_Busy = 1;
				}
			}
		}
#endif
	}
}

