// ============================================================
//  AUDIO OUTPUT
//  ES8388 codec (I2C control) + I2S TX via esp_codec_dev, with a
//  3-band equalizer in front. Pins: Ai-Thinker ESP32 Audio Kit v2.2.
// ============================================================
#include "app_state.h"

#include <math.h>
#include <string.h>
#include <algorithm>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "es8388_codec.h"

static const char* TAG = "audio_out";

// ---- Ai-Thinker ESP32 Audio Kit (ES8388) pins ----
static const gpio_num_t PIN_I2C_SCL  = GPIO_NUM_32;
static const gpio_num_t PIN_I2C_SDA  = GPIO_NUM_33;
static const gpio_num_t PIN_I2S_MCLK = GPIO_NUM_0;
static const gpio_num_t PIN_I2S_BCLK = GPIO_NUM_27;
static const gpio_num_t PIN_I2S_WS   = GPIO_NUM_25;
static const gpio_num_t PIN_I2S_DOUT = GPIO_NUM_26;
static const int        PIN_PA_EN    = 21;

static const uint32_t DEFAULT_SAMPLE_RATE = 44100;
static const size_t   SCRATCH_FRAMES = 1024;

static esp_codec_dev_handle_t s_codec = nullptr;
static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_sampleRate = 0;
static int s_volume = 50;
static int16_t* s_scratch = nullptr;

// ============================================================
//  3-BAND EQUALIZER
//  Same algorithm as arduino-audio-tools Equalizer3Bands
//  (musicdsp.org "3 band equaliser"), 880 Hz / 5 kHz split.
// ============================================================
struct EqChannel {
  float f1p0, f1p1, f1p2, f1p3;
  float f2p0, f2p1, f2p2, f2p3;
  float sdm1, sdm2, sdm3;
};

static const float EQ_FREQ_LOW = 880.0f;
static const float EQ_FREQ_HIGH = 5000.0f;
static const float EQ_VSA = 1.0f / 4294967295.0f;  // denormal guard

static EqChannel s_eq[2];
static float s_eqLf = 0, s_eqHf = 0;
static volatile float s_gainLow = 1.0f;
static volatile float s_gainHigh = 1.0f;
static volatile bool s_eqEnabled = false;

static void eqConfigure(uint32_t sampleRate) {
  memset(s_eq, 0, sizeof(s_eq));
  s_eqLf = 2.0f * sinf((float)M_PI * (EQ_FREQ_LOW / (float)sampleRate));
  s_eqHf = 2.0f * sinf((float)M_PI * (EQ_FREQ_HIGH / (float)sampleRate));
}

static inline int16_t eqSample(EqChannel& es, float sample, float lowGain, float highGain) {
  es.f1p0 += (s_eqLf * (sample - es.f1p0)) + EQ_VSA;
  es.f1p1 += (s_eqLf * (es.f1p0 - es.f1p1));
  es.f1p2 += (s_eqLf * (es.f1p1 - es.f1p2));
  es.f1p3 += (s_eqLf * (es.f1p2 - es.f1p3));
  float l = es.f1p3;

  es.f2p0 += (s_eqHf * (sample - es.f2p0)) + EQ_VSA;
  es.f2p1 += (s_eqHf * (es.f2p0 - es.f2p1));
  es.f2p2 += (s_eqHf * (es.f2p1 - es.f2p2));
  es.f2p3 += (s_eqHf * (es.f2p2 - es.f2p3));
  float h = es.sdm3 - es.f2p3;
  float m = es.sdm3 - (h + l);

  es.sdm3 = es.sdm2;
  es.sdm2 = es.sdm1;
  es.sdm1 = sample;

  float outValue = l * lowGain + m + h * highGain;
  if (outValue > 32767.0f) outValue = 32767.0f;
  if (outValue < -32768.0f) outValue = -32768.0f;
  return (int16_t)outValue;
}

void audioOutputSetEq(int bass, int treble) {
  bass = std::clamp(bass, -10, 10);
  treble = std::clamp(treble, -10, 10);
  s_gainLow = 1.0f + (bass / 10.0f);
  s_gainHigh = 1.0f + (treble / 10.0f);
  s_eqEnabled = (bass != 0 || treble != 0);
}

// ============================================================
//  CODEC / I2S
// ============================================================
static bool openCodec(uint32_t sampleRate) {
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 2;
  fs.channel_mask = 0;
  fs.sample_rate = sampleRate;
  fs.mclk_multiple = 0;

  if (s_sampleRate != 0) {
    esp_codec_dev_close(s_codec);
  }
  if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "codec open at %lu Hz failed", (unsigned long)sampleRate);
    s_sampleRate = 0;
    return false;
  }
  s_sampleRate = sampleRate;
  esp_codec_dev_set_out_vol(s_codec, s_volume);
  esp_codec_dev_set_out_mute(s_codec, s_volume == 0);
  eqConfigure(sampleRate);
  ESP_LOGI(TAG, "output %lu Hz, 16-bit stereo", (unsigned long)sampleRate);
  return true;
}

bool audioOutputInit() {
  if (s_codec != nullptr) return true;

  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port = I2C_NUM_0;
  busCfg.sda_io_num = PIN_I2C_SDA;
  busCfg.scl_io_num = PIN_I2C_SCL;
  busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_new_master_bus(&busCfg, &bus) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus init failed");
    return false;
  }

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 8;
  chanCfg.dma_frame_num = 512;
  chanCfg.auto_clear = true;  // play silence instead of repeating stale DMA on underrun
  if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "I2S channel create failed");
    return false;
  }

  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_SAMPLE_RATE);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = PIN_I2S_MCLK;
  stdCfg.gpio_cfg.bclk = PIN_I2S_BCLK;
  stdCfg.gpio_cfg.ws = PIN_I2S_WS;
  stdCfg.gpio_cfg.dout = PIN_I2S_DOUT;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(s_tx, &stdCfg) != ESP_OK) {
    ESP_LOGE(TAG, "I2S std mode init failed");
    return false;
  }

  audio_codec_i2s_cfg_t i2sCfg = {};
  i2sCfg.port = I2S_NUM_0;
  i2sCfg.tx_handle = s_tx;
  i2sCfg.rx_handle = nullptr;
  const audio_codec_data_if_t* dataIf = audio_codec_new_i2s_data(&i2sCfg);

  audio_codec_i2c_cfg_t i2cCfg = {};
  i2cCfg.port = I2C_NUM_0;
  i2cCfg.addr = ES8388_CODEC_DEFAULT_ADDR;
  i2cCfg.bus_handle = bus;
  const audio_codec_ctrl_if_t* ctrlIf = audio_codec_new_i2c_ctrl(&i2cCfg);
  const audio_codec_gpio_if_t* gpioIf = audio_codec_new_gpio();

  es8388_codec_cfg_t esCfg = {};
  esCfg.ctrl_if = ctrlIf;
  esCfg.gpio_if = gpioIf;
  esCfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  esCfg.master_mode = false;
  esCfg.pa_pin = PIN_PA_EN;
  esCfg.pa_reverted = false;
  const audio_codec_if_t* codecIf = es8388_codec_new(&esCfg);
  if (dataIf == nullptr || ctrlIf == nullptr || codecIf == nullptr) {
    ESP_LOGE(TAG, "ES8388 interface init failed");
    return false;
  }

  esp_codec_dev_cfg_t devCfg = {};
  devCfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  devCfg.codec_if = codecIf;
  devCfg.data_if = dataIf;
  s_codec = esp_codec_dev_new(&devCfg);
  if (s_codec == nullptr) {
    ESP_LOGE(TAG, "codec device create failed");
    return false;
  }
  // Keep the codec powered across sample-rate changes (avoids pops).
  esp_codec_set_disable_when_closed(s_codec, false);

  s_scratch = (int16_t*)heap_caps_malloc(SCRATCH_FRAMES * 2 * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (s_scratch == nullptr) {
    ESP_LOGE(TAG, "scratch buffer allocation failed");
    return false;
  }

  s_volume = std::clamp(globalSettings.volume, 0, 100);
  audioOutputSetEq(globalSettings.bass, globalSettings.treble);
  return openCodec(DEFAULT_SAMPLE_RATE);
}

void audioOutputSetVolume(int volume) {
  s_volume = std::clamp(volume, 0, 100);
  if (s_codec == nullptr || s_sampleRate == 0) return;
  esp_codec_dev_set_out_vol(s_codec, s_volume);
  esp_codec_dev_set_out_mute(s_codec, s_volume == 0);
}

void audioOutputWrite(const int16_t* stereo, size_t frames, uint32_t sampleRate) {
  if (s_codec == nullptr || frames == 0) return;
  if (sampleRate != s_sampleRate && !openCodec(sampleRate)) return;

  bool eqOn = s_eqEnabled;
  float lowGain = s_gainLow;
  float highGain = s_gainHigh;

  while (frames > 0) {
    size_t n = std::min(frames, SCRATCH_FRAMES);
    if (eqOn) {
      for (size_t i = 0; i < n; i++) {
        s_scratch[i * 2] = eqSample(s_eq[0], stereo[i * 2], lowGain, highGain);
        s_scratch[i * 2 + 1] = eqSample(s_eq[1], stereo[i * 2 + 1], lowGain, highGain);
      }
    } else {
      memcpy(s_scratch, stereo, n * 2 * sizeof(int16_t));
    }
    esp_codec_dev_write(s_codec, s_scratch, (int)(n * 2 * sizeof(int16_t)));
    stereo += n * 2;
    frames -= n;
  }
}

void audioOutputSilence() {
  if (s_codec == nullptr || s_sampleRate == 0) return;
  // Flush the DMA ring with zeros so a stopped stream does not leave a tail.
  memset(s_scratch, 0, SCRATCH_FRAMES * 2 * sizeof(int16_t));
  esp_codec_dev_write(s_codec, s_scratch, (int)(SCRATCH_FRAMES * 2 * sizeof(int16_t)));
}
