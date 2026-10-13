#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BUILTIN_LED_GPIO GPIO_NUM_48

// 五向导航按钮
#define M_BUTTON_GPIO        GPIO_NUM_0
#define F_BUTTON_GPIO       GPIO_NUM_21
#define B_BUTTON_GPIO       GPIO_NUM_47
#define R_BUTTON_GPIO       GPIO_NUM_1
#define L_BUTTON_GPIO       GPIO_NUM_2

// ST7789 170x320 SPI 屏，横屏使用（320 宽 x 170 高）
#define DISPLAY_MOSI_PIN           GPIO_NUM_11
#define DISPLAY_CLK_PIN           GPIO_NUM_12
#define DISPLAY_CS_PIN            GPIO_NUM_10
#define DISPLAY_DC_PIN            GPIO_NUM_8
#define DISPLAY_RST_PIN           GPIO_NUM_7
#define DISPLAY_BACKLIGHT_PIN     GPIO_NUM_6

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  170
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY true
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
// 横屏（MADCTL MV=1）下原始 35 列偏移转移到纵向轴
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  35
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0

#define ML307_RX_PIN GPIO_NUM_5
#define ML307_TX_PIN GPIO_NUM_4

// A MCP Test: Control a lamp
#define LAMP_GPIO GPIO_NUM_18

#endif // _BOARD_CONFIG_H_
