// 谷仓次元屏（SDGOODS Dimension Screen）xiaozhi-esp32 板型
// 硬件事实源：SDGOODS-ESP32S3/components/sdgoods_board
//   ESP32-S3 + 8MB Octal PSRAM + 32MB Flash
//   ST77916 QSPI 360x360 圆屏（屏厂初始化表取自 sdgoods_board/lcd/st77916_vendor_init.inc）
//   数字 MEMS 麦 + I2S 功放（无 codec 芯片，见 sdgoods_audio_codec.cc）
#include "wifi_board.h"
#include "sdgoods_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "adc_battery_monitor.h"
#include "system_info.h"
#include "mcp_server.h"
#include "config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>  // adc_unit_t / ADC_CHANNEL_7（新 esp_adc 头，避免 legacy driver/adc.h 弃用警告）
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <functional>
#include <string>
#include <lvgl.h>

#define TAG "sdgoods_dimension_screen"

// 控制中心样式用字体（与原厂同源 Noto Sans SC，lv_font_conv 生成子集）：
// 16 号 = 标题/正文（对齐原厂 si_yuan_black_icon_16），14 号 = 瓷砖下方 caption（对齐 si_yuan_black_icon_14）。
// 控制中心页文案为中文，两支字体均覆盖 ASCII 0x20-0x7E + 所需汉字子集（控制中心设置…关于数据缓存等）。
// 图标不再用 Material 字体，改回原厂 canvas 手绘 48x48 白线稿（CcIconDraw）。
LV_FONT_DECLARE(font_cc_noto_16_4);
LV_FONT_DECLARE(font_cc_noto_14_4);

// 圆屏安全区：上游 LcdDisplay 是矩形布局，在 360x360 圆屏上四角内容会被圆形黑边裁掉。
// 这里在板级子类里覆写 SetupUI，对全宽元素按弦宽向内收紧（r=180，不改动上游通用代码）。
class SdgoodsRoundLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    // SetupUI 完成后回调（板级用来挂控制中心浮层：必须在容器建好之后再叠在上层）
    std::function<void()> on_setup_done;

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        // 顶部状态栏（y≈0..32，弦宽≈205）：图标内收
        if (top_bar_ != nullptr) {
            lv_obj_set_style_pad_left(top_bar_, 72, 0);
            lv_obj_set_style_pad_right(top_bar_, 72, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_width(status_label_, 190);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_width(notification_label_, 190);
        }
        // 聊天区（仅微信气泡样式才有；默认大 emoji 样式下 content_ 为 null，此段空转）
        if (content_ != nullptr) {
            lv_obj_set_style_pad_left(content_, 44, 0);
            lv_obj_set_style_pad_right(content_, 44, 0);
        }
        // 底部字幕条（默认样式：bottom_bar_ + chat_message_label_ 单行滚动）：
        // 上游贴底全宽(360) 会被圆屏下缘弧切 + 左右两端出圆；这里抬进圆内、
        // 按该高度的弦宽加宽到 240（尽量长条、又不超出圆），做半透圆角胶囊，保留单行滚动。
        // 配网提示与对话字幕共用这一个控件（Alert→SetChatMessage），改一处两边同步。
        if (bottom_bar_ != nullptr) {
            lv_obj_set_width(bottom_bar_, 240);
            lv_obj_set_style_radius(bottom_bar_, 999, 0);                    // 胶囊圆角（LVGL 自动限幅到 h/2）
            lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_20, 0);             // 浅灰药丸：10% 在白底上太淡，对话态反光/滚动时几乎看不见，提到 20% 稳定可见
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -50);         // 抬进可视圆内
            lv_obj_move_foreground(bottom_bar_);                            // 兜底：防对话态任何层压住胶囊底
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_set_width(chat_message_label_, 216);                     // 胶囊内安全宽度
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);  // 保留单行滚动
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
        // 低电提示（底部 y≈320..340，弦宽急剧变窄）：缩窄并上提
        if (low_battery_popup_ != nullptr) {
            lv_obj_set_width(low_battery_popup_, 160);
            lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -48);
        }
        if (on_setup_done) {
            on_setup_done();
        }
    }
};

// ST77916 QSPI 屏厂初始化序列（谷仓次元屏实测可用）
static const st77916_lcd_init_cmd_t vendor_specific_init_sdgoods[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

class SdgoodsDimensionScreen : public WifiBoard {
private:
    Button boot_button_;
    SdgoodsRoundLcdDisplay* display_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;

    // 控制中心浮层（照抄原厂 sdgoods_cc.c 的 iOS 风格：满屏纯黑 + 圆形瓷砖 + 二级页）
    lv_obj_t* cc_page_ = nullptr;        // 一级页（控制中心）
    lv_obj_t* cc_set_page_ = nullptr;    // 二级：设置（音量/亮度/网络/电池）
    lv_obj_t* cc_slider_page_ = nullptr; // 二级：滑块页（音量/亮度）
    lv_obj_t* cc_data_page_ = nullptr;   // 二级：网络数据页
    lv_obj_t* cc_battery_page_ = nullptr;// 二级：电量页
    lv_obj_t* cc_about_page_ = nullptr;  // 二级：关于设备页
    lv_obj_t* cc_slider_value_label_ = nullptr; // 滑块页顶部数值
    bool cc_slider_is_volume_ = true;            // 当前滑块页是音量还是亮度
    lv_coord_t cc_gesture_start_y_ = 0;

    // 熄屏（语音“关闭屏幕”的友好形态：降背光 + 全黑页触摸即醒，避免误以为死机）
    lv_obj_t* cc_dim_page_ = nullptr;
    int saved_brightness_ = -1;   // 熄屏前的背光档位（-1 = 无记录，唤醒时从 settings 恢复）

    void InitializePowerLatch() {
        // 电池供电自锁：开机立即拉高 GPIO7 保持上电，否则松开电源键就断电（USB 供电时看不出，
        // 电池供电时表现为“只能按住才开机”）。与 SDGOODS BSP main.c 开机顺序一致。
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BAT_CONTROL_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(BAT_CONTROL_GPIO, BAT_CONTROL_LATCH_LEVEL);
        ESP_LOGI(TAG, "Battery power latch engaged (GPIO%d high)", BAT_CONTROL_GPIO);
    }

    void InitializeDisplayPower() {
        // 次元屏屏电源使能：GPIO12 拉低上电（与 SDGOODS BSP 一致）
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << DISPLAY_POWER_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(DISPLAY_POWER_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                        QSPI_PIN_NUM_LCD_DATA0,
                                                                        QSPI_PIN_NUM_LCD_DATA1,
                                                                        QSPI_PIN_NUM_LCD_DATA2,
                                                                        QSPI_PIN_NUM_LCD_DATA3,
                                                                        QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void Initializest77916Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 0,
            .pclk_hz = DISPLAY_SPI_PCLK_HZ,
            .trans_queue_depth = 10,
            .on_color_trans_done = NULL,
            .user_ctx = NULL,
            .lcd_cmd_bits = 32,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .octal_mode = 0,
                .quad_mode = 1,
                .sio_mode = 0,
                .lsb_first = 0,
                .cs_high_active = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver");
        st77916_vendor_config_t vendor_config = {};
        vendor_config.init_cmds = vendor_specific_init_sdgoods;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_sdgoods) / sizeof(st77916_lcd_init_cmd_t);
        vendor_config.flags.use_qspi_interface = 1;

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL;
        panel_config.reset_gpio_num = QSPI_PIN_NUM_LCD_RST;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SdgoodsRoundLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    /*fullscreen_psram_buf=*/false);  // 保持内部 SRAM DMA 部分缓冲：QSPI esp_lcd 走满屏 PSRAM 需 ~259KB DMA 回弹缓冲，内部 RAM 不够会 setup_dma_priv_buffer 失败并卡死 flush（实测），20 行 DMA 反而是最快可用路径
        // 上游 SetupUI 由 Application::Start 触发，建完容器后再叠控制中心浮层
        display_->on_setup_done = [this]() { SetupControlCenter(); };
    }

    void InitializeI2c() {
        // 触摸专用 I2C 总线（新驱动）：SDA=11 / SCL=10
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = TOUCH_I2C_SDA_PIN,
            .scl_io_num = TOUCH_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        // 触摸不是致命路径：失败只记日志，不 crash（语音交互仍可用）
        if (i2c_new_master_bus(&bus_cfg, &i2c_bus_) != ESP_OK) {
            i2c_bus_ = nullptr;
            ESP_LOGE(TAG, "I2C bus init failed, touch disabled");
        }
    }

    void InitializeTouch() {
        if (i2c_bus_ == nullptr) {
            return;
        }
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = TOUCH_GPIO_RST,
            .int_gpio_num = TOUCH_GPIO_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                // 屏做了 swap_xy + mirror_x 硬件旋转，触摸原始坐标（面板坐标系）需要同变换。
                // esp_lcd_touch 软件变换顺序：先 mirror 后 swap，即 (x,y)->(y, 359-x)，
                // 与原厂 map_raw_to_lvgl（out_x=raw_y, out_y=W-1-raw_x）一致；若实机方向不对再调
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        io_cfg.scl_speed_hz = 400 * 1000;
        io_cfg.control_phase_bytes = 1;
        io_cfg.dc_bit_offset = 0;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 0;
        io_cfg.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Touch panel IO init failed");
            return;
        }
        if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
            ESP_LOGE(TAG, "CST816S init failed");
            return;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "CST816S touch ready (I2C addr 0x%02X)", ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS);
    }

    // 关机：熄屏 -> 释放电池自锁闩断电；USB 供电时落到深睡兑底（与原厂 sdgoods_power_off 同构）
    void PowerOff() {
        ESP_LOGW(TAG, "Power off: releasing battery latch GPIO%d", BAT_CONTROL_GPIO);
        GetBacklight()->SetBrightness(0);
        vTaskDelay(pdMS_TO_TICKS(800));  // 等背光渐变完全熄灭
        gpio_set_level(BAT_CONTROL_GPIO, BAT_CONTROL_LATCH_LEVEL ? 0 : 1);
        esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);  // 电源键低有效唤醒
        esp_deep_sleep_start();
    }
    
    // 熄屏：不全黑“装死机”，而是降背光 + 全黑页常驻“轻触唤醒”提示，触摸任意位置即醒。
    // 语音“关闭屏幕”走这里（MCP self.screen.sleep），而不是 set_brightness(0) 真黑屏。
    void ScreenOff() {
        if (cc_dim_page_ != nullptr) {
            return;
        }
        DeleteAllCcPages();  // 先收掉控制中心浮层，熄屏页才是最上层
        int cur = GetBacklight()->brightness();
        saved_brightness_ = cur > 0 ? cur : -1;
        GetBacklight()->SetBrightness(4);  // 保留微亮：页面提示可见 + 触摸可唤醒 + 省电
        auto page = lv_obj_create(lv_screen_active());
        cc_dim_page_ = page;
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_color(page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(page);   // 盖住顶部下滑捕获条，熄屏时误滑不开控制中心
        auto lbl = lv_label_create(page);
        lv_label_set_text(lbl, "熄屏中\n轻触屏幕唤醒");
        lv_obj_set_style_text_font(lbl, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x555A66), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(page, OnDimTouched, LV_EVENT_CLICKED, this);
        ESP_LOGI(TAG, "Screen off (dim mode), wake by touch");
    }
    
    static void OnDimTouched(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        // CLICKED 在按下分发中会删页？不会——CLICKED 在松手后派发；但为与全局惯例一致仍走 async
        lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->ScreenWake(); }, self);
    }
    
    void ScreenWake() {
        if (cc_dim_page_ != nullptr) {
            lv_obj_delete(cc_dim_page_);
            cc_dim_page_ = nullptr;
        }
        if (saved_brightness_ >= 0) {
            GetBacklight()->SetBrightness(static_cast<uint8_t>(saved_brightness_));
            saved_brightness_ = -1;
        } else {
            GetBacklight()->RestoreBrightness();  // 无记录（如开机即熄屏）：回到 settings 档位
        }
        ESP_LOGI(TAG, "Screen wake");
    }

    // 覆写 Board 虚函数：上游通用 MCP 工具 self.screen.set_brightness 会透传到这里。
    // 0 = 进触摸唤醒的熄屏页（而非真关背光被误认为死机）；非 0 若正在熄屏则先唤醒再调亮度。
    virtual void SetScreenBrightness(uint8_t brightness) override {
        if (cc_dim_page_ != nullptr) {
            ScreenWake();
        }
        if (brightness == 0) {
            ScreenOff();
            return;
        }
        GetBacklight()->SetBrightness(brightness, true);  // permanent：存 settings
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // 长按 ≥1.5s：释放自锁闩关机（LONG_PRESS_START 触发一次，无需等松手）
        boot_button_.OnLongPress([this]() {
            PowerOff();
        });
    }

    // 语音控制工具：self.screen.sleep / self.screen.wake（名字唯一，不与上游冲突）。
    // 上游通用 self.screen.set_brightness 已改为透传 Board::SetScreenBrightness（见本类覆写），
    // “亮度调到 0”不再真关背光装死机，而是进微亮熄屏页触摸即醒。
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.screen.sleep",
            "Turn off the screen (dim standby). Call when user says turn off / close the screen (关闭屏幕/熄屏). The screen wakes up by touching it.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ScreenOff();
                return true;
            });
        mcp_server.AddTool("self.screen.wake",
            "Wake up the screen from dim standby. Call when user says turn on / open the screen (打开屏幕/唤醒屏幕).",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ScreenWake();
                return true;
            });
    }

    // ---------- 下拉控制中心（1:1 照搬原厂 sdgoods_cc.c） ----------
    // 视觉语言：满屏纯黑页 + 直径 68 圆形瓷砖（#2C2C2E，按下 #3A3A3C）+ canvas 手绘
    // 白线稿图标（zoom 208）+ 下方 14 号灰 caption(#E0E0E5) + 底部灰色 home 小横条；
    // 一级页「设置 / 关于 / 关机」，设置页「音量 / 亮度 / 网络 / 电池 / 熄屏」，
    // 点开进二级滑块/数据/电量/关于页（文案与几何均对齐原厂）。
    // lv_style_selector_t 是 uint32_t（part<<16 | state），直接用枚举常量组合
    static constexpr lv_style_selector_t kSel() { return LV_PART_MAIN; }
    static constexpr lv_style_selector_t kSelPressed() { return LV_PART_MAIN | LV_STATE_PRESSED; }
    static constexpr lv_style_selector_t kSelIndicator() { return LV_PART_INDICATOR; }
    static constexpr lv_style_selector_t kSelKnob() { return LV_PART_KNOB; }

    // 满屏不透明黑页（原厂：菜单页不漏出主屏）。刻意不做淡入/淡出——
    // 主界面是白底、本页是纯黑，半透明过渡会被部分缓冲逐格刷出，肉眼看到
    // “白→灰→黑”的渐进过程，反而被误判成卡顿；原厂就是瞬间整屏黑，干脆利落。
    lv_obj_t* CreateCcPage() {
        auto page = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_color(page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(page);
        return page;
    }

    // 页标题：白色 16 号字，y=40（原厂标题同一位置）
    void CreateCcTitle(lv_obj_t* page, const char* text) {
        auto title = lv_label_create(page);
        lv_label_set_text(title, text);
        lv_obj_set_style_text_font(title, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);  // 触摸穿透，不挡手势
    }

    // 原厂 cc_icon_t：48x48 canvas 手绘白线稿图标种类
    enum CcIconKind {
        CC_ICON_PWR, CC_ICON_BRI, CC_ICON_VOL, CC_ICON_DATA, CC_ICON_BAT,
        CC_ICON_INFO, CC_ICON_SET, CC_ICON_SLEEP,
    };

    // 在 48x48 canvas 上按原厂 sdgoods_cc.c cc_icon_draw 的几何 1:1 绘白线稿。
    // LVGL8 -> LVGL9 API 换算：无 lv_canvas_draw_*，改用 lv_canvas_init_layer/
    // finish_layer 夹住 lv_draw_line/rect/arc/triangle；lv_trigo_sin/cos 仍返回
    // ±32767，必须 >> LV_TRIGO_SHIFT(15) 归一化（否则半径放大 32 倍飞出画布）。
    static void CcIconDraw(lv_obj_t* canvas, CcIconKind kind) {
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);

        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_white();
        line.width = 4;
        line.opa = LV_OPA_COVER;

        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = 4;
        arc.opa = LV_OPA_COVER;

        lv_draw_rect_dsc_t fill;
        lv_draw_rect_dsc_init(&fill);
        fill.bg_color = lv_color_white();
        fill.bg_opa = LV_OPA_COVER;
        fill.radius = 5;
        fill.border_width = 0;

        // 封装小工具：原厂 lv_canvas_draw_rect/line 在 LVGL9 下的等价调用
        auto rect = [&](int32_t x, int32_t y, int32_t w, int32_t h, const lv_draw_rect_dsc_t* d) {
            lv_area_t a{ x, y, x + w - 1, y + h - 1 };
            lv_draw_rect(&layer, d, &a);
        };
        auto seg = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
            lv_draw_line_dsc_t l = line;
            l.p1.x = x1; l.p1.y = y1; l.p2.x = x2; l.p2.y = y2;
            lv_draw_line(&layer, &l);
        };
        auto dot = [&](int32_t cx, int32_t cy, int32_t r) {
            lv_draw_rect_dsc_t d = fill;
            d.radius = LV_RADIUS_CIRCLE;
            rect(cx - r, cy - r, r * 2, r * 2, &d);
        };

        switch (kind) {
        case CC_ICON_PWR:
            arc.center.x = 24; arc.center.y = 24; arc.radius = 15;
            arc.start_angle = 0; arc.end_angle = 360;
            lv_draw_arc(&layer, &arc);
            seg(24, 5, 24, 17);
            break;
        case CC_ICON_BRI: {
            // 中心实心圆 + 8 向胶囊光芒（线段两端叠 6x6 圆点拼圆头；canvas 下 round 端画不出）
            lv_draw_rect_dsc_t c = fill; c.radius = LV_RADIUS_CIRCLE;
            rect(18, 18, 12, 12, &c);
            for (int i = 0; i < 8; i++) {
                int32_t a = i * 45;
                int32_t s = lv_trigo_sin(a), cc = lv_trigo_cos(a);
                int32_t x0 = 24 + ((11 * cc) >> LV_TRIGO_SHIFT);
                int32_t y0 = 24 + ((11 * s) >> LV_TRIGO_SHIFT);
                int32_t x1 = 24 + ((16 * cc) >> LV_TRIGO_SHIFT);
                int32_t y1 = 24 + ((16 * s) >> LV_TRIGO_SHIFT);
                seg(x0, y0, x1, y1);
                dot(x0, y0, 3);
                dot(x1, y1, 3);
            }
            break;
        }
        case CC_ICON_VOL: {
            lv_draw_rect_dsc_t spk = fill; spk.radius = 2;
            rect(8, 17, 9, 14, &spk);                       // 箱体
            lv_draw_triangle_dsc_t tri;
            lv_draw_triangle_dsc_init(&tri);
            tri.color = lv_color_white(); tri.opa = LV_OPA_COVER;
            tri.p[0].x = 17; tri.p[0].y = 18; tri.p[1].x = 17; tri.p[1].y = 30; tri.p[2].x = 27; tri.p[2].y = 36;
            lv_draw_triangle(&layer, &tri);                 // 梯形=两三角形拼接
            tri.p[0].x = 17; tri.p[0].y = 18; tri.p[1].x = 27; tri.p[1].y = 36; tri.p[2].x = 27; tri.p[2].y = 12;
            lv_draw_triangle(&layer, &tri);
            arc.center.x = 27; arc.center.y = 24; arc.radius = 13; arc.start_angle = 320; arc.end_angle = 40;
            lv_draw_arc(&layer, &arc);                      // 两道声波弧
            arc.radius = 19; arc.start_angle = 315; arc.end_angle = 45;
            lv_draw_arc(&layer, &arc);
            break;
        }
        case CC_ICON_DATA: {
            lv_draw_rect_dsc_t bar = fill; bar.radius = 2;
            rect(11, 28, 8, 10, &bar);
            rect(21, 20, 8, 18, &bar);
            rect(31, 12, 8, 26, &bar);
            break;
        }
        case CC_ICON_BAT:
            rect(6, 14, 30, 3, &fill); rect(6, 31, 30, 3, &fill);   // 外框上/下
            rect(6, 14, 3, 20, &fill); rect(33, 14, 3, 20, &fill);  // 外框左/右
            rect(36, 20, 6, 8, &fill); rect(11, 19, 18, 9, &fill);  // 凸点 + 电量条
            break;
        case CC_ICON_INFO:
            rect(21, 9, 6, 6, &fill); rect(21, 20, 6, 16, &fill);   // 点 + 竖线
            break;
        case CC_ICON_SET: {
            lv_draw_arc_dsc_t ring = arc;
            ring.center.x = 24; ring.center.y = 24; ring.radius = 18;
            ring.start_angle = 0; ring.end_angle = 360;
            lv_draw_arc(&layer, &ring);                             // 外圈
            rect(20, 20, 8, 8, &fill);                              // 中心方孔
            for (int i = 0; i < 8; i++) {
                int32_t a = i * 45;
                int32_t s = lv_trigo_sin(a), cc = lv_trigo_cos(a);
                int32_t cx = 24 + ((21 * cc) >> LV_TRIGO_SHIFT);
                int32_t cy = 24 + ((21 * s) >> LV_TRIGO_SHIFT);
                rect(cx - 3, cy - 3, 6, 6, &fill);                  // 8 齿
            }
            break;
        }
        case CC_ICON_SLEEP: {
            // 熄屏（原厂无对应图标）：自绘月牙 —— 一段开口朝右的粗弧
            lv_draw_arc_dsc_t m = arc;
            m.center.x = 24; m.center.y = 24; m.radius = 13;
            m.start_angle = 300; m.end_angle = 120;
            lv_draw_arc(&layer, &m);
            break;
        }
        default: break;
        }
        lv_canvas_finish_layer(canvas, &layer);
    }

    // 圆形瓷砖（原厂 make_round_btn）：lv_obj 灰底圆 + canvas 手绘线稿（zoom 208）+ 下挂 caption
    void CreateCcTile(lv_obj_t* parent, int cx, int cy, CcIconKind icon_kind, const char* caption,
                      lv_event_cb_t tap_cb) {
        constexpr int d = 68;
        auto btn = lv_obj_create(parent);          // 原厂用 lv_obj 而非 lv_button
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, d, d);
        lv_obj_set_pos(btn, cx - d / 2, cy - d / 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3C), kSelPressed());
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, tap_cb, LV_EVENT_CLICKED, this);

        // 图标缓冲：PSRAM 48x48 ARGB8888，跟随 canvas 删除而释放（不吃宝贵的内部 RAM）
        auto* buf = static_cast<uint32_t*>(heap_caps_malloc(48 * 48 * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
        if (buf == nullptr) {
            return;
        }
        auto cv = lv_canvas_create(btn);
        lv_canvas_set_buffer(cv, buf, 48, 48, LV_COLOR_FORMAT_ARGB8888);
        CcIconDraw(cv, icon_kind);
        lv_obj_center(cv);
        lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_transform_scale(cv, 208, 0);   // 原厂 zoom 208 ≈ 81% 缩小
        lv_obj_set_style_transform_pivot_x(cv, 24, 0);  // 以中心为缩放轴
        lv_obj_set_style_transform_pivot_y(cv, 24, 0);
        // 缓冲生命周期挂到 canvas DELETE：防图标缓冲泄漏
        lv_obj_add_event_cb(cv, [](lv_event_t* e) {
            auto* b = static_cast<uint32_t*>(lv_event_get_user_data(e));
            if (b != nullptr) {
                heap_caps_free(b);
            }
        }, LV_EVENT_DELETE, buf);

        auto cap = lv_label_create(parent);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_font(cap, &font_cc_noto_14_4, 0);  // 原厂 caption 用 14 号
        lv_obj_set_style_text_color(cap, lv_color_hex(0xE0E0E5), 0);
        lv_obj_align_to(cap, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    }

    // 底部 home 指示小横条：纯装饰（原厂 36x5 灰胶囊，@180,334）
    void CreateCcBottomHint(lv_obj_t* page) {
        auto bar = lv_obj_create(page);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 36, 5);
        lv_obj_set_pos(bar, DISPLAY_WIDTH / 2 - 18, 334);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x9A9A9E), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    // 整页下滑/上滑手势绑定（原厂：底部上滑关全部；边缘右滑返回上级）
    struct PageGestureCtx { SdgoodsDimensionScreen* self; bool down; };
    static void OnPageGesture(lv_event_t* e) {
        auto* ctx = static_cast<PageGestureCtx*>(lv_event_get_user_data(e));
        if (ctx == nullptr || ctx->self == nullptr) {
            return;
        }
        auto indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        auto code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED) {
            ctx->self->cc_gesture_start_y_ = pt.y;
        } else if (code == LV_EVENT_PRESSING) {
            if (ctx->down) {
                if (pt.y - ctx->self->cc_gesture_start_y_ >= 50) {
                    // 同样不在按下分发中删对象树，挪到下一拍
                    lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->CloseControlCenter(); }, ctx->self);
                }
            } else {
                if (ctx->self->cc_gesture_start_y_ - pt.y >= 50) {
                    lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->CloseControlCenter(); }, ctx->self);
                }
            }
        }
    }
    void BindCloseGesture(lv_obj_t* page, bool down) {
        auto* ctx = new PageGestureCtx{this, down};
        lv_obj_add_event_cb(page, OnPageGesture, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(page, [](lv_event_t* e) { delete static_cast<PageGestureCtx*>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, ctx);
    }

    // 左边缘右滑返回（原厂 swipe_back 语义，绑在页自身上）
    struct EdgeBackCtx { SdgoodsDimensionScreen* self; std::function<void()> back; lv_coord_t start_x; };
    static void OnEdgeBack(lv_event_t* e) {
        auto* ctx = static_cast<EdgeBackCtx*>(lv_event_get_user_data(e));
        auto code = lv_event_get_code(e);
        if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) {
            return;
        }
        auto indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (code == LV_EVENT_PRESSED) {
            ctx->start_x = pt.x;
        } else if (pt.x - ctx->start_x >= 50 && ctx->start_x <= 40) {
            auto back = ctx->back;
            lv_async_call([](void* arg) { (*static_cast<std::function<void()>*>(arg))(); }, new std::function<void()>(back));
        }
    }
    void BindEdgeBack(lv_obj_t* page, std::function<void()> back) {
        auto* ctx = new EdgeBackCtx{this, back, 0};
        lv_obj_add_event_cb(page, OnEdgeBack, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(page, [](lv_event_t* e) { delete static_cast<EdgeBackCtx*>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, ctx);
    }

    void DeletePage(lv_obj_t*& page) {
        if (page != nullptr) {
            auto p = page;
            page = nullptr;
            lv_obj_delete(p);   // 瞬间关闭，无淡出（理由同 CreateCcPage 注释）
        }
    }
    void DeleteAllCcPages() {
        DeletePage(cc_page_); DeletePage(cc_set_page_); DeletePage(cc_slider_page_);
        DeletePage(cc_data_page_); DeletePage(cc_battery_page_); DeletePage(cc_about_page_);
    }

    // 瓷砖点击分发（LVGL 事件回调里不直接改对象树，统一 async 下一拍执行，原厂同款教训）
    static void AsyncRun(lv_async_cb_t fn, void* arg) { lv_async_call(fn, arg); }
    static void AfterOpenSet(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSetPage(); }
    static void AfterOpenAbout(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenAboutPage(); }
    static void AfterOpenVolume(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSliderPage(true); }
    static void AfterOpenBrightness(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSliderPage(false); }
    static void AfterOpenData(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenDataPage(); }
    static void AfterOpenBattery(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenBatteryPage(); }
    static void AfterPower(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->PowerOff(); }
    static void AfterWifiConfig(void* arg) {
        auto self = static_cast<SdgoodsDimensionScreen*>(arg);
        self->CloseControlCenter();
        self->EnterWifiConfigMode();
    }

    static void OnTileSettings(lv_event_t* e) { AsyncRun(AfterOpenSet, lv_event_get_user_data(e)); }
    static void OnTileAbout(lv_event_t* e) { AsyncRun(AfterOpenAbout, lv_event_get_user_data(e)); }
    static void OnTilePower(lv_event_t* e) { AsyncRun(AfterPower, lv_event_get_user_data(e)); }
    static void OnTileVolume(lv_event_t* e) { AsyncRun(AfterOpenVolume, lv_event_get_user_data(e)); }
    static void OnTileBrightness(lv_event_t* e) { AsyncRun(AfterOpenBrightness, lv_event_get_user_data(e)); }
    static void OnTileData(lv_event_t* e) { AsyncRun(AfterOpenData, lv_event_get_user_data(e)); }
    static void OnTileBattery(lv_event_t* e) { AsyncRun(AfterOpenBattery, lv_event_get_user_data(e)); }
    static void OnNetConfig(lv_event_t* e) { AsyncRun(AfterWifiConfig, lv_event_get_user_data(e)); }

    static void AfterScreenOff(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->ScreenOff(); }
    static void OnTileScreenOff(lv_event_t* e) { AsyncRun(AfterScreenOff, lv_event_get_user_data(e)); }

    static void OnGestureStrip(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        auto code = lv_event_get_code(e);
        lv_indev_t* indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (code == LV_EVENT_PRESSED) {
            self->cc_gesture_start_y_ = pt.y;
        } else if (code == LV_EVENT_PRESSING) {
            if (self->cc_page_ == nullptr && pt.y - self->cc_gesture_start_y_ >= 40) {
                // 点开整页也是改对象树，同样 async 下一拍
                lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenControlCenter(); }, self);
            }
        }
    }

    void SetupControlCenter() {
        // 顶部下滑捕获条（平时只存这一个隐藏对象，点开才建整页，省常驻内存）
        auto strip = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, DISPLAY_WIDTH, 50);
        lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(strip, OnGestureStrip, LV_EVENT_ALL, this);
    }

    // 一级页：原厂三键「Settings / About / Power」同款布局与坐标（cx=84/180/276，cy=169）
    void OpenControlCenter() {
        if (cc_page_ != nullptr) {
            return;
        }
        auto page = CreateCcPage();
        cc_page_ = page;
        CreateCcTitle(page, "控制中心");
        CreateCcTile(page, 84, 169, CC_ICON_SET, "设置", OnTileSettings);
        CreateCcTile(page, 180, 169, CC_ICON_INFO, "关于", OnTileAbout);
        CreateCcTile(page, 276, 169, CC_ICON_PWR, "关机", OnTilePower);
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);  // 整页上滑关闭（下拉打开后，上滑收起，符合直觉且与子页一致）
    }

    // 二级设置页：原厂上排 Volume/Brightness/Data（cy=130）+ 下排 Battery（84,230），
    // 熄屏键 Sleep（180,230）为本板扩展（原厂无）
    void OpenSetPage() {
        DeletePage(cc_set_page_);
        auto page = CreateCcPage();
        cc_set_page_ = page;
        CreateCcTitle(page, "设置");
        CreateCcTile(page, 84, 130, CC_ICON_VOL, "音量", OnTileVolume);
        CreateCcTile(page, 180, 130, CC_ICON_BRI, "亮度", OnTileBrightness);
        CreateCcTile(page, 276, 130, CC_ICON_DATA, "网络", OnTileData);
        CreateCcTile(page, 84, 230, CC_ICON_BAT, "电池", OnTileBattery);
        CreateCcTile(page, 180, 230, CC_ICON_SLEEP, "熄屏", OnTileScreenOff);
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);  // 底部上滑直接关全部
        BindEdgeBack(page, [this]() { DeletePage(cc_set_page_); });  // 左边缘右滑回一级
    }

    // 滑块页（原厂 open_slider 同款）：标题 y=40、数值 y=72、240x24 大滑块居中
    void OpenSliderPage(bool volume) {
        DeletePage(cc_slider_page_);
        cc_slider_is_volume_ = volume;
        int init = volume ? GetAudioCodec()->output_volume() : GetBacklight()->brightness();
        auto page = CreateCcPage();
        cc_slider_page_ = page;
        CreateCcTitle(page, volume ? "音量" : "亮度");

        cc_slider_value_label_ = lv_label_create(page);
        lv_label_set_text_fmt(cc_slider_value_label_, "%d%%", init);
        lv_obj_set_style_text_font(cc_slider_value_label_, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(cc_slider_value_label_, lv_color_white(), 0);
        lv_obj_align(cc_slider_value_label_, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_clear_flag(cc_slider_value_label_, LV_OBJ_FLAG_CLICKABLE);

        auto slider = lv_slider_create(page);
        lv_obj_set_size(slider, 240, 24);
        lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
        lv_slider_set_range(slider, volume ? 0 : 1, 100);  // 亮度下限 1，防存 0 黑屏（原厂 CC_BRI_MIN）
        lv_slider_set_value(slider, init, LV_ANIM_OFF);
        // 原厂不对滑块做任何自定义样式（直接用默认主题）；仅绑事件
        lv_obj_add_event_cb(slider, OnCcSliderChanged, LV_EVENT_VALUE_CHANGED, this);

        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_slider_page_); });
    }

    static void OnCcSliderChanged(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        int v = lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
        if (self->cc_slider_value_label_ != nullptr) {
            lv_label_set_text_fmt(self->cc_slider_value_label_, "%d%%", v);
        }
        if (self->cc_slider_is_volume_) {
            self->GetAudioCodec()->SetOutputVolume(v);
            ESP_LOGI(TAG, "Volume set to %d", v);
        } else {
            self->GetBacklight()->SetBrightness(static_cast<uint8_t>(v), true);  // permanent：存 settings，开机恢复
        }
    }

    // 页内信息行：垂直居中堆叠（数据/电量/关于页共用，白主灰辅两档）
    lv_obj_t* CreateCcInfoRow(lv_obj_t* page, const char* text, uint32_t color, lv_coord_t y_ofs) {
        auto lbl = lv_label_create(page);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, y_ofs);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    }

    void EnsureBatteryMonitor() {
        if (battery_monitor_ == nullptr) {
            battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, BAT_ADC_CHANNEL,
                                                     BAT_ADC_UPPER_RESISTOR, BAT_ADC_LOWER_RESISTOR);
        }
    }

    std::string BuildWifiLine() {
        std::string ssid = "--";
        std::string rssi_str = "--";
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ssid = reinterpret_cast<const char*>(ap.ssid);
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "%d dBm", ap.rssi);
            rssi_str = rbuf;
        }
        std::string ip_str = "--";
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ipbuf[16];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
            ip_str = ipbuf;
        }
        return "WiFi " + ssid + " · " + rssi_str + "\nIP " + ip_str;
    }

    // 网络页（原厂 Data 页行式，文案中文）：内存/缓存实测 + WiFi 状态 + 重新配网按钮
    void OpenDataPage() {
        DeletePage(cc_data_page_);
        auto page = CreateCcPage();
        cc_data_page_ = page;
        CreateCcTitle(page, "网络");
        // RAM/PSRAM 都由 heap_caps 实测，不写死标称值（对齐原厂「data 里的数据要真实」）
        uint32_t ram_free = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        uint32_t ram_total = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
        uint32_t psram_mb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
        char l1[48], l2[48], l3[48];
        snprintf(l1, sizeof(l1), "内存 %lu / %lu KB", (unsigned long)ram_free, (unsigned long)ram_total);
        snprintf(l2, sizeof(l2), "缓存 %lu MB", (unsigned long)psram_mb);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(l3, sizeof(l3), "WiFi %.20s %d dBm", (const char*)ap.ssid, ap.rssi);
        } else {
            snprintf(l3, sizeof(l3), "WiFi --");
        }
        const char* lines[3] = { l1, l2, l3 };
        for (int i = 0; i < 3; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -48 + i * 40);   // 行距 40px，整块居中（原厂同口径）
        }

        auto btn = lv_button_create(page);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3C), kSelPressed());
        lv_obj_add_event_cb(btn, OnNetConfig, LV_EVENT_CLICKED, this);
        auto lbl2 = lv_label_create(btn);
        lv_label_set_text(lbl2, "重新配网");
        lv_obj_set_style_text_font(lbl2, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl2, lv_color_white(), 0);
        lv_obj_center(lbl2);

        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_data_page_); });
    }

    // 电量页（原厂 Battery 页行式，文案中文）："电量 n%%" + 充电状态（无 GetVoltage 接口，不显电压行）
    void OpenBatteryPage() {
        DeletePage(cc_battery_page_);
        EnsureBatteryMonitor();
        auto page = CreateCcPage();
        cc_battery_page_ = page;
        CreateCcTitle(page, "电池");
        unsigned level = battery_monitor_->GetBatteryLevel();
        char l1[32], l2[32];
        snprintf(l1, sizeof(l1), "电量 %u%%", level);
        snprintf(l2, sizeof(l2), "%s", battery_monitor_->IsCharging() ? "充电中" : "电池供电");
        const char* lines[2] = { l1, l2 };
        for (int i = 0; i < 2; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -28 + i * 40);   // 与数据页行距口径一致
        }
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_battery_page_); });
    }

    // 关于页（原厂 About 页行式）：name / v%s / 编译日期 时间，行距 34px
    void OpenAboutPage() {
        DeletePage(cc_about_page_);
        auto page = CreateCcPage();
        cc_about_page_ = page;
        CreateCcTitle(page, "关于");
        const esp_app_desc_t* d = esp_app_get_description();
        char l2[64], l3[64];
        const char* l1 = "sdgoods-xiaozhi";   // 固定展示移植标识，不用上游 project_name "xiaozhi"
        snprintf(l2, sizeof(l2), "v%s", d->version);
        snprintf(l3, sizeof(l3), "%s %s", d->date, d->time);
        const char* lines[3] = { l1, l2, l3 };
        for (int i = 0; i < 3; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -64 + i * 34);   // 整块视觉居中（原厂同款）
        }
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_about_page_); });
    }

    void CloseControlCenter() {
        DeleteAllCcPages();
    }

    std::string BuildInfoText() {
        char buf[224];
        const char* version = esp_app_get_description()->version;

        std::string ip_str = "--";
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ipbuf[16];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
            ip_str = ipbuf;
        }

        std::string rssi_str = "--";
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "%d dBm", ap.rssi);
            rssi_str = rbuf;
        }

        unsigned heap_kb = static_cast<unsigned>(SystemInfo::GetFreeHeapSize() / 1024);
        snprintf(buf, sizeof(buf), "固件 v%s\nIP %s · WiFi %s\nMAC %s · 内存 %uKB",
                 version, ip_str.c_str(), rssi_str.c_str(),
                 SystemInfo::GetMacAddress().c_str(), heap_kb);
        return buf;
    }

public:
    SdgoodsDimensionScreen() :
        boot_button_(BOOT_BUTTON_GPIO, false, 1500) {
        InitializePowerLatch();
        InitializeDisplayPower();
        InitializeSpi();
        Initializest77916Display();
        InitializeI2c();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SdgoodsAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_MIC_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_SPK_GPIO_MCLK,
            AUDIO_PA_GPIO);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SdgoodsDimensionScreen);
