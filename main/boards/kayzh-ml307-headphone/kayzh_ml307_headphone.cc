#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/ab13x_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "dual_network_board.h"
#include "identity.h"
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
#include <esp_timer.h>

#include <atomic>
#include <string>

#define TAG "KayzhML307Headphone"

class KayzhML307Headphone : public DualNetworkBoard {
private:
    Display* display_ = nullptr;
    Button m_button_;
    Button f_button_;
    Button b_button_;
    Button r_button_;
    Button l_button_;

    // ---- Identity selection menu (L long press) ----
    enum class MenuState : int { kHidden = 0, kList, kConfirm };
    std::atomic<MenuState> menu_state_{MenuState::kHidden};
    int menu_cursor_ = 0;
    int menu_pending_slot_ = -1;
    int64_t menu_last_activity_ms_ = 0;
    esp_timer_handle_t menu_timer_ = nullptr;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    bool MenuVisible() const { return menu_state_.load() != MenuState::kHidden; }

    static constexpr uint32_t kMenuAccentColor = 0xFFB300;
    static constexpr uint32_t kMenuTextColor = 0xFFFFFF;

    // ---- Full-screen LVGL overlay used to render the menu (never SetChatMessage) ----
    lv_obj_t* menu_overlay_ = nullptr;
    lv_obj_t* menu_title_ = nullptr;
    lv_obj_t* menu_rows_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* menu_hint_ = nullptr;

    static lv_obj_t* CreateMenuLabel(lv_obj_t* parent, const char* text) {
        lv_obj_t* label = lv_label_create(parent);
        lv_obj_set_width(label, LV_PCT(96));
        // Single line only: overflow is ellipsised, never wrapped (layout is 6 fixed rows).
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        // The overlay background is always dark, so white reads in both themes.
        lv_obj_set_style_text_color(label, lv_color_hex(kMenuTextColor), 0);
        lv_label_set_text(label, text);
        return label;
    }

    // Rough display width in columns (CJK = 2). Used only to decide whether the
    // extra " 当前" marker still fits inside the 12-Chinese-character budget.
    static int MenuTextColumns(const std::string& text) {
        int columns = 0;
        for (size_t i = 0; i < text.size();) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80) {
                columns += 1;
                i += 1;
            } else if ((c & 0xE0) == 0xC0) {
                columns += 1;
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                columns += 2;
                i += 3;
            } else {
                columns += 2;
                i += 4;
            }
        }
        return columns;
    }

    // Lazily build the overlay; reused for as long as the menu stays open.
    void MenuOverlayCreate() {
        auto* display = GetDisplay();
        if (display == nullptr || menu_overlay_ != nullptr) {
            return;
        }
        DisplayLockGuard lock(display);
        lv_obj_t* screen = lv_screen_active();
        if (screen == nullptr) {
            return;
        }
        menu_overlay_ = lv_obj_create(screen);
        lv_obj_remove_style_all(menu_overlay_);
        lv_obj_set_size(menu_overlay_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(menu_overlay_);
        lv_obj_set_style_bg_color(menu_overlay_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(menu_overlay_, LV_OPA_90, 0);
        lv_obj_clear_flag(menu_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(menu_overlay_, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(menu_overlay_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(menu_overlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        // Fixed layout on the 320x170 panel: title + 4 slot rows + 1 hint row = 6 lines.
        // Height budget: 6 * 21px line height + 5 * 4px row gap + 2 * 4px padding
        //                = 126 + 20 + 8 = 154px < 170px.
        lv_obj_set_style_pad_row(menu_overlay_, 4, 0);
        lv_obj_set_style_pad_all(menu_overlay_, 4, 0);

        menu_title_ = CreateMenuLabel(menu_overlay_, "身份选择");
        for (auto& row : menu_rows_) {
            row = CreateMenuLabel(menu_overlay_, "");
        }
        menu_hint_ = CreateMenuLabel(menu_overlay_, "F/B选  M确认  L取消");

        // Above the emoji box, chat bubbles and status bar.
        lv_obj_move_foreground(menu_overlay_);
    }

    void MenuOverlayDestroy() {
        if (menu_overlay_ == nullptr) {
            return;
        }
        auto* display = GetDisplay();
        if (display != nullptr) {
            DisplayLockGuard lock(display);
            lv_obj_del(menu_overlay_);
        }
        menu_overlay_ = nullptr;
        menu_title_ = nullptr;
        menu_hint_ = nullptr;
        for (auto& row : menu_rows_) {
            row = nullptr;
        }
    }

    void RenderMenuList() {
        MenuOverlayCreate();
        auto* display = GetDisplay();
        if (display == nullptr || menu_overlay_ == nullptr) {
            return;
        }

        int count = Identity::GetCount();
        int current = Identity::GetSlot();
        std::string rows[4];
        for (int i = 0; i < 4; ++i) {
            if (i >= count) {
                continue;
            }
            std::string body = std::to_string(i + 1) + "." + Identity::GetName(i);
            body += "  ";
            body += Identity::IsConfigured(i) ? "已配置" : "未配置";
            // " 当前" costs 6 columns. Budget is 12 Chinese chars (24 columns)
            // including the 1-column cursor prefix; if it does not fit, mark the
            // current slot with '*' instead of appending a suffix.
            bool show_current_suffix = (i == current) && (MenuTextColumns(body) + 1 + 6 <= 24);
            if (i == current && !show_current_suffix) {
                rows[i] = (i == menu_cursor_) ? ">*" : "*";
                rows[i] += body;
            } else {
                rows[i] = (i == menu_cursor_) ? ">" : " ";
                rows[i] += body;
                if (i == current) {
                    rows[i] += "  当前";
                }
            }
        }
        // 设备端无法解绑：服务端才是"是否已绑"的权威，换绑只能在后台删除设备后重启该槽。
        // Shares the fixed 6th line with the key hints — never a 7th line.
        const char* hint = Identity::IsConfigured(menu_cursor_) ? "M确认 L取消 换绑需后台删"
                                                               : "F/B选  M确认  L取消";

        DisplayLockGuard lock(display);
        lv_label_set_text(menu_title_, "身份选择");
        for (int i = 0; i < 4; ++i) {
            if (i >= count) {
                lv_obj_add_flag(menu_rows_[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(menu_rows_[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(menu_rows_[i], rows[i].c_str());
            lv_obj_set_style_text_color(
                menu_rows_[i],
                lv_color_hex(i == menu_cursor_ ? kMenuAccentColor : kMenuTextColor), 0);
        }
        lv_label_set_text(menu_hint_, hint);
    }

    void RenderMenuConfirm() {
        MenuOverlayCreate();
        auto* display = GetDisplay();
        if (display == nullptr || menu_overlay_ == nullptr) {
            return;
        }

        std::string line1 = Identity::GetName(menu_pending_slot_) + " 尚未配置";

        DisplayLockGuard lock(display);
        lv_label_set_text(menu_title_, "身份选择");
        lv_obj_remove_flag(menu_rows_[0], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(menu_rows_[0], line1.c_str());
        lv_obj_set_style_text_color(menu_rows_[0], lv_color_hex(kMenuTextColor), 0);
        lv_obj_remove_flag(menu_rows_[1], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(menu_rows_[1], "重启后将显示六位码用于绑定");
        lv_obj_set_style_text_color(menu_rows_[1], lv_color_hex(kMenuTextColor), 0);
        lv_obj_add_flag(menu_rows_[2], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_rows_[3], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(menu_hint_, "M确认 L取消");
    }

    void MenuOpen() {
        if (MenuVisible()) {
            return;
        }
        // Only enter the menu from idle; a conversation in progress keeps the screen.
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        menu_cursor_ = Identity::GetSlot();
        menu_pending_slot_ = -1;
        menu_last_activity_ms_ = NowMs();
        menu_state_.store(MenuState::kList);
        if (menu_timer_ != nullptr) {
            esp_timer_start_periodic(menu_timer_, 500 * 1000);
        }
        RenderMenuList();
    }

    void MenuClose() {
        menu_state_.store(MenuState::kHidden);
        menu_pending_slot_ = -1;
        if (menu_timer_ != nullptr) {
            esp_timer_stop(menu_timer_);
        }
        // Every exit path (cancel, confirm+reboot, 10s timeout, non-idle) lands here,
        // so the overlay is torn down in exactly one place.
        MenuOverlayDestroy();
    }

    void MenuMove(int delta) {
        if (menu_state_.load() != MenuState::kList) {
            return;
        }
        menu_last_activity_ms_ = NowMs();
        int next = menu_cursor_ + delta;
        if (next < 0 || next >= Identity::GetCount()) {
            return;  // no wrap at either end
        }
        menu_cursor_ = next;
        RenderMenuList();
    }

    static void RebootSoon(int delay_ms) {
        auto& app = Application::GetInstance();
        app.Schedule([&app, delay_ms]() {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            app.Reboot();
        });
    }

    void MenuConfirm() {
        MenuState state = menu_state_.load();
        if (state == MenuState::kHidden) {
            return;
        }
        menu_last_activity_ms_ = NowMs();

        if (state == MenuState::kConfirm) {
            int slot = menu_pending_slot_;
            MenuClose();
            if (!Identity::SelectSlot(slot)) {
                return;
            }
            RebootSoon(300);
            return;
        }

        int target = menu_cursor_;
        if (target == Identity::GetSlot()) {
            MenuClose();
            if (auto display = GetDisplay()) {
                display->ShowNotification("当前已是 " + Identity::GetName(target));
            }
            return;
        }
        if (!Identity::IsConfigured(target)) {
            menu_pending_slot_ = target;
            menu_state_.store(MenuState::kConfirm);
            RenderMenuConfirm();
            return;
        }
        MenuClose();
        // Must persist the slot BEFORE rebooting: this branch used to only toast and
        // restart, so picking any already-configured slot silently stayed on the old one.
        if (!Identity::SelectSlot(target)) {
            return;
        }
        if (auto display = GetDisplay()) {
            display->ShowNotification("切换到 " + Identity::GetName(target));
        }
        RebootSoon(800);
    }

    void MenuCancel() {
        MenuState state = menu_state_.load();
        if (state == MenuState::kHidden) {
            return;
        }
        menu_last_activity_ms_ = NowMs();
        if (state == MenuState::kConfirm) {
            menu_pending_slot_ = -1;
            menu_state_.store(MenuState::kList);
            RenderMenuList();
            return;
        }
        MenuClose();
    }

    void MenuTick() {
        MenuState state = menu_state_.load();
        if (state == MenuState::kHidden) {
            if (menu_timer_ != nullptr) {
                esp_timer_stop(menu_timer_);
            }
            return;
        }
        // A conversation started under the menu: get out of the way.
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            MenuClose();
            return;
        }
        if (NowMs() - menu_last_activity_ms_ < 10000) {
            return;
        }
        if (state == MenuState::kConfirm) {
            menu_pending_slot_ = -1;
            menu_state_.store(MenuState::kList);
            menu_last_activity_ms_ = NowMs();
            RenderMenuList();
            return;
        }
        MenuClose();
    }

    void InitializeMenuTimer() {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<KayzhML307Headphone*>(arg);
            Application::GetInstance().Schedule([self]() { self->MenuTick(); });
        };
        args.arg = this;
        args.name = "identity_menu";
        ESP_ERROR_CHECK(esp_timer_create(&args, &menu_timer_));
    }

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
        // 中键：单击对话 / 菜单确认；长按打断播报；双击（启动态）切换网络
        m_button_.OnClick([this]() {
            if (MenuVisible()) {
                Application::GetInstance().Schedule([this]() { MenuConfirm(); });
                return;
            }
            Application::GetInstance().ToggleChatState();
        });
        m_button_.OnLongPress([this]() {
            if (MenuVisible()) {
                return;
            }
            Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        });
        m_button_.OnDoubleClick([this]() {
            if (MenuVisible()) {
                return;
            }
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });

        // 前键（上）：单击音量+10，长按最大音量；菜单内单击上移
        f_button_.OnClick([this]() {
            if (MenuVisible()) {
                Application::GetInstance().Schedule([this]() { MenuMove(-1); });
                return;
            }
            AdjustVolume(10);
        });
        f_button_.OnLongPress([this]() {
            if (MenuVisible()) {
                return;
            }
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        // 后键（下）：单击音量-10，长按静音；菜单内单击下移
        b_button_.OnClick([this]() {
            if (MenuVisible()) {
                Application::GetInstance().Schedule([this]() { MenuMove(1); });
                return;
            }
            AdjustVolume(-10);
        });
        b_button_.OnLongPress([this]() {
            if (MenuVisible()) {
                return;
            }
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        // 右键：单击循环背光亮度（100/60/30/10），长按进入 WiFi 配网（仅 WiFi）
        r_button_.OnClick([this]() {
            if (MenuVisible()) {
                return;
            }
            static const uint8_t kLevels[] = {100, 60, 30, 10};
            auto backlight = GetBacklight();
            uint8_t next = kLevels[0];
            for (uint8_t level : kLevels) {
                if (level < backlight->brightness()) {
                    next = level;
                    break;
                }
            }
            backlight->SetBrightness(next, true);
            GetDisplay()->ShowNotification("亮度 " + std::to_string(next));
        });
        r_button_.OnLongPress([this]() {
            if (MenuVisible()) {
                return;
            }
            if (GetNetworkType() != NetworkType::WIFI) {
                GetDisplay()->ShowNotification("配网仅支持 Wi-Fi");
                return;
            }
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.EnterWifiConfigMode();
        });

        // 左键：单击切换主题（亮/暗），长按 3 秒进入身份选择（菜单内单击取消）
        l_button_.OnClick([this]() {
            if (MenuVisible()) {
                Application::GetInstance().Schedule([this]() { MenuCancel(); });
                return;
            }
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
        l_button_.OnLongPress([this]() {
            if (MenuVisible()) {
                return;
            }
            Application::GetInstance().Schedule([this]() { MenuOpen(); });
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() { static LampController lamp(LAMP_GPIO); }

public:
    KayzhML307Headphone()
        : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
          m_button_(M_BUTTON_GPIO, false, 3000),
          f_button_(F_BUTTON_GPIO),
          b_button_(B_BUTTON_GPIO),
          r_button_(R_BUTTON_GPIO),
          l_button_(L_BUTTON_GPIO, false, 3000) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeMenuTimer();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual void StartNetwork() override {
        DualNetworkBoard::StartNetwork();
        // Show which identity this boot is using (slot 0 included).
        if (auto display = GetDisplay()) {
            display->ShowNotification(Identity::GetName(Identity::GetSlot()), 2000);
        }
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
