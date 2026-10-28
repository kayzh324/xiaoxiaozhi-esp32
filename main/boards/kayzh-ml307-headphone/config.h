#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BUILTIN_LED_GPIO GPIO_NUM_48

// 五向导航按钮
// 模块在机壳内顺时针旋转了 90° 安装，所以按“物理方向”重映射：
//   模块自己的 L 脚 → 物理上(F)、模块 F 脚 → 物理右(R)、模块 R 脚 → 物理下(B)、模块 B 脚 → 物理左(L)
// 下面常量一律表示“用户看到的物理方向”，不是模块丝印方向。改线时按此对应重新核对。
#define M_BUTTON_GPIO        GPIO_NUM_0
#define F_BUTTON_GPIO        GPIO_NUM_2   // 模块 L
#define B_BUTTON_GPIO        GPIO_NUM_1   // 模块 R
#define R_BUTTON_GPIO       GPIO_NUM_21   // 模块 F
#define L_BUTTON_GPIO       GPIO_NUM_47   // 模块 B

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
