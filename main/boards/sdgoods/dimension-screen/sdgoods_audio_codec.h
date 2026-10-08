#ifndef _SDGOODS_AUDIO_CODEC_H
#define _SDGOODS_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <mutex>

// 谷仓次元屏板载音频（无 codec 芯片）：
//   TX: I2S 控制器 0，16bit 立体声 + MCLK（功放需要 MCLK 时基），PA 由 GPIO 控制
//   RX: I2S 控制器 1，32bit 单声道左槽（MEMS 数字麦，24bit 数据左对齐于 32bit 槽）
// 参数取自 SDGOODS-ESP32S3 板级驱动 sdgoods_audio.c 的实机验证配置。
class SdgoodsAudioCodec : public AudioCodec {
public:
    SdgoodsAudioCodec(int input_sample_rate, int output_sample_rate,
        gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din,
        gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t spk_mclk,
        gpio_num_t pa_gpio);
    virtual ~SdgoodsAudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

protected:
    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;

private:
    std::mutex data_if_mutex_;
    gpio_num_t pa_gpio_;
};

#endif // _SDGOODS_AUDIO_CODEC_H
