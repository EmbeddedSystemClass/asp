#include "includes.h"
#include "driver.h"
#include "usart6.h"
#include "kfifo.h"
#include "usart_timer.h"

/********************************************平台配置参数********************************************/
#define USART6_TXD_Pin				GPIO_Pin_6
#define USART6_TXD_Port				GPIOC
#define USART6_TXD_Pin_RCC		RCC_AHB1Periph_GPIOC

#define USART6_RXD_Pin				GPIO_Pin_7
#define USART6_RXD_Port				GPIOC
#define USART6_RXD_Pin_RCC		RCC_AHB1Periph_GPIOC

#define TX_POLL 0
#define TX_IRQ  1
#define TX_DMA  2

#define TX_MODE	 TX_IRQ
#define RX_MODE RX_LENGTH_MODE


#define MAX_RX_BUFF_SIZE 512  //必须定义为2的次方
#define MAX_TX_BUFF_SIZE 512  //必须定义为2的次方

#define SYS_TICK_MS 10

#define USART6_PUTCHAR(ch) ( USART6->DR = (ch & (uint16_t)0x01FF) )
#define USART6_GETCHAR()   (uint16_t)(USART6->DR & (uint16_t)0x01FF);
/****************************************************************************************************/

static s8 usart6_open(u32 lParam);
static s8 usart6_close(void);

static s32 usart6_read_packet(u8* buff, u32 buff_len);
static s32 usart6_read_length(u8* buff, u32 want_len);
static s32 usart6_read_keychar(u8* buff, u32 buff_len);
static s32 usart6_read(u8* buff, u32 buff_len);
static s32 usart6_write(u8* buff, u32 buff_len);
static s32 usart6_ioctl(u8 cmd, u32 arg);
static u8  usart6_hard_init(USART_InitTypeDef* desc);
static u8  usart6_hard_deinit(void);
static u8  usart6_dma_send(u8* buff, u32 buff_len);	
///////////////////////////////////////////////////
static u8 USART6_pDisc[] = "usart6";
static struct kfifo tx_fifo;
static struct kfifo rx_fifo;
static u8  gInit = 0;


/////////////可配置参数/////////////////
static s8	 key_char 		  = '0';
static u8	 rx_mode 			  = RX_PACKET_MODE;
static u32 rx_timeout_ticks  = 100;  //100tick = 1000ms
static u32 tx_timeout_ticks  = 100;  //100tick = 1000ms
static u32 rx_length 		  = 0;	//定长模式下需要接收的数据长度
static u32 N_char_timeout = 10; //10个字符接收时间作为超时时间
static u32 baudRate			  = 9600;
///////////////////////////////////////

static SYS_EVENT* rx_event = NULL;	//接收同步事件
static SYS_EVENT* tx_event = NULL;	//发送同步事件
static u32 TimeVal = 0;	//字符超时间隔计数器
static u8 rx_buff[MAX_RX_BUFF_SIZE];	//接收缓冲区
static u8 tx_buff[MAX_TX_BUFF_SIZE];	//发送缓冲区

static u8 rx_char;  //接收中断中临时存放接收到的数据
static u8 tx_char 	= 0;
static u8 rx_char_prev = 0x30;
static volatile u8 dmx_send_complete = 1;
static DEV_REG usart6_cfg = 			//设备注册信息表						
{
	CHAR_USART6,				//设备ID号
	0,  								//是否共享			0:不可共享使用, 1:可共享使用
	0, 									//对于共享设备最大打开次数
	1,									//最大读次数
	1,									//最大写次数
	USART6_pDisc,				//驱动描述			自定义
	20120001,						//驱动版本			自定义
	(u32*)usart6_open,	//设备打开函数指针
	(u32*)usart6_close, //设备关闭函数指针
	(u32*)usart6_read,	//字符读函数
	(u32*)usart6_write,	//字符写函数
	(u32*)usart6_ioctl	//控制函数
};
///////////////////////////////////////////////////	
static s8 usart6_open(u32 lParam)
{
	 if(gInit)
	 {
			return -1;
	 }		 
	 usart6_hard_init((USART_InitTypeDef*)lParam);
	 gInit = 1;
	 return 0;
}
static s8 usart6_close()
{
		usart6_hard_deinit();
		gInit = 0;
		return 0;
}
#if(RX_MODE==RX_PACKET_MODE)
//字符超时接收模式读取函数
static s32 usart6_read_packet(u8* buff, u32 buff_len)
{
	 u8 BoxErr = 0;				//邮箱接收错误标志
	 s32 ret = 0;
#if SYS_CRITICAL_METHOD == 3                      /* Allocate storage for CPU status register           */
	 SYS_CPU_SR	cpu_sr = 0;
#endif
	 if(kfifo_len(&rx_fifo) > 0)
	 {
				return kfifo_get(&rx_fifo,buff,buff_len);
	 }
	 SysMboxPend(rx_event, rx_timeout_ticks, &BoxErr);		//邮箱接收挂起
	
	 if(BoxErr != 0)
	 {
			return 0;
	 }

	SYS_ENTER_CRITICAL();
	ret = kfifo_get(&rx_fifo,buff,buff_len);
	SYS_EXIT_CRITICAL();
	 
	return ret;
}
#endif

/*
定长读取函数
如果在触发接收中断的时候此函数还未被调用，那么rx_length就没有办法得到初始化，就默认为0
*/
#if(RX_MODE==RX_LENGTH_MODE)
static s32 usart6_read_length(u8* buff, u32 want_len)
{
	 u8 BoxErr = 0;				//邮箱接收错误标志
	 s32 len   = 0;
#if SYS_CRITICAL_METHOD == 3                      /* Allocate storage for CPU status register           */
		SYS_CPU_SR	cpu_sr = 0;
#endif
	
	 SYS_ENTER_CRITICAL();
	 
	 len = kfifo_len(&rx_fifo);
	 if(len >= want_len)
	 {		  
			len = kfifo_get(&rx_fifo,buff,want_len);
		  SYS_EXIT_CRITICAL();
			return len;
	 }
	 
	 rx_length = want_len;
	 SYS_EXIT_CRITICAL();
	 

	 SysMboxPend(rx_event, rx_timeout_ticks, &BoxErr);		//邮箱接收挂起

	 if(BoxErr != 0)
	 {
			return 0;
	 }

	 SYS_ENTER_CRITICAL();
	 len = kfifo_get(&rx_fifo,buff,want_len);
	 SYS_EXIT_CRITICAL();
	 
	 return len;
}
#endif

#if(RX_MODE==RX_KEY_MODE)
static s32 usart6_read_keychar(u8* buff, u32 buff_len)
{
	 u8 BoxErr = 0;				//邮箱接收错误标志
	 SysMboxPend(rx_event, rx_timeout_ticks, &BoxErr);		//邮箱接收挂起

	 if(BoxErr != 0)
	 {
			return 0;
	 }
	 return kfifo_get(&rx_fifo,buff,buff_len);
		
}
#endif

static s32 usart6_read(u8* buff, u32 buff_len)
{
		s32 ret  = 0;
#if 0
		 switch(rx_mode)
		 {
				case 	RX_PACKET_MODE:
						ret = usart6_read_packet(buff, buff_len);
						break;
				case	RX_LENGTH_MODE:
						ret = usart6_read_length(buff, buff_len);
						break;
				case	RX_KEY_MODE:
						ret = usart6_read_keychar(buff, buff_len);
						break;
				default:
						ret = usart6_read_packet(buff, buff_len);
						break;
		 }
 #else
		#if(RX_MODE==RX_PACKET_MODE)
				ret = usart6_read_packet(buff, buff_len);
		 #elif(RX_MODE==RX_LENGTH_MODE)
				ret = usart6_read_length(buff, buff_len);
		 #elif(RX_MODE==RX_KEY_MODE)
				ret = usart6_read_keychar(buff, buff_len);	
		 #endif
 #endif

	return ret;	 
}
static s32 usart6_irq_send(u8* buff, u32 buff_len)
{
		u8 BoxErr  = 0;				//邮箱接收错误标志
		u8 tx_data = 0;

		if(buff_len == 0)
		{
				return 0;
		}
		kfifo_put(&tx_fifo,buff,buff_len);
		//有可能再次调用此函数的时候，发送fifo中有未发送完毕的数据
		while(USART_GetFlagStatus(USART6, USART_FLAG_TXE) == RESET); //如果连续调用此函数，等待发送数据寄存器为空,也就是等待上一次发送的数据已经发送完毕
		//while(USART_GetFlagStatus(USART6, USART_FLAG_TC) == RESET); //如果连续调用此函数，等待发送数据寄存器为空,也就是等待上一次发送的数据已经发送完毕

		if( 0 == kfifo_getc( &tx_fifo,&tx_data ) )
		{

			USART6_PUTCHAR(tx_data);
			USART_ITConfig(USART6, USART_IT_TXE, ENABLE);						//开启发送中断
		}
		SysMboxPend(tx_event,tx_timeout_ticks,&BoxErr);

		if(BoxErr != 0)
		{
			return 0;
		}
		return buff_len;			
}
static s32 usart6_write(u8* buff, u32 buff_len)
{
	#if(TX_MODE == TX_IRQ)
		return usart6_irq_send(buff,buff_len);
	#elif (TX_MODE == TX_DMA)
		return usart6_dma_send(buff,buff_len);
	#elif (TX_MODE == TX_POLL)
	{
		int i;
		for(i = 0; i < buff_len; i++)
		{
			USART6_PUTCHAR(buff[i]);
			//while(USART_GetFlagStatus(USART6, USART_FLAG_TC) == RESET); 
			while(USART_GetFlagStatus(USART6, USART_FLAG_TXE) == RESET);//大数据量必死,但是小数据量又必须用他
		}
		return buff_len;
	}
	#endif
}

static void usart6_flush(u8 type)
{
		if(type == 0)
		{
				kfifo_reset(&rx_fifo);
		}
		else if(type == 1)
		{
				kfifo_reset(&tx_fifo);
		}
		else
		{
			kfifo_reset(&rx_fifo);
			kfifo_reset(&tx_fifo);
		}
}
static void usart6_update_timeout(u32 baud,u32 nchar)
{
		if(nchar == 0) nchar = 5;
		TimeVal = ((1000000 / (baud / 10)) * nchar) / 20;		//接收超时量	秒/波特率/10*2个字/20us TIM3的计数频率
}
static s32 usart6_ioctl(u8 cmd, u32 arg)
{
	s32 val = 0;
	switch(cmd)
	{
		case CMD_SET_RX_TIMEOUT:
			rx_timeout_ticks =(arg/SYS_TICK_MS);
			break;
		case CMD_GET_RX_TIMEOUT:
			*(u32*)(arg) = rx_timeout_ticks * SYS_TICK_MS;
			break;
		case CMD_SET_RX_MODE:
			{
				
				if(arg != rx_mode)
				{
						usart6_flush(0);
						rx_mode = arg;
				}
			}
			
			break;
		case CMD_GET_RX_MODE:
			*(u8*)(arg) = rx_mode;
			break;
		case CMD_SET_KEY_CHAR:
			key_char = arg;
			break;
		case CMD_GET_KEY_CHAR:
			*(u8*)(arg) = key_char;
			break;
		case CMD_SET_N_CHAR_TIMEOUT:
			N_char_timeout = arg;
			usart6_update_timeout(baudRate,N_char_timeout);
			break;
		case CMD_GET_N_CHAR_TIMEOUT:
			*(u32*)(arg) = N_char_timeout;
			break;
		case CMD_GET_INPUT_BUF_SIZE:
			*(u32*)(arg) = kfifo_len(&rx_fifo);
			break;
		case CMD_FLUSH_INPUT:
			usart6_flush(0);
			break;
		case CMD_FLUSH_OUTPUT:
			usart6_flush(1);
			break;
		default:
			val = -1;
			break;
	}
	return val;
}

/*!
	 \brief 串口通信参数初始化
*/
static void usart6_commu_param_init(USART_InitTypeDef* desc)
{		
	USART_InitTypeDef UART_InitStructure;											//串口结构

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART6, ENABLE);		//串口分配时钟

	UART_InitStructure.USART_BaudRate            = desc->USART_BaudRate;											//波特率
	UART_InitStructure.USART_WordLength          = desc->USART_WordLength;								//数据位8bit
	UART_InitStructure.USART_StopBits            = desc->USART_StopBits;									//停止位个数
	UART_InitStructure.USART_Parity              = desc->USART_Parity ;									//不进行奇偶效验
	UART_InitStructure.USART_HardwareFlowControl = desc->USART_HardwareFlowControl;		//RTS和CTS使能(None不使用)
	UART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;			//发送和接收使能
	USART_Init(USART6, &UART_InitStructure);																					//初始化串口
}
/*!
	 \brief 串口GPIO引脚初始化
*/
void usart6_gpio_init(void)																			//串口引脚初始化
{
	GPIO_InitTypeDef GPIO_InitStructure;		//串口引脚结构
		
	//串口引脚分配时钟	
	RCC_AHB1PeriphClockCmd(USART6_TXD_Pin_RCC, ENABLE);												//打开TXD时钟
	RCC_AHB1PeriphClockCmd(USART6_RXD_Pin_RCC, ENABLE);												//打开RXD时钟
	
	//TXD引脚映射到USART6
	if(USART6_TXD_Pin == GPIO_Pin_6)
		GPIO_PinAFConfig(GPIOC, GPIO_PinSource6, GPIO_AF_USART6);		//Connect PXx to PC6
	else
		GPIO_PinAFConfig(GPIOG, GPIO_PinSource14, GPIO_AF_USART6);		//Connect PXx to PG14
	
	//RXD引脚映射到USART6
  if(USART6_RXD_Pin == GPIO_Pin_7)
		GPIO_PinAFConfig(GPIOC, GPIO_PinSource7, GPIO_AF_USART6);	//Connect PXx to PC7
	else
		GPIO_PinAFConfig(GPIOG, GPIO_PinSource9, GPIO_AF_USART6);		//Connect PXx to PG9

	//配置串口 Tx (PC.06) 为复用推挽输出
  GPIO_InitStructure.GPIO_Pin = USART6_TXD_Pin;			//串口发送引脚
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;			//轮流模式
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;	//转换频率
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;		//推挽输出
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;	//不作上下拉
  if(USART6_TXD_Pin == GPIO_Pin_6)
		GPIO_Init(GPIOC, &GPIO_InitStructure);						//初始化引脚
	else
		GPIO_Init(GPIOG, &GPIO_InitStructure);						//初始化引脚
    
	// 配置串口 Rx (PC.07) 为浮空输入
  GPIO_InitStructure.GPIO_Pin = USART6_RXD_Pin;				//串口接收引脚
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;				//轮流模式
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;		//转换频率
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;		//不作上下拉
	if(USART6_RXD_Pin == GPIO_Pin_7)
		GPIO_Init(GPIOC, &GPIO_InitStructure);						//初始化引脚
	else
		GPIO_Init(GPIOG, &GPIO_InitStructure);						//初始化引脚
}
/*!
	 \brief 串口中断初始化
*/
void usart6_nvic_init(void)
{
	NVIC_InitTypeDef NVIC_InitStructure; 											//中断控制器变量

	NVIC_InitStructure.NVIC_IRQChannel = USART6_IRQn;					//设置中断通道
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;	//主优先级设置
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;				//设置优先级
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;						//打开串口中断
	NVIC_Init(&NVIC_InitStructure);														//初始化中断向量表
}
#if(TX_MODE==TX_DMA)
u8 usart6_dmatxd_init(void)
{
	NVIC_InitTypeDef NVIC_InitStructure; 		//中断控制器变量
	DMA_InitTypeDef DMA_InitStructure;			//DMA结构

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);							//使能DMA1时钟
	DMA_DeInit(DMA2_Stream6);
	
	//DMA中断向量 DMA_Tx 配置
	NVIC_InitStructure.NVIC_IRQChannel = DMA2_Stream6_IRQn;						//设置DMA中断通道
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;					//主优先级设置
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;								//设置优先级
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;										//打开中断
	NVIC_Init(&NVIC_InitStructure); 

	//DMA配置
	DMA_DeInit(DMA2_Stream6);  		   																									//复位DMA1_Channel4通道为默认值
  DMA_InitStructure.DMA_Channel = DMA_Channel_5; 
	DMA_InitStructure.DMA_PeripheralBaseAddr = USART6_BASE + 4;												//DMA通道外设基地址
	if( (tx_buff == 0)	|| (	MAX_TX_BUFF_SIZE < 4))																							//没有设置发送缓冲区
		return 0;
	DMA_InitStructure.DMA_Memory0BaseAddr = (uint32_t)tx_buff;				//DMA通道存储器基地址
	DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;														//DMA目的地	(DMA_DIR_PeripheralSRC源单向传输)  双向传输
																															//初始化DMA失败
	DMA_InitStructure.DMA_BufferSize = MAX_TX_BUFF_SIZE;									//发送缓冲区大小
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;									//当前外设寄存器不变
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;														//当前存储寄存器增加
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;						//外设数据宽度为字节(8位)
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;										//存储器数据宽度为字节(8位)
	DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;																			//正常缓冲模式
	DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;														//DMA通道优先级非常高
	DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Disable;         
	DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;
	DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_Single;
	DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
	DMA_Init(DMA2_Stream6, &DMA_InitStructure);																				//根据上诉设置初始化DMA
	DMA_ITConfig(DMA2_Stream6, DMA_IT_TC, ENABLE);    																//开启DMA通道中断
	return 1;
}
#endif

static void usart6_event_init(void)
{
	if(rx_event == (SYS_EVENT*)0) 					//没有创建接收完成信号量
		rx_event = SysMboxCreate((void *)0);	//创建接收完成信号量

	#if(TX_MODE==TX_IRQ)
		if(tx_event == (SYS_EVENT*)0) 					//没有创建接收完成信号量
			tx_event = SysMboxCreate((void *)0);	//创建接收完成信号量
	#endif
}

static void usart6_fifo_init(void)
{
		kfifo_init(&rx_fifo,rx_buff,MAX_RX_BUFF_SIZE);
	
		#if(TX_MODE==TX_IRQ) //中断发送模式下才使用fifo
			kfifo_init(&tx_fifo,tx_buff,MAX_TX_BUFF_SIZE);
		#elif(TX_MODE==TX_DMA)

		#endif
			
}

static int check_data_lost(u8 ch)
{
	 if(ch == 0x30)
	 {
				if(rx_char_prev != 0x39)
				{
					  rx_char_prev = ch;
						return 1;
				}
	 }
	 else
	 {
			  if( (ch - rx_char_prev) != 1)
				{
					  rx_char_prev = ch;
					  return 1;
				}
	 }
	 rx_char_prev = ch;
	 return 0;
}


#if 0
void USART6_IRQHandler(void)
{
	u16 state;
	
	SysIntEnter();																					//任务优先级调度保护					
	
	//接收中断触发
	
	state = USART6->SR;
	if((state&0x20) != 0)
	{
		//数据接收动作
		rx_char = USART6_GETCHAR();
		#if 0
			if( check_data(rx_char) != 0)
			{
					rx_char = rx_char;
			}
		#endif
		kfifo_putc( &rx_fifo,  rx_char);

		#if(RX_MODE==RX_PACKET_MODE)		//包数据检测机制的话，复位超时计数器，等待下一个接收的字符
		{
					TIM3->CCR1 = TIM3->CNT + TimeVal;										//重新增加超时量

					//接收第一个数据时，开启超时中断，接收完成该中断会有定时中断程序关闭
					if((TIM3->DIER & TIM_FLAG_CC1) == 0)   							//如果比较通道使能为关闭
					{	 			
						TIM3->SR = (uint16_t)~TIM_FLAG_CC1;								//清空比较通道1的中断标志 
						TIM3->DIER |= TIM_IT_CC1;													//比较通道打开等待下一次中断
					}
		}
		#elif(RX_MODE==RX_LENGTH_MODE)
		 {
			  int len = kfifo_len(&rx_fifo);

				if( len  == rx_length )
				{
					  rx_length = 0;
						SysMboxPost(rx_event, (void *)(&rx_char));	//发送激活等待的邮箱信号  
				}
				else if(len >= MAX_RX_BUFF_SIZE)
				{

					  SysMboxPost(rx_event, (void *)(&rx_char));	//发送激活等待的邮箱信号  
				}
		 }
		 #elif(RX_MODE==RX_KEY_MODE)
		 {
				if(rx_char == key_char)
				{
					SysMboxPost(rx_event, (void *)&rx_char);	//发送激活等待的邮箱信号
					
				}
		 }
		 #endif

	}
	
	
	else
	{
		state = USART6->SR;
		if( (state&0x08) != 0 )
		{
				USART6_GETCHAR();																//清接收溢出标志，只能用读数据的方式来清溢出标志
		}
	}
	//发送中断触发

	state = USART6->SR;
	if(( state&0x80 ) != 0 )
	{		

			if(-1 == kfifo_getc(&tx_fifo,&ch)) //fifo中没有数据了
			{	
				//禁止发送中断
				
				USART_ITConfig(USART6, USART_IT_TXE, DISABLE);
				if(tx_event)
				{
					  SysMboxPost(tx_event, (void *)&tx_char);	//发送激活等待的邮箱信号
				}
			}
			else
			{
				//还有数据，继续发送fifo中的数据
				USART6_PUTCHAR(ch);
			}		
	
	}

	
	SysIntExit();   																						//任务优先级调度保护解除
}
#else
void USART6_IRQHandler(void)
{

	SysIntEnter();																					//任务优先级调度保护					
	
	//接收中断触发

	if(USART_GetITStatus(USART6, USART_IT_RXNE) == SET)
	{
		//数据接收动作
		rx_char = USART6_GETCHAR();
#if 0
		if( check_data_lost(rx_char) != 0)
		{
			  rx_char = rx_char;
		}
#endif
		kfifo_putc( &rx_fifo,  rx_char);

		#if(RX_MODE==RX_PACKET_MODE)		//包数据检测机制的话，复位超时计数器，等待下一个接收的字符
		{
					TIM3->CCR2 = TIM3->CNT + TimeVal;										//重新增加超时量

					//接收第一个数据时，开启超时中断，接收完成该中断会有定时中断程序关闭
					if((TIM3->DIER & TIM_FLAG_CC2) == 0)   							//如果比较通道使能为关闭
					{	 			
						TIM3->SR = (uint16_t)~TIM_FLAG_CC2;								//清空比较通道1的中断标志 
						TIM3->DIER |= TIM_IT_CC2;													//比较通道打开等待下一次中断
					}
		}
		#elif(RX_MODE==RX_LENGTH_MODE)
		 {
			  int len = kfifo_len(&rx_fifo);

				if( len  == rx_length )
				{
					  rx_length = 0;
						SysMboxPost(rx_event, (void *)(&rx_char));	//发送激活等待的邮箱信号  
				}
				else if(len >= MAX_RX_BUFF_SIZE)
				{

					  SysMboxPost(rx_event, (void *)(&rx_char));	//发送激活等待的邮箱信号  
				}
		 }
		 #elif(RX_MODE==RX_KEY_MODE)
		 {
				if(rx_char == key_char)
				{
					SysMboxPost(rx_event, (void *)&rx_char);	//发送激活等待的邮箱信号
					
				}
		 }
		 #endif

	}
	//当接收移位寄存器中收完了数据，准备将数据往DR寄存器中放的时候，发现RNXE标志没有被清空，就会触发此中断
	
	else if(USART_GetFlagStatus(USART6, USART_IT_ORE) == SET)		//检测是否有接收溢出
  {
			USART6_GETCHAR();																//清接收溢出标志，只能用读数据的方式来清溢出标志
  }
	
	//发送中断触发

	if(USART_GetITStatus(USART6, USART_IT_TXE) == SET)					//发送中断
	{		

			if(-1 == kfifo_getc(&tx_fifo,&tx_char)) //fifo中没有数据了
			{	
				//禁止发送中断
				
				USART_ITConfig(USART6, USART_IT_TXE, DISABLE);
				if(tx_event)
				{
					  SysMboxPost(tx_event, (void *)&tx_char);	//发送激活等待的邮箱信号
				}
			}
			else
			{
					//还有数据，继续发送fifo中的数据
					USART6_PUTCHAR(tx_char);
			}		
	
	}

	
	SysIntExit();   																						//任务优先级调度保护解除
}
#endif

static u8 usart6_dma_send(u8* buff, u32 buff_len)	
{
	u8 BoxErr = 0;
	s32 timeout = tx_timeout_ticks;
	if(buff_len > MAX_TX_BUFF_SIZE) return 0;
	if(buff_len == 0) return 0;
	
	while( (dmx_send_complete == 0) && timeout > 0)
	{
			SysTimeDly(1);
			--timeout;
	}
	memcpy(tx_buff,buff,buff_len);
  DMA2_Stream6->NDTR = buff_len;					   						//发送字节数量
	dmx_send_complete = 0;
	USART_DMACmd(USART6, USART_DMAReq_Tx, ENABLE); 	//开启
	DMA_Cmd(DMA2_Stream6, ENABLE);									//始能DMA通道
	
	SysMboxPend(tx_event,tx_timeout_ticks,&BoxErr);
	
	if(BoxErr != 0)
	{
		return 0;
	}
	if(dmx_send_complete != 1)
	{
		
	}
	return buff_len;	

}

#if(TX_MODE==TX_DMA)
void DMA2_Stream6_IRQHandler(void)
{
	SysIntEnter();	
  
	if(DMA_GetFlagStatus(DMA2_Stream7,DMA_FLAG_TCIF7))    //DMA发送完成中断
	{
				
		USART_DMACmd(USART6, USART_DMAReq_Tx, DISABLE);     //关闭DMA发送
		DMA_Cmd(DMA2_Stream7, DISABLE);	                    //关闭DMA通道 

		dmx_send_complete = 1;
		if(tx_event != 0)											//开启了发送完成信号量通知
			SysMboxPost(tx_event, (void *)&tx_char);	//发送激活等待的邮箱信号
		
	}
	
	DMA_ClearFlag(DMA2_Stream7,DMA_FLAG_FEIF7 | DMA_FLAG_DMEIF7 | DMA_FLAG_TEIF7 | DMA_FLAG_HTIF7 | DMA_FLAG_TCIF7);  //Modify
	
	SysIntExit();
}
#endif

void usart6_timer_isr(void)
{
	#if (RX_MODE == RX_PACKET_MODE)
		if(rx_event != 0)
		{
			 SysMboxPost(rx_event, (void *)&rx_char);	//发送激活等待的邮箱信号
		}
	#endif
}

s8 usart6_init(void)
{
	if(DeviceInstall(&usart6_cfg) != HVL_NO_ERR)			//设备注册
		return -1;
	return 0;
}																								//启用定时器计算包接收超时


static u8 usart6_hard_deinit()
{
	USART_Cmd(USART6, DISABLE); 
	USART_ClearITPendingBit(USART6,USART_IT_RXNE);//清接收标志
	USART_ITConfig(USART6,USART_IT_RXNE, DISABLE);	//开启接收中断
	RCC_AHB1PeriphClockCmd(USART6_TXD_Pin_RCC, DISABLE);						//关闭TXD时钟
	RCC_AHB1PeriphClockCmd(USART6_RXD_Pin_RCC, DISABLE);						//关闭RXD时钟
	return 0;
}
static u8 usart6_hard_init(USART_InitTypeDef* desc)
{
#if (RX_MODE == RX_PACKET_MODE)
	usart_timer_init();																									//启用定时器计算包接收超时
#endif
	baudRate = desc->USART_BaudRate;
  usart6_update_timeout(baudRate,N_char_timeout);
	USART_Cmd(USART6, DISABLE);  																		//使能失能串口外设
	usart6_commu_param_init(desc);												//串口初始化
	usart6_gpio_init();																							//串口引脚初始化	
	
	usart6_event_init();
	usart6_fifo_init();
	
	usart6_nvic_init();	
	USART_ClearITPendingBit(USART6,USART_IT_RXNE);//清接收标志
	USART_ClearITPendingBit(USART6,USART_IT_TXE);
	USART_ClearITPendingBit(USART6,USART_IT_TC);
	#if(TX_MODE==TX_IRQ)

	#elif(TX_MODE==TX_DMA)
		usart6_dmatxd_init();
	#endif
	
	USART_ITConfig(USART6,USART_IT_RXNE, ENABLE);	//开启接收中断
	USART_Cmd(USART6, ENABLE);  									//使能失能串口外设
	
	return 0;
}
