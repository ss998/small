/*
    ChibiOS - Copyright (C) 2006..2016 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "ch.h"
#include "hal.h"

#include "board.h"
#include "shell.h"
#include "chprintf.h"

#include "w5500.h"
#include "mqtt_interface.h"
#include "MQTTClient.h"

#define WIZ_SPIx SPID1
#define TCP_SOCKET 0
#define BUFFER_SIZE 2048

BaseSequentialStream *serial = (BaseSequentialStream*) &SD1;

struct opts_struct
{
	char* clientid;
	int nodelimiter;
	char* delimiter;
	enum QoS qos;
	char* username;
	char* password;
	unsigned char host[4];
	int port;
	int showtopics;
}opts = { (char*)"wiznet-mqtt", 0, (char*)"\n", QOS0, NULL, NULL, {10, 104, 15, 84}, 1883, 0 };

unsigned char tempBuffer[BUFFER_SIZE] = {};
unsigned char buf[100];

wiz_NetInfo wiz_netinfo = { .mac = {0x0C, 0x29, 0xAB, 0x7C, 0x00, 0x01},
                            .ip = {192, 168, 1, 199},
                            .sn = {255, 255, 255, 0},
                            .gw = {192, 168, 1, 1},
                            .dns = {0, 0, 0, 0},
                            .dhcp = NETINFO_STATIC };

/*===========================================================================*/
/* Command line related.                                                     */
/*===========================================================================*/

#define SHELL_WA_SIZE   THD_WORKING_AREA_SIZE(2048)

static void cmd_gpio(BaseSequentialStream *chp, int argc , char *argv[]) {
  (void)argv;
  const char *usage = "gpio a3 1";
  if (argc < 2) {
    shellUsage(chp, usage);
    return;
  }
}

static const ShellCommand commands[] = {
  {"gpio", cmd_gpio, "gpio status."},
  {NULL, NULL, NULL}
};

static const ShellConfig shell_cfg1 = {
  (BaseSequentialStream *)&SD1,
  commands
};

/*
 * Maximum speed SPI configuration (18MHz, CPHA=0, CPOL=0, MSb first).
 */
static const SPIConfig spicfg = {
  NULL,
  GPIOA,
  GPIOA_SPI1_NSS,
  // SPI_CR1_CPHA | SPI_CR1_CPOL | SPI_CR1_MSTR | SPI_CR1_BR_0 | SPI_CR1_SSM,
  SPI_CR1_CPHA | SPI_CR1_CPOL | SPI_CR1_BR_0,
  0
};

void spiCrisEnter(void) {
  __set_PRIMASK(1);
}

void spiCrisExit(void) {
  __set_PRIMASK(0);
}

void spiCsSelect(void) {
  spiSelect(&WIZ_SPIx);
}

void spiCsUnSelect(void) {
  spiUnselect(&WIZ_SPIx);
}

uint8_t spiReadByte(void) {
  while((WIZ_SPIx.spi->SR & ((uint16_t)0x0002)) == 0);
  WIZ_SPIx.spi->DR = 0xFF;
  while((WIZ_SPIx.spi->SR & ((uint16_t)0x0001)) == 0);
  return WIZ_SPIx.spi->DR;
}

void spiWriteByte(uint8_t data) {
	while((WIZ_SPIx.spi->SR & ((uint16_t)0x0002)) == 0);
	WIZ_SPIx.spi->DR = data;
	while((WIZ_SPIx.spi->SR & ((uint16_t)0x0001)) == 0);
	WIZ_SPIx.spi->DR;
}

void messageArrived(MessageData* md)
{
	unsigned char testbuffer[100];
	MQTTMessage* message = md->message;

	if (opts.showtopics)
	{
		memcpy(testbuffer,(char*)message->payload,(int)message->payloadlen);
		*(testbuffer + (int)message->payloadlen + 1) = '\n';
		chprintf(serial, "%s\r\n",testbuffer);
	}

	if (opts.nodelimiter)
		chprintf(serial, "%.*s", (int)message->payloadlen, (char*)message->payload);
	else
		chprintf(serial, "%.*s%s", (int)message->payloadlen, (char*)message->payload, opts.delimiter);
}

void w5500Init(void)
{
	uint8_t wiznet_phylink;
	uint8_t wiznet_arg[2][8] = {{2,2,2,2,2,2,2,2},{2,2,2,2,2,2,2,2}};

  reg_wizchip_cris_cbfunc(spiCrisEnter, spiCrisExit);
  reg_wizchip_cs_cbfunc(spiCsSelect, spiCsUnSelect);
  reg_wizchip_spi_cbfunc(spiReadByte, spiWriteByte);

  if(ctlwizchip(CW_INIT_WIZCHIP, (void*)wiznet_arg) == -1) {
    chprintf(serial, "WIZCHIP Initialized fail.\r\n");
	}

	do{
		 if(ctlwizchip(CW_GET_PHYLINK, (void*)&wiznet_phylink) == -1){
				chprintf(serial, "Unknown PHY Link status.\r\n");
		 }
	}while(wiznet_phylink == PHY_LINK_OFF);
  
  ctlnetwork(CN_SET_NETINFO, (void*)&wiz_netinfo);

  setSHAR(wiz_netinfo.mac);
}

static THD_WORKING_AREA(SPI_THREAD_WA, 256);
static THD_FUNCTION(spiThread, p) {

  Network n;
  MQTTClient c;

  int rc = 0;
  
  (void)p;
  chRegSetThreadName("w55-mqtt");

  NewNetwork(&n, TCP_SOCKET);
  ConnectNetwork(&n, opts.host, opts.port);
  MQTTClientInit(&c,&n,1000,buf,100,tempBuffer,2048);

	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	data.willFlag = 0;
	data.MQTTVersion = 3;
	data.clientID.cstring = opts.clientid;
	data.username.cstring = opts.username;
	data.password.cstring = opts.password;

	data.keepAliveInterval = 60;
	data.cleansession = 1;

	rc = MQTTConnect(&c, &data);
	chprintf(serial, "MQTT Connected %d\r\n", rc);
	opts.showtopics = 1;

	chprintf(serial, "MQTT Subscribing to %s\r\n", "hello/wiznet");
	rc = MQTTSubscribe(&c, "hello/wiznet", opts.qos, messageArrived);
	chprintf(serial, "MQTT Subscribed %d\r\n", rc);

  while(TRUE) {
    MQTTYield(&c, data.keepAliveInterval);
    chThdSleepMilliseconds(500);
  }
}

int main(void) {
  /*
   * System initializations.
   * - HAL initialization, this also initializes the configured device drivers
   *   and performs the board-specific initializations.
   * - Kernel initialization, the main() function becomes a thread and the
   *   RTOS is active.
   */
  halInit();
  chSysInit();

  SysTick_Config(72000);

  /*
   * USART1 I/O pins setup.
   */
  palSetPadMode(GPIOA, GPIOA_USART1_TX, PAL_MODE_STM32_ALTERNATE_PUSHPULL);
  palSetPadMode(GPIOA, GPIOA_USART1_RX, PAL_MODE_INPUT);  

  /*
   * Shell manager initialization.
   */
  sdStart(&SD1, NULL);
  chprintf(serial, "\r\nSmall Remote System\r\n");
  shellInit();

  /*
   * SPI1 I/O pins setup.
   */
  palSetPadMode(GPIOA, GPIOA_SPI1_SCK, PAL_MODE_STM32_ALTERNATE_PUSHPULL);    /* SCK. */
  palSetPadMode(GPIOA, GPIOA_SPI1_MISO, PAL_MODE_STM32_ALTERNATE_PUSHPULL);   /* MISO.*/
  palSetPadMode(GPIOA, GPIOA_SPI1_MOSI, PAL_MODE_STM32_ALTERNATE_PUSHPULL);   /* MOSI.*/
  palSetPadMode(GPIOA, GPIOA_SPI1_NSS, PAL_MODE_OUTPUT_PUSHPULL);             /* NSS. */
  palSetPadMode(GPIOC, GPIOC_W5500_RST, PAL_MODE_OUTPUT_PUSHPULL);              /* RESET. */
  palSetPad(GPIOA, GPIOA_SPI1_NSS);

  /*
   * SPI1 module initialization.
   */
  spiStart(&WIZ_SPIx, &spicfg);

  /*
   * Hardware reset W5500 chip.
   */
  palClearPad(GPIOC, GPIOC_W5500_RST);
  chThdSleepMilliseconds(50);
  palSetPad(GPIOC, GPIOC_W5500_RST);
  chThdSleepMilliseconds(200);

  /*
   * W5500 chip initialization
   */
  w5500Init();

  chThdCreateStatic(SPI_THREAD_WA, sizeof(SPI_THREAD_WA), NORMALPRIO + 1, spiThread, NULL);

  /*
   * Shell create thread.
   */
  chThdCreateFromHeap(NULL, SHELL_WA_SIZE, "shell", NORMALPRIO, shellThread, (void *)&shell_cfg1);
  
  /*
   * Normal main() thread activity, in this demo it does nothing.
   */
  while (TRUE) {
    chThdSleepMilliseconds(1000);
  }
  return 0;
}
