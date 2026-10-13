#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/ab13x_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "dual_network_board.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_reset.h"

#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#define TAG "KayzhML307Headphone"

class KayzhML307Headphone : public DualNetworkBoard {
private:
    Display* display_ = nullptr;
    Button m_button_;
    Button f_button_;
    Button b_button_;
    Button r_button_;
    Button l_button_;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 调整音量并在屏幕上显示当前值
    void AdjustVolume(int delta) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void InitializeButtons() {
        // 中键：单击切换对话状态；双击切换 WiFi/ML307；长按恢复音量
        m_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                    return;
                }
            }
            app.ToggleChatState();
        });
        m_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting ||
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });

        // 前键（上）：单击音量+10，长按最大音量
        f_button_.OnClick([this]() { AdjustVolume(10); });
        f_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        // 后键（下）：单击音量-10，长按静音
        b_button_.OnClick([this]() { AdjustVolume(-10); });
        b_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        // 右键：按下开始聆听，松开停止
        r_button_.OnPressDown([]() { Application::GetInstance().StartListening(); });
        r_button_.OnPressUp([]() { Application::GetInstance().StopListening(); });

        // 左键：长按切换主题（亮/暗）
        l_button_.OnLongPress([]() {
            Settings settings("display", true);
            std::string theme = settings.GetString("theme", "light");
            theme = theme == "light" ? "dark" : "light";
            settings.SetString("theme", theme);
            if (auto display = Board::GetInstance().GetDisplay()) {
                if (auto new_theme = LvglThemeManager::GetInstance().GetTheme(theme)) {
                    display->SetTheme(new_theme);
                }
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() { static LampController lamp(LAMP_GPIO); }

public:
    KayzhML307Headphone()
        : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
          m_button_(M_BUTTON_GPIO),
          f_button_(F_BUTTON_GPIO),
          b_button_(B_BUTTON_GPIO),
          r_button_(R_BUTTON_GPIO),
          l_button_(L_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Audio comes from a UAC 1.0 headset (AB13X adapter) on the USB-OTG port.
        static Ab13xAudioCodec audio_codec;
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(KayzhML307Headphone);
