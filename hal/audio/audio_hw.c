/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2013-2015 The CyanogenMod Project
 *               Daniel Hillenbrand <codeworkx@cyanogenmod.com>
 *               Guillaume "XpLoDWilD" Lesniak <xplodgui@gmail.com>
 * Copyright (c) 2015-2017 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2015-2017 Christopher N. Hesse <raymanfx@gmail.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0
//#define ALOG_TRACE 1

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <linux/videodev2.h>
#include <linux/videodev2_exynos_media.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>
#include <audio_route/audio_route.h>

#include "routing.h"
#include "ril_interface.h"

#ifdef ALOG_TRACE
#define ALOGT ALOGV
#else
#define ALOGT(a...) do { } while(0)
#endif

#define PCM_CARD 0
#define PCM_CARD_SPDIF 1
#define PCM_TOTAL 2

#define PCM_DEVICE 0       /* Playback link */
#define PCM_DEVICE_VOICE 1 /* Baseband link */
#define PCM_DEVICE_SCO 2   /* Bluetooth link */
#define PCM_DEVICE_DEEP 3  /* Deep buffer */

#define MIXER_CARD 0

/* duration in ms of volume ramp applied when starting capture to remove plop */
#define CAPTURE_START_RAMP_MS 100

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define DEVICE_DAPM_PATH "/d/asoc/Pacific WM5110 Sound/dapm"
#define CODEC_DAPM_PATH "/d/asoc/Pacific WM5110 Sound/florida-codec/dapm"

/*
 * Set the deep-buffer and low-latency output buffer sizes to
 * integral multiple of msec. This reduces the variations in the writes.
 */
#define DEEP_BUFFER_OUTPUT_PERIOD_SIZE 960
#define DEEP_BUFFER_OUTPUT_PERIOD_COUNT 5

#define LOW_LATENCY_OUTPUT_PERIOD_SIZE 240
#define LOW_LATENCY_OUTPUT_PERIOD_COUNT 2

#define AUDIO_CAPTURE_PERIOD_SIZE 320
#define AUDIO_CAPTURE_PERIOD_COUNT 2

#define AUDIO_CAPTURE_LOW_LATENCY_PERIOD_SIZE 240
#define AUDIO_CAPTURE_LOW_LATENCY_PERIOD_COUNT 2

#define SCO_CAPTURE_PERIOD_SIZE 240
#define SCO_CAPTURE_PERIOD_COUNT 2

#define HDMI_MULTI_PERIOD_SIZE  336
#define HDMI_MULTI_PERIOD_COUNT 8
#define HDMI_MULTI_DEFAULT_CHANNEL_COUNT 6 /* 5.1 */
#define HDMI_MULTI_DEFAULT_SAMPLING_RATE 48000
/*
 * Default sampling for HDMI multichannel output
 *
 * Maximum number of channel mask configurations supported. Currently the
 * primary output only supports 1 (stereo) and the
 * multi channel HDMI output 2 (5.1 and 7.1)
 */
#define HDMI_MAX_SUPPORTED_CHANNEL_MASKS 2


struct pcm_config pcm_config_fast = {
    .channels = 2,
    .rate = 48000,
    .period_size = LOW_LATENCY_OUTPUT_PERIOD_SIZE,
    .period_count = LOW_LATENCY_OUTPUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_deep = {
    .channels = 2,
    .rate = 48000,
    .period_size = DEEP_BUFFER_OUTPUT_PERIOD_SIZE,
    .period_count = DEEP_BUFFER_OUTPUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 48000,
    .period_size = AUDIO_CAPTURE_PERIOD_SIZE,
    .period_count = AUDIO_CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in_low_latency = {
    .channels = 2,
    .rate = 48000,
    .period_size = AUDIO_CAPTURE_LOW_LATENCY_PERIOD_SIZE,
    .period_count = AUDIO_CAPTURE_LOW_LATENCY_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = 8000,
    .period_size = SCO_CAPTURE_PERIOD_SIZE,
    .period_count = SCO_CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_voice = {
    .channels = 2,
    .rate = 8000,
    .period_size = AUDIO_CAPTURE_PERIOD_SIZE,
    .period_count = AUDIO_CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_voice_wide = {
    .channels = 2,
    .rate = 16000,
    .period_size = AUDIO_CAPTURE_PERIOD_SIZE,
    .period_count = AUDIO_CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_hdmi_multi = {
    .channels = HDMI_MULTI_DEFAULT_CHANNEL_COUNT,
    .rate = HDMI_MULTI_DEFAULT_SAMPLING_RATE,
    .period_size = HDMI_MULTI_PERIOD_SIZE,
    .period_count = HDMI_MULTI_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

enum output_type {
    OUTPUT_DEEP_BUF,      // deep PCM buffers output stream
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_HDMI,          // HDMI multi channel
    OUTPUT_TOTAL
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    audio_devices_t out_device; /* "or" of stream_out.device for all active output streams */
    audio_devices_t in_device;
    bool mic_mute;
    audio_source_t input_source;
    int cur_route_id;     /* current route ID: combination of input source
                           * and output device IDs */
    audio_mode_t mode;

    struct audio_route *audio_route;
    struct {
        const char *device;
        const char *route;
        int dev_id;
    } active_output;
    struct {
        const char *device;
        const char *route;
    } active_input;

    /* Call audio */
    struct pcm *pcm_voice_rx;
    struct pcm *pcm_voice_tx;

    /* SCO audio */
    struct pcm *pcm_sco_rx;
    struct pcm *pcm_sco_tx;

    float voice_volume;
    bool in_call;
    bool tty_mode;
    bool bluetooth_nrec;
    bool wb_amr;
    bool two_mic_control;

    int hdmi_drv_fd;
    audio_channel_mask_t in_channel_mask;

    /* RIL */
    struct ril_handle ril;

    struct stream_out *outputs[OUTPUT_TOTAL];
    pthread_mutex_t lock_outputs; /* see note below on mutex acquisition order */
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock;   /* acquire before lock to prevent playback thread DOS */
    struct pcm *pcm[PCM_TOTAL];
    struct pcm_config config;
    unsigned int pcm_device;
    bool standby; /* true if all PCMs are inactive */
    audio_devices_t device;
    /* FIXME: when HDMI multichannel output is active, other outputs must be disabled as
     * HDMI and WM1811 share the same I2S. This means that notifications and other sounds are
     * silent when watching a 5.1 movie. */
    bool disabled;

    audio_channel_mask_t channel_mask;
    /* Array of supported channel mask configurations. +1 so that the last entry is always 0 */
    audio_channel_mask_t supported_channel_masks[HDMI_MAX_SUPPORTED_CHANNEL_MASKS + 1];
    bool muted;
    uint64_t written; /* total frames written, not cleared when entering standby */
    int64_t last_write_time_us;

    struct audio_device *dev;
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock;   /* acquire before lock to prevent playback thread DOS */
    struct pcm *pcm;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int64_t frames_read; /* total frames read, not cleared when entering standby */
    int64_t last_read_time_us;
    int read_status;

    audio_source_t input_source;
    audio_io_handle_t io_handle;
    audio_devices_t device;

    uint16_t ramp_vol;
    uint16_t ramp_step;
    size_t ramp_frames;

    audio_channel_mask_t channel_mask;
    audio_input_flags_t flags;
    struct pcm_config *config;

    struct audio_device *dev;
};

#define STRING_TO_ENUM(string) { #string, string }

struct string_to_enum {
    const char *name;
    uint32_t value;
};

const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};

static int get_output_device_id(audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE)
        return OUT_DEVICE_NONE;

    if (popcount(device) == 2) {
        if ((device == (AUDIO_DEVICE_OUT_SPEAKER |
                        AUDIO_DEVICE_OUT_WIRED_HEADSET)) ||
            (device == (AUDIO_DEVICE_OUT_SPEAKER |
                        AUDIO_DEVICE_OUT_WIRED_HEADPHONE))) {
            return OUT_DEVICE_SPEAKER_AND_HEADSET;
        } else if (device == (AUDIO_DEVICE_OUT_SPEAKER |
                              AUDIO_DEVICE_OUT_EARPIECE)) {
            return OUT_DEVICE_SPEAKER_AND_EARPIECE;
        } else {
            return OUT_DEVICE_NONE;
        }
    }

    if (popcount(device) != 1)
        return OUT_DEVICE_NONE;

    switch (device) {
    case AUDIO_DEVICE_OUT_SPEAKER:
        return OUT_DEVICE_SPEAKER;
    case AUDIO_DEVICE_OUT_EARPIECE:
        return OUT_DEVICE_EARPIECE;
    case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        return OUT_DEVICE_HEADSET;
    case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        return OUT_DEVICE_HEADPHONES;
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        return OUT_DEVICE_BT_SCO;
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        return OUT_DEVICE_BT_SCO_HEADSET_OUT;
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
        return OUT_DEVICE_BT_SCO_CARKIT;
    default:
        return OUT_DEVICE_NONE;
    }
}

static int get_input_source_id(audio_source_t source, bool wb_amr)
{
    switch (source) {
    case AUDIO_SOURCE_DEFAULT:
        return IN_SOURCE_NONE;
    case AUDIO_SOURCE_MIC:
        return IN_SOURCE_MIC;
    case AUDIO_SOURCE_CAMCORDER:
        return IN_SOURCE_CAMCORDER;
    case AUDIO_SOURCE_VOICE_RECOGNITION:
        return IN_SOURCE_VOICE_RECOGNITION;
    case AUDIO_SOURCE_VOICE_COMMUNICATION:
        return IN_SOURCE_VOICE_COMMUNICATION;
    case AUDIO_SOURCE_VOICE_CALL:
        if (wb_amr) {
            return IN_SOURCE_VOICE_CALL_WB;
        }
        return IN_SOURCE_VOICE_CALL;
    default:
        return IN_SOURCE_NONE;
    }
}

static void do_out_standby(struct stream_out *out);
static void adev_set_call_audio_path(struct audio_device *adev);
static int voice_set_volume(struct audio_hw_device *dev, float volume);
static void start_ril_call(struct audio_device *adev);

static void lock_input_stream(struct stream_in *in);
static void unlock_input_stream(struct stream_in *in);
static void lock_output_stream(struct stream_out *out);
static void unlock_output_stream(struct stream_out *out);

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the
 * following order: hw device > in stream > out stream
 */

/* Helper functions */

static int open_hdmi_driver(struct audio_device *adev)
{
    if (adev->hdmi_drv_fd < 0) {
        adev->hdmi_drv_fd = open("/dev/video16", O_RDWR);
        if (adev->hdmi_drv_fd < 0)
            ALOGE("%s cannot open video16 - error: %s\n",
                  __func__, strerror(errno));
    }
    return adev->hdmi_drv_fd;
}

/* must be called with hw device mutex locked */
static int enable_hdmi_audio(struct audio_device *adev, int enable)
{
    int ret;
    struct v4l2_control ctrl;

    ret = open_hdmi_driver(adev);
    if (ret < 0) {
        return ret;
    }

    ctrl.id = V4L2_CID_TV_ENABLE_HDMI_AUDIO;
    ctrl.value = !!enable;
    ret = ioctl(adev->hdmi_drv_fd, VIDIOC_S_CTRL, &ctrl);

    if (ret < 0) {
        ALOGE("V4L2_CID_TV_ENABLE_HDMI_AUDIO ioctl error (%d)", errno);
    }

    return ret;
}

/* must be called with hw device mutex locked */
static int read_hdmi_channel_masks(struct audio_device *adev, struct stream_out *out) {
    int ret;
    struct v4l2_control ctrl;

    ret = open_hdmi_driver(adev);
    if (ret < 0)
        return ret;

    ctrl.id = V4L2_CID_TV_MAX_AUDIO_CHANNELS;
    ret = ioctl(adev->hdmi_drv_fd, VIDIOC_G_CTRL, &ctrl);
    if (ret < 0) {
        ALOGE("V4L2_CID_TV_MAX_AUDIO_CHANNELS ioctl error (%d)", errno);
        return ret;
    }

    ALOGV("%s ioctl %d got %d max channels", __func__, ret, ctrl.value);

    if (ctrl.value != 6 && ctrl.value != 8)
        return -ENOSYS;

    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
    if (ctrl.value == 8)
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_7POINT1;

    return ret;
}

/* must be called with hw device mutex locked */
static int set_hdmi_channels(struct audio_device *adev, int channels) {
    int ret;
    struct v4l2_control ctrl;

    ret = open_hdmi_driver(adev);
    if (ret < 0)
        return ret;

    ctrl.id = V4L2_CID_TV_SET_NUM_CHANNELS;
    ctrl.value = channels;
    ret = ioctl(adev->hdmi_drv_fd, VIDIOC_S_CTRL, &ctrl);
    if (ret < 0)
        ALOGE("V4L2_CID_TV_SET_NUM_CHANNELS ioctl error (%d)", errno);

    return ret;
}

static const char *audio_device_to_dapm(int out_device_id)
{
    switch (out_device_id) {
    case OUT_DEVICE_SPEAKER_AND_EARPIECE:
    case OUT_DEVICE_SPEAKER_AND_HEADSET:
    case OUT_DEVICE_EARPIECE:
        return "RCV";
    case OUT_DEVICE_SPEAKER:
        return "SPK";
    case OUT_DEVICE_HEADSET:
    case OUT_DEVICE_HEADPHONES:
        return "HP";
    }

    return NULL;
}

static void output_device_off(int out_device_id)
{
    char *state = "Off";
    const char *device;
    char dapm[64] = {0};
    bool ok = false;
    int i;

    device = audio_device_to_dapm(out_device_id);
    if (device == NULL) {
        goto out;
    }
    snprintf(dapm, sizeof(dapm), "%s/%s", DEVICE_DAPM_PATH, device);

    ALOGV("%s: Check if %s is turned off\n", __func__, device);

    for (i = 0; i < 20; i++) {
        const char *p;
        char line[32] = {0};
        FILE *fp;

        fp = fopen(dapm, "r");
        if (fp == NULL) {
            ALOGE("%s: Failed to open %s\n", __func__, dapm);
            break;
        }

        p = fgets(line, sizeof(line), fp);
        fclose(fp);
        if (p == NULL) {
            break;
        }

        p = strstr(line, state);
        if (p != NULL) {
            ok = true;
            break;
        }
        usleep(5);
    }
out:
    if (ok) {
        ALOGV("%s: Output device %s turned off!\n", __func__, device);
    } else {
        ALOGE("%s: Failed to wait for %s to turn off", __func__, device);
        usleep(50);
    }
}

const char *audio_inputs[] = {
    "Headset Mic",
    "Main Mic",
    "Sub Mic",
    "Third Mic"
};

static void input_devices_off(void)
{
    char *state = "Off";
    bool ok = false;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(audio_inputs); i++) {
        char dapm[64] = {0};
        int j;

        snprintf(dapm, sizeof(dapm), "%s/%s", DEVICE_DAPM_PATH, audio_inputs[i]);

        for (j = 0; j < 20; j++) {
            const char *p;
            char line[32] = {0};
            FILE *fp;

            fp = fopen(dapm, "r");
            if (fp == NULL) {
                ALOGE("%s: Failed to open %s\n", __func__, dapm);
                break;
            }

            p = fgets(line, sizeof(line), fp);
            fclose(fp);
            if (p == NULL) {
                break;
            }

            p = strstr(line, state);
            if (p != NULL) {
                ok = true;
                break;
            }
            usleep(5);
        }
    }
out:
    if (ok) {
        ALOGV("%s: Input devices turned off!\n", __func__);
    } else {
        ALOGE("%s: Failed to wait for device to turn off", __func__);
        usleep(50);
    }
}

const char *audio_codecs[] = {
    "DSP1",
    "DSP2",
    "DSP3",
    "DSP4"
};

static void codecs_off(void)
{
    char *state = "Off";
    bool ok = false;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(audio_codecs); i++) {
        char dapm[64] = {0};
        int j;

        snprintf(dapm, sizeof(dapm), "%s/%s", CODEC_DAPM_PATH, audio_codecs[i]);

        for (j = 0; j < 20; j++) {
            const char *p;
            char line[32] = {0};
            FILE *fp;

            fp = fopen(dapm, "r");
            if (fp == NULL) {
                ALOGE("%s: Failed to open %s - %s\n",
                      __func__,
                      dapm,
                      strerror(errno));
                break;
            }

            p = fgets(line, sizeof(line), fp);
            fclose(fp);
            if (p == NULL) {
                break;
            }

            p = strstr(line, state);
            if (p != NULL) {
                ok = true;
                break;
            }
            usleep(5);
        }
    }
out:
    if (ok) {
        ALOGV("%s: DSPs turned off!\n", __func__);
    } else {
        ALOGE("%s: Failed to wait for codec to turn off", __func__);
        usleep(50);
    }
}

static void enable_audio_route(struct audio_device *adev,
                               const char *mixer_path)
{
    ALOGV("%s: %s\n", __func__, mixer_path);

    audio_route_apply_and_update_path(adev->audio_route, mixer_path);
}

static void disable_audio_route(struct audio_device *adev,
                                const char *mixer_path)
{
    ALOGV("%s: %s\n", __func__, mixer_path);

    audio_route_reset_and_update_path(adev->audio_route, mixer_path);
}

static void enable_audio_device(struct audio_device *adev,
                                 const char *device)
{
    ALOGV("%s: %s\n", __func__, device);

    audio_route_apply_and_update_path(adev->audio_route, device);
}

static void disable_audio_device(struct audio_device *adev,
                                 const char *device)
{
    ALOGV("%s: %s\n", __func__, device);

    audio_route_reset_and_update_path(adev->audio_route, device);
}

static bool route_changed(struct audio_device *adev)
{
    int output_device_id = get_output_device_id(adev->out_device);
    int input_source_id = get_input_source_id(adev->input_source, adev->wb_amr);
    int new_route_id;

    new_route_id = (1 << (input_source_id + OUT_DEVICE_CNT)) + (1 << output_device_id);
    return new_route_id != adev->cur_route_id;
}

static void select_devices(struct audio_device *adev)
{
    int output_device_id = get_output_device_id(adev->out_device);
    int input_source_id = get_input_source_id(adev->input_source, adev->wb_amr);
    const char *output_route = NULL;
    const char *output_device = NULL;
    const char *input_route = NULL;
    const char *input_device = NULL;
    char current_device[64] = {0};
    int new_route_id;

#ifndef HDMI_INCAPABLE
    enable_hdmi_audio(adev, adev->out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL);
#endif

    new_route_id = (1 << (input_source_id + OUT_DEVICE_CNT)) + (1 << output_device_id);
    if (new_route_id == adev->cur_route_id) {
        ALOGV("*** %s: Routing hasn't changed, leaving function.", __func__);
        return;
    }

    adev->cur_route_id = new_route_id;

    if (input_source_id != IN_SOURCE_NONE) {
        if (output_device_id != OUT_DEVICE_NONE) {
            input_route =
                    route_configs[input_source_id][output_device_id]->input_route;
            input_device =
                    route_configs[input_source_id][output_device_id]->input_device;
            output_route =
                    route_configs[input_source_id][output_device_id]->output_route;
            output_device =
                    route_configs[input_source_id][output_device_id]->output_device;
        } else {
            switch (adev->in_device) {
            case AUDIO_DEVICE_IN_WIRED_HEADSET & ~AUDIO_DEVICE_BIT_IN:
                output_device_id = OUT_DEVICE_HEADSET;
                break;
            case AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET & ~AUDIO_DEVICE_BIT_IN:
                output_device_id = OUT_DEVICE_BT_SCO_HEADSET_OUT;
                break;
            default:
                if (adev->input_source == AUDIO_SOURCE_VOICE_CALL) {
                    output_device_id = OUT_DEVICE_EARPIECE;
                } else {
                    output_device_id = OUT_DEVICE_SPEAKER;
                }
                break;
            }

            input_route =
                (route_configs[input_source_id][output_device_id])->input_route;
            input_device =
                (route_configs[input_source_id][output_device_id])->input_device;
        }
    } else {
        if (output_device_id != OUT_DEVICE_NONE) {
            output_route =
                    (route_configs[IN_SOURCE_MIC][output_device_id])->output_route;
            output_device =
                    (route_configs[IN_SOURCE_MIC][output_device_id])->output_device;
        }
    }

    ALOGV("***** %s: devices=%#x, input src=%d -> "
          "output route: %s, input route: %s",
          __func__,
          adev->out_device, adev->input_source,
          output_route ? output_route : "none",
          input_route ? input_route : "none");

    /*
     * The Arizona driver documentation describes firmware loading this way:
     *
     * To load a firmware, or to reboot the ADSP with different firmware you
     * must:
     * - Disconnect the ADSP from any active audio path so that it will be
     *   powered-down
     * - Set the firmware control to the firmware you want to load
     * - Connect the ADSP to an active audio path so it will be powered-up
     */

    /*
     * Disable the output and input device
     */
    if (adev->active_output.route != NULL) {
        disable_audio_route(adev, adev->active_output.route);
    }
    if (adev->active_input.route != NULL) {
        disable_audio_route(adev, adev->active_input.route);
    }
    codecs_off();

    if (adev->active_output.device != NULL) {
        disable_audio_device(adev, adev->active_output.device);
        output_device_off(adev->active_output.dev_id);
    }

    if (adev->active_input.device != NULL) {
        disable_audio_device(adev, adev->active_input.device);
        input_devices_off();
    }

    /*
     * Already tell the modem that we are in a call. This should make it
     * faster to accept an incoming call.
     */
    if (adev->in_call) {
        start_ril_call(adev);
    }

    /*
     * Apply the new audio routes
     */

    /* OUTPUT */
    if (output_route != NULL) {
        enable_audio_route(adev, output_route);
        adev->active_output.route = output_route;
    } else {
        adev->active_output.route = NULL;
    }

    if (output_device != NULL) {
        enable_audio_device(adev, output_device);
        adev->active_output.device = output_device;
        adev->active_output.dev_id = output_device_id;
    } else {
        adev->active_output.device = NULL;
        adev->active_output.dev_id = -1;
    }

    /* INPUT */
    if (input_route != NULL) {
        enable_audio_route(adev, input_route);
        adev->active_input.route = input_route;
    } else {
        adev->active_input.route = NULL;
    }

    if (input_device != NULL) {
        enable_audio_device(adev, input_device);
        adev->active_input.device = input_device;
    } else {
        adev->active_input.device = NULL;
    }
}

static void force_non_hdmi_out_standby(struct audio_device *adev)
{
    enum output_type type;
    struct stream_out *out;

    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        out = adev->outputs[type];
        if (type == OUTPUT_HDMI || !out)
            continue;
        lock_output_stream(out);
        do_out_standby(out);
        unlock_output_stream(out);
    }
}

/**********************************************************
 * BT SCO functions
 **********************************************************/

/* must be called with the hw device mutex locked, OK to hold other mutexes */
static void start_bt_sco(struct audio_device *adev)
{
    if (adev->pcm_sco_rx != NULL || adev->pcm_sco_tx != NULL) {
        ALOGW("%s: SCO PCMs already open!\n", __func__);
        return;
    }

    ALOGV("%s: Opening SCO PCMs", __func__);

    adev->pcm_sco_rx = pcm_open(PCM_CARD,
                                PCM_DEVICE_SCO,
                                PCM_OUT | PCM_MONOTONIC,
                                &pcm_config_sco);
    if (adev->pcm_sco_rx != NULL && !pcm_is_ready(adev->pcm_sco_rx)) {
        ALOGE("%s: cannot open PCM SCO RX stream: %s",
              __func__, pcm_get_error(adev->pcm_sco_rx));
        goto err_sco_rx;
    }

    adev->pcm_sco_tx = pcm_open(PCM_CARD,
                                PCM_DEVICE_SCO,
                                PCM_IN | PCM_MONOTONIC,
                                &pcm_config_sco);
    if (adev->pcm_sco_tx && !pcm_is_ready(adev->pcm_sco_tx)) {
        ALOGE("%s: cannot open PCM SCO TX stream: %s",
              __func__, pcm_get_error(adev->pcm_sco_tx));
        goto err_sco_tx;
    }

    pcm_start(adev->pcm_sco_rx);
    pcm_start(adev->pcm_sco_tx);

    return;

err_sco_tx:
    pcm_close(adev->pcm_sco_tx);
    adev->pcm_sco_tx = NULL;
err_sco_rx:
    pcm_close(adev->pcm_sco_rx);
    adev->pcm_sco_rx = NULL;
}

/* must be called with the hw device mutex locked, OK to hold other mutexes */
static void stop_bt_sco(struct audio_device *adev) {
    ALOGV("%s: Closing SCO PCMs", __func__);

    if (adev->pcm_sco_rx != NULL) {
        pcm_stop(adev->pcm_sco_rx);
        pcm_close(adev->pcm_sco_rx);
        adev->pcm_sco_rx = NULL;
    }

    if (adev->pcm_sco_tx != NULL) {
        pcm_stop(adev->pcm_sco_tx);
        pcm_close(adev->pcm_sco_tx);
        adev->pcm_sco_tx = NULL;
    }
}

/**********************************************************
 * Samsung RIL functions
 **********************************************************/

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
static int start_voice_call(struct audio_device *adev)
{
    struct pcm_config *voice_config;

    if (adev->pcm_voice_rx != NULL || adev->pcm_voice_tx != NULL) {
        ALOGW("%s: Voice PCMs already open!\n", __func__);
        return 0;
    }

    ALOGV("%s: Opening voice PCMs", __func__);

    if (adev->wb_amr) {
        voice_config = &pcm_config_voice_wide;
    } else {
        voice_config = &pcm_config_voice;
    }

    /* Open modem PCM channels */
    adev->pcm_voice_rx = pcm_open(PCM_CARD,
                                  PCM_DEVICE_VOICE,
                                  PCM_OUT | PCM_MONOTONIC,
                                  voice_config);
    if (adev->pcm_voice_rx != NULL && !pcm_is_ready(adev->pcm_voice_rx)) {
        ALOGE("%s: cannot open PCM voice RX stream: %s",
              __func__, pcm_get_error(adev->pcm_voice_rx));
        goto err_voice_rx;
    }

    adev->pcm_voice_tx = pcm_open(PCM_CARD,
                                  PCM_DEVICE_VOICE,
                                  PCM_IN | PCM_MONOTONIC,
                                  voice_config);
    if (adev->pcm_voice_tx != NULL && !pcm_is_ready(adev->pcm_voice_tx)) {
        ALOGE("%s: cannot open PCM voice TX stream: %s",
              __func__, pcm_get_error(adev->pcm_voice_tx));
        goto err_voice_tx;
    }

    pcm_start(adev->pcm_voice_rx);
    pcm_start(adev->pcm_voice_tx);

    /* start SCO stream if needed */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        start_bt_sco(adev);
    }

    return 0;

err_voice_tx:
    pcm_close(adev->pcm_voice_tx);
    adev->pcm_voice_tx = NULL;
err_voice_rx:
    pcm_close(adev->pcm_voice_rx);
    adev->pcm_voice_rx = NULL;

    return -ENOMEM;
}

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
static void stop_voice_call(struct audio_device *adev)
{
    int status = 0;

    ALOGV("%s: Closing active PCMs", __func__);

    if (adev->pcm_voice_rx) {
        pcm_stop(adev->pcm_voice_rx);
        pcm_close(adev->pcm_voice_rx);
        adev->pcm_voice_rx = NULL;
        status++;
    }

    if (adev->pcm_voice_tx) {
        pcm_stop(adev->pcm_voice_tx);
        pcm_close(adev->pcm_voice_tx);
        adev->pcm_voice_tx = NULL;
        status++;
    }

    /* End SCO stream if needed */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        stop_bt_sco(adev);
        status++;
    }

    ALOGV("%s: Successfully closed %d active PCMs", __func__, status);
}

static void start_ril_call(struct audio_device *adev)
{
    switch (adev->out_device) {
    case AUDIO_DEVICE_OUT_EARPIECE:
    case AUDIO_DEVICE_OUT_SPEAKER:
        adev->two_mic_control = true;
        break;
    default:
        adev->two_mic_control = false;
        break;
    }

    if (adev->two_mic_control) {
        ALOGV("%s: enabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_ON);
    } else {
        ALOGV("%s: disabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_OFF);
    }

    adev_set_call_audio_path(adev);
    voice_set_volume(&adev->hw_device, adev->voice_volume);

    ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_START);
}

static void start_call(struct audio_device *adev)
{
    if (adev->in_call) {
        return;
    }

    adev->in_call = true;

    if (adev->out_device == AUDIO_DEVICE_NONE &&
        adev->in_device == AUDIO_DEVICE_NONE) {
        ALOGV("%s: No device selected, use earpiece as the default",
              __func__);
        adev->out_device = AUDIO_DEVICE_OUT_EARPIECE;
    }
    adev->input_source = AUDIO_SOURCE_VOICE_CALL;

    select_devices(adev);
    start_voice_call(adev);
}

static void stop_call(struct audio_device *adev)
{
    if (!adev->in_call) {
        return;
    }

    ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_STOP);
    stop_voice_call(adev);

    /* Do not change devices if we are switching to WB */
    if (adev->mode != AUDIO_MODE_IN_CALL) {
        /* Use speaker as the default. We do not want to stay in earpiece mode */
        if (adev->out_device == AUDIO_DEVICE_NONE ||
            adev->out_device == AUDIO_DEVICE_OUT_EARPIECE) {
            adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
        }
        adev->input_source = AUDIO_SOURCE_DEFAULT;

        ALOGV("*** %s: Reset route to out devices=%#x, input src=%#x",
              __func__,
              adev->out_device,
              adev->input_source);

        adev->in_call = false;
        select_devices(adev);
    }

    adev->in_call = false;
}

static void adev_set_wb_amr_callback(void *data, int enable)
{
    struct audio_device *adev = (struct audio_device *)data;

    pthread_mutex_lock(&adev->lock);

    if (adev->wb_amr != enable) {
        adev->wb_amr = enable;

        /* reopen the modem PCMs at the new rate */
        if (adev->in_call && route_changed(adev)) {
            ALOGV("%s: %s Incall Wide Band support",
                  __func__,
                  enable ? "Turn on" : "Turn off");

            stop_call(adev);
            start_call(adev);
        }
    }

    pthread_mutex_unlock(&adev->lock);
}

static void adev_set_call_audio_path(struct audio_device *adev)
{
    enum _AudioPath device_type;

    switch(adev->out_device) {
        case AUDIO_DEVICE_OUT_SPEAKER:
            device_type = SOUND_AUDIO_PATH_SPEAKER;
            break;
        case AUDIO_DEVICE_OUT_EARPIECE:
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            device_type = SOUND_AUDIO_PATH_HEADSET;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
            device_type = SOUND_AUDIO_PATH_HEADPHONE;
            break;
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            device_type = SOUND_AUDIO_PATH_BLUETOOTH;
            break;
        default:
            /* if output device isn't supported, use handset by default */
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
    }

    ALOGV("%s: ril_set_call_audio_path(%d)", __func__, device_type);

    ril_set_call_audio_path(&adev->ril, device_type);
}

/* must be called with hw device outputs list, output stream, and hw device mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    ALOGV("%s: starting stream", __func__);

    if (out == adev->outputs[OUTPUT_HDMI]) {
        force_non_hdmi_out_standby(adev);
    } else if (adev->outputs[OUTPUT_HDMI] && !adev->outputs[OUTPUT_HDMI]->standby) {
        out->disabled = true;
        return 0;
    }

    out->disabled = false;

    if (out->device & (AUDIO_DEVICE_OUT_SPEAKER |
                       AUDIO_DEVICE_OUT_WIRED_HEADSET |
                       AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                       AUDIO_DEVICE_OUT_AUX_DIGITAL |
                       AUDIO_DEVICE_OUT_ALL_SCO)) {
        out->pcm[PCM_CARD] = pcm_open(PCM_CARD,
                                      out->pcm_device,
                                      PCM_OUT | PCM_MONOTONIC,
                                      &out->config);
        if (out->pcm[PCM_CARD] && !pcm_is_ready(out->pcm[PCM_CARD])) {
            ALOGE("pcm_open(PCM_CARD) failed: %s",
                  pcm_get_error(out->pcm[PCM_CARD]));
            pcm_close(out->pcm[PCM_CARD]);
            return -ENOMEM;
        }
    }

    if (out->device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
        out->pcm[PCM_CARD_SPDIF] = pcm_open(PCM_CARD_SPDIF,
                                            out->pcm_device,
                                            PCM_OUT | PCM_MONOTONIC,
                                            &out->config);
        if (out->pcm[PCM_CARD_SPDIF] &&
                !pcm_is_ready(out->pcm[PCM_CARD_SPDIF])) {
            ALOGE("pcm_open(PCM_CARD_SPDIF) failed: %s",
                  pcm_get_error(out->pcm[PCM_CARD_SPDIF]));
            pcm_close(out->pcm[PCM_CARD_SPDIF]);
            return -ENOMEM;
        }
    }

    /* in call routing must go through set_parameters */
    if (!adev->in_call) {
        adev->out_device |= out->device;
        select_devices(adev);
    }

    if (out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        set_hdmi_channels(adev, out->config.channels);
    }

    ALOGV("%s: stream out device: %d, actual: %d",
          __func__, out->device, adev->out_device);

    return 0;
}

/* must be called with input stream and hw device mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    in->pcm = pcm_open(PCM_CARD,
                       PCM_DEVICE,
                       PCM_IN | PCM_MONOTONIC,
                       in->config);
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
    }

    in->frames_in = 0;
    in->buffer_size = 0;

    /* in call routing must go through set_parameters */
    if (!adev->in_call) {
        adev->input_source = in->input_source;
        adev->in_device = in->device;
        adev->in_channel_mask = in->channel_mask;

        select_devices(adev);
    }

    /* initialize volume ramp */
    in->ramp_frames = (CAPTURE_START_RAMP_MS * in->requested_rate) / 1000;
    in->ramp_step = (uint16_t)(USHRT_MAX / in->ramp_frames);
    in->ramp_vol = 0;

    return 0;
}

static size_t get_input_buffer_size(unsigned int sample_rate,
                                    audio_format_t format,
                                    unsigned int channel_count,
                                    bool is_low_latency)
{
    const struct pcm_config *config = is_low_latency ?
            &pcm_config_in_low_latency : &pcm_config_in;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (config->period_size * sample_rate) / config->rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * audio_bytes_per_sample(format);
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in *in;
    size_t i;

    if (buffer_provider == NULL || buffer == NULL) {
        return -EINVAL;
    }

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        size_t size_in_bytes = pcm_frames_to_bytes(in->pcm,
                                                   in->config->period_size);
        if (in->buffer_size < in->config->period_size) {
            in->buffer_size = in->config->period_size;
            in->buffer = (int16_t *)realloc(in->buffer, size_in_bytes);
            ALOG_ASSERT((in->buffer != NULL),
                        "%s: failed to reallocate read_buf", __func__);
        }
        in->read_status = pcm_read(in->pcm,
                                   (void *)in->buffer,
                                   size_in_bytes);
        if (in->read_status != 0) {
            ALOGE("%s: pcm_read error %d", __func__, in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        in->frames_in = in->config->period_size;

        /* Do stereo to mono conversion in place by discarding right channel */
        if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer +
            (in->config->period_size - in->frames_in) *
                audio_channel_count_from_in_mask(in->channel_mask);

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;

        ALOGT("%s: frames_rd: %zd, frames_wr: %zd, in->config.channels: %d\n",
              __func__,
              frames_rd,
              frames_wr,
              in->config->channels);

        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer + pcm_frames_to_bytes(in->pcm, frames_wr)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                .raw = NULL,
                .frame_count = frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer + pcm_frames_to_bytes(in->pcm, frames_wr),
                        buf.raw,
                        pcm_frames_to_bytes(in->pcm, buf.frame_count));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }

    return frames_wr;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused,
                               uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.period_size *
           audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream __unused,
                          audio_format_t format __unused)
{
    return -ENOSYS;
}

/* Return the set of output devices associated with active streams
 * other than out.  Assumes out is non-NULL and out->dev is locked.
 */
static audio_devices_t output_devices(struct stream_out *out)
{
    struct audio_device *dev = out->dev;
    enum output_type type;
    audio_devices_t devices = AUDIO_DEVICE_NONE;

    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *other = dev->outputs[type];
        if (other && (other != out) && !other->standby) {
            // TODO no longer accurate
            /* safe to access other stream without a mutex,
             * because we hold the dev lock,
             * which prevents the other stream from being closed
             */
            devices |= other->device;
        }
    }

    return devices;
}

/* must be called with hw device outputs list, all out streams, and hw device mutex locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int i;

    ALOGV("%s: output standby: %d", __func__, out->standby);

    if (!out->standby) {
        for (i = 0; i < PCM_TOTAL; i++) {
            if (out->pcm[i]) {
                pcm_close(out->pcm[i]);
                out->pcm[i] = NULL;
            }
        }
        out->standby = true;

        if (out == adev->outputs[OUTPUT_HDMI]) {
            /* force standby on low latency output stream so that it can reuse HDMI driver if
             * necessary when restarted */
            force_non_hdmi_out_standby(adev);
        }

        /* re-calculate the set of active devices from other streams */
        adev->out_device = output_devices(out);

        /* Skip resetting the mixer if no output device is active */
        if (adev->out_device)
            select_devices(adev);
    }
}

static void lock_input_stream(struct stream_in *in)
{
    pthread_mutex_lock(&in->pre_lock);
    pthread_mutex_lock(&in->lock);
    pthread_mutex_unlock(&in->pre_lock);
}

static void unlock_input_stream(struct stream_in *in)
{
    pthread_mutex_unlock(&in->lock);
}

static void lock_output_stream(struct stream_out *out)
{
    pthread_mutex_lock(&out->pre_lock);
    pthread_mutex_lock(&out->lock);
    pthread_mutex_unlock(&out->pre_lock);
}

static void unlock_output_stream(struct stream_out *out)
{
    pthread_mutex_unlock(&out->lock);
}

/* lock outputs list, all output streams, and device */
static void lock_all_outputs(struct audio_device *adev)
{
    enum output_type type;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *out = adev->outputs[type];
        if (out) {
            lock_output_stream(out);
        }
    }
    pthread_mutex_lock(&adev->lock);
}

/* unlock device, all output streams (except specified stream), and outputs list */
static void unlock_all_outputs(struct audio_device *adev, struct stream_out *except)
{
    /* unlock order is irrelevant, but for cleanliness we unlock in reverse order */
    pthread_mutex_unlock(&adev->lock);
    enum output_type type = OUTPUT_TOTAL;
    do {
        struct stream_out *out = adev->outputs[--type];
        if (out && out != except) {
            unlock_output_stream(out);
        }
    } while (type != (enum output_type) 0);
    pthread_mutex_unlock(&adev->lock_outputs);
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    lock_all_outputs(adev);

    do_out_standby(out);

    unlock_all_outputs(adev, NULL);

    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGV("%s: key value pairs: %s", __func__, kvpairs);

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);

        lock_all_outputs(adev);

        if ((out->device != val) && (val != 0)) {
            /* Force standby if moving to/from SPDIF or if the output
             * device changes when in SPDIF mode */
            if (((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                 (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) ||
                (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) {
                do_out_standby(out);
            }

            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_OUT_ALL_SCO) ^
                (out->device & AUDIO_DEVICE_OUT_ALL_SCO)) {
                do_out_standby(out);
            }

#ifndef HDMI_INCAPABLE
            if (!out->standby && (out == adev->outputs[OUTPUT_HDMI] ||
                !adev->outputs[OUTPUT_HDMI] ||
                adev->outputs[OUTPUT_HDMI]->standby)) {
                adev->out_device = output_devices(out) | val;
                select_devices(adev);
            }
#endif

            out->device = val;
            adev->out_device = output_devices(out) | val;

            /*
             * If we switch from earpiece to speaker, we need to fully reset the
             * modem audio path.
             */
            if (adev->in_call) {
                if (route_changed(adev)) {
                    stop_call(adev);
                    start_call(adev);
                }
            } else {
                select_devices(adev);
            }

            /* start SCO stream if needed */
            if (val & AUDIO_DEVICE_OUT_ALL_SCO) {
                start_bt_sco(adev);
            }
        }

        unlock_all_outputs(adev, NULL);
    }

    str_parms_destroy(parms);
    return ret;
}

/*
 * Returns a pointer to a heap allocated string. The caller is responsible
 * for freeing the memory for it using free().
 */
static char *out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    const char *str;
    char value[256];
    struct str_parms *reply = str_parms_create();
    size_t i, j;
    int ret;
    bool first = true;

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        i = 0;
        /* the last entry in supported_channel_masks[] is always 0 */
        while (out->supported_channel_masks[i] != 0) {
            for (j = 0; j < ARRAY_SIZE(out_channels_name_to_enum_table); j++) {
                if (out_channels_name_to_enum_table[j].value == out->supported_channel_masks[i]) {
                    if (!first) {
                        strcat(value, "|");
                    }
                    strcat(value, out_channels_name_to_enum_table[j].name);
                    first = false;
                    break;
                }
            }
            i++;
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
        str = str_parms_to_str(reply);
    } else {
        str = keys;
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);
    return strdup(str);
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return (out->config.period_size * out->config.period_count * 1000) /
            out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream,
                          float left,
                          float right __unused)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    if (out == adev->outputs[OUTPUT_HDMI]) {
        /* only take left channel into account: the API is for stereo anyway */
        out->muted = (left == 0.0f);
        return 0;
    }
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int i;

    /* FIXME This comment is no longer correct
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    lock_output_stream(out);
    if (out->standby) {
        unlock_output_stream(out);
        lock_all_outputs(adev);
        if (!out->standby) {
            unlock_all_outputs(adev, out);
            goto false_alarm;
        }
        ret = start_output_stream(out);
        if (ret < 0) {
            unlock_all_outputs(adev, NULL);
            goto final_exit;
        }
        out->standby = false;
        unlock_all_outputs(adev, out);
    }
false_alarm:

    if (out->disabled) {
        ret = -EPIPE;
        goto exit;
    }

    if (out->muted)
        memset((void *)buffer, 0, bytes);

    /* Write to all active PCMs */
    for (i = 0; i < PCM_TOTAL; i++)
        if (out->pcm[i]) {
            ret = pcm_write(out->pcm[i], (void *)buffer, bytes);
            if (ret != 0)
                break;
        }
    if (ret == 0)
        out->written += bytes / (out->config.channels * sizeof(short));

exit:
    unlock_output_stream(out);
final_exit:

    if (ret != 0) {
        struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
        const int64_t elapsed_time_since_last_write = now - out->last_write_time_us;
        int64_t sleep_time = bytes * 1000000LL / audio_stream_out_frame_size(stream) /
                   out_get_sample_rate(&stream->common) - elapsed_time_since_last_write;
        if (sleep_time > 0) {
            usleep(sleep_time);
        } else {
            // we don't sleep when we exit standby (this is typical for a real alsa buffer).
            sleep_time = 0;
        }
        out->last_write_time_us = now + sleep_time;
        // last_write_time_us is an approximation of when the (simulated) alsa
        // buffer is believed completely full. The usleep above waits for more space
        // in the buffer, but by the end of the sleep the buffer is considered
        // topped-off.
        //
        // On the subsequent out_write(), we measure the elapsed time spent in
        // the mixer. This is subtracted from the sleep estimate based on frames,
        // thereby accounting for drain in the alsa buffer during mixing.
        // This is a crude approximation; we don't handle underruns precisely.
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream __unused,
                                   uint32_t *dsp_frames __unused)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused,
                                   effect_handle_t effect __unused)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                                        int64_t *timestamp __unused)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;

    lock_output_stream(out);

    int i;
    // There is a question how to implement this correctly when there is more than one PCM stream.
    // We are just interested in the frames pending for playback in the kernel buffer here,
    // not the total played since start.  The current behavior should be safe because the
    // cases where both cards are active are marginal.
    for (i = 0; i < PCM_TOTAL; i++)
        if (out->pcm[i]) {
            size_t avail;
            if (pcm_get_htimestamp(out->pcm[i], &avail, timestamp) == 0) {
                size_t kernel_buffer_size = out->config.period_size * out->config.period_count;
                // FIXME This calculation is incorrect if there is buffering after app processor
                int64_t signed_frames = out->written - kernel_buffer_size + avail;
                // It would be unusual for this value to be negative, but check just in case ...
                if (signed_frames >= 0) {
                    *frames = signed_frames;
                    ret = 0;
                }
                break;
            }
        }

    unlock_output_stream(out);

    return ret;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused,
                              uint32_t rate __unused)
{
    return 0;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->channel_mask;
}


static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 audio_channel_count_from_in_mask(in_get_channels(stream)),
                                 (in->flags & AUDIO_INPUT_FLAG_FAST) != 0);
}

static audio_format_t in_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream __unused,
                         audio_format_t format __unused)
{
    return -ENOSYS;
}

/* must be called with in stream and hw device mutex locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        in->standby = true;
        if (in->pcm != NULL) {
            pcm_close(in->pcm);
            in->pcm = NULL;
        }

        if (adev->mode != AUDIO_MODE_IN_CALL) {
            in->dev->input_source = AUDIO_SOURCE_DEFAULT;
            in->dev->in_device = AUDIO_DEVICE_NONE;
            in->dev->in_channel_mask = 0;
            select_devices(adev);
        }
    }
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    lock_input_stream(in);
    pthread_mutex_lock(&in->dev->lock);

    do_in_standby(in);

    pthread_mutex_unlock(&in->dev->lock);
    unlock_input_stream(in);

    return 0;
}

static int in_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;
    bool apply_now = false;

    parms = str_parms_create_str(kvpairs);

    lock_input_stream(in);
    pthread_mutex_lock(&adev->lock);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->input_source != val) && (val != 0)) {
            in->input_source = val;
            apply_now = !in->standby;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        /* no audio device uses val == 0 */
        if ((in->device != val) && (val != 0)) {
            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) ^
                    (in->device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
                do_in_standby(in);
            }
            in->device = val;
            apply_now = !in->standby;
        }
    }

    if (apply_now) {
        adev->input_source = in->input_source;
        adev->in_device = in->device;
        select_devices(adev);
    }

    pthread_mutex_unlock(&adev->lock);
    unlock_input_stream(in);

    str_parms_destroy(parms);
    return ret;
}

static char *in_get_parameters(const struct audio_stream *stream __unused,
                               const char *keys __unused)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream __unused,
                       float gain __unused)
{
    return 0;
}

static void in_apply_ramp(struct stream_in *in, int16_t *buffer, size_t frames)
{
    size_t i;
    uint16_t vol = in->ramp_vol;
    uint16_t step = in->ramp_step;

    frames = (frames < in->ramp_frames) ? frames : in->ramp_frames;

    if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
        for (i = 0; i < frames; i++)
        {
            buffer[i] = (int16_t)((buffer[i] * vol) >> 16);
            vol += step;
        }
    else
        for (i = 0; i < frames; i++)
        {
            buffer[2*i] = (int16_t)((buffer[2*i] * vol) >> 16);
            buffer[2*i + 1] = (int16_t)((buffer[2*i + 1] * vol) >> 16);
            vol += step;
        }


    in->ramp_vol = vol;
    in->ramp_frames -= frames;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    lock_input_stream(in);
    if (in->standby) {
        pthread_mutex_lock(&adev->lock);
        ret = start_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret < 0)
            goto exit;
        in->standby = false;
    }

    /*if (in->num_preprocessors != 0)
        ret = process_frames(in, buffer, frames_rq);
      else */
    ret = read_frames(in, buffer, frames_rq);

    if (ret > 0)
        ret = 0;

    if (in->ramp_frames > 0)
        in_apply_ramp(in, buffer, frames_rq);

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret != 0) {
        struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;

        // we do a full sleep when exiting standby.
        const bool standby = in->last_read_time_us == 0;
        const int64_t elapsed_time_since_last_read = standby ?
                0 : now - in->last_read_time_us;
        int64_t sleep_time = bytes * 1000000LL / audio_stream_in_frame_size(stream) /
                in_get_sample_rate(&stream->common) - elapsed_time_since_last_read;
        if (sleep_time > 0) {
            usleep(sleep_time);
        } else {
            sleep_time = 0;
        }
        in->last_read_time_us = now + sleep_time;
        // last_read_time_us is an approximation of when the (simulated) alsa
        // buffer is drained by the read, and is empty.
        //
        // On the subsequent in_read(), we measure the elapsed time spent in
        // the recording thread. This is subtracted from the sleep estimate based on frames,
        // thereby accounting for fill in the alsa buffer during the interim.
        memset(buffer, 0, bytes);
    }

    if (bytes > 0) {
        in->frames_read += bytes / audio_stream_in_frame_size(stream);
    }

    unlock_input_stream(in);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream __unused)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream __unused,
                               effect_handle_t effect __unused)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream __unused,
                                  effect_handle_t effect __unused)
{
    return 0;
}

static int in_get_capture_position(const struct audio_stream_in *stream,
                                   int64_t *frames,
                                   int64_t *time)
{
    struct stream_in *in;
    int rc = -ENOSYS;

    if (stream == NULL || frames == NULL || time == NULL) {
        return -EINVAL;
    }
    in = (struct stream_in *)stream;

    lock_input_stream(in);
    if (in->pcm != NULL) {
        struct timespec timestamp;
        unsigned int avail;

        rc = pcm_get_htimestamp(in->pcm, &avail, &timestamp);
        if (rc != 0) {
            rc = -EINVAL;
        } else {
            *frames = in->frames_read + avail;
            *time = timestamp.tv_sec * 1000000000LL + timestamp.tv_nsec;
            rc = 0;
        }
    }
    unlock_input_stream(in);

    return rc;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle __unused,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;
    enum output_type type;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;
    out->device = devices;

    if (flags & AUDIO_OUTPUT_FLAG_DIRECT &&
        devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        pthread_mutex_lock(&adev->lock);
        ret = read_hdmi_channel_masks(adev, out);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0)
            goto err_open;
        if (config->sample_rate == 0)
            config->sample_rate = HDMI_MULTI_DEFAULT_SAMPLING_RATE;
        if (config->channel_mask == 0)
            config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
        out->channel_mask = config->channel_mask;
        out->config = pcm_config_hdmi_multi;
        out->config.rate = config->sample_rate;
        out->config.channels = popcount(config->channel_mask);
        out->pcm_device = PCM_DEVICE;
        type = OUTPUT_HDMI;
    } else if (flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        ALOGV("*** %s: Deep buffer pcm config", __func__);
        out->config = pcm_config_deep;
        out->pcm_device = PCM_DEVICE_DEEP;
        type = OUTPUT_DEEP_BUF;
    } else {
        ALOGV("*** %s: Fast buffer pcm config", __func__);
        out->config = pcm_config_fast;
        out->pcm_device = PCM_DEVICE;
        type = OUTPUT_LOW_LATENCY;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;
    /* out->muted = false; by calloc() */
    /* out->written = 0; by calloc() */

    pthread_mutex_init(&out->lock, (const pthread_mutexattr_t *) NULL);
    pthread_mutex_init(&out->pre_lock, (const pthread_mutexattr_t *) NULL);

    pthread_mutex_lock(&adev->lock_outputs);
    if (adev->outputs[type]) {
        pthread_mutex_unlock(&adev->lock_outputs);
        ret = -EBUSY;
        goto err_open;
    }
    adev->outputs[type] = out;
    pthread_mutex_unlock(&adev->lock_outputs);

    *stream_out = &out->stream;

    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct audio_device *adev;
    enum output_type type;

    out_standby(&stream->common);
    adev = (struct audio_device *)dev;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; type++) {
        if (adev->outputs[type] == (struct stream_out *) stream) {
            adev->outputs[type] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&adev->lock_outputs);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char value[32];
    int ret;

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms,
                            AUDIO_PARAMETER_KEY_BT_NREC,
                            value,
                            sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0) {
            adev->bluetooth_nrec = true;
        } else {
            adev->bluetooth_nrec = false;
        }
    }

    /* FIXME: This does not work with LL, see workaround in this HAL */
    ret = str_parms_get_str(parms, "noise_suppression", value, sizeof(value));
    if (ret >= 0) {
        ALOGV("*** %s: noise_suppression=%s", __func__, value);

        /* value is either off or auto */
        if (strcmp(value, "off") == 0) {
            adev->two_mic_control = false;
        } else {
            adev->two_mic_control = true;
        }
    }

    str_parms_destroy(parms);
    return ret;
}

static char *adev_get_parameters(const struct audio_hw_device *dev __unused,
                                 const char *keys __unused)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev __unused)
{
    return 0;
}

static int voice_set_volume(struct audio_hw_device *dev, float volume)
{
    struct audio_device *adev = (struct audio_device *)dev;

    ALOGT("%s: Set volume to %f\n", __func__, volume);

    adev->voice_volume = volume;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        enum _SoundType sound_type;

        switch (adev->out_device) {
            case AUDIO_DEVICE_OUT_EARPIECE:
                sound_type = SOUND_TYPE_VOICE;
                break;
            case AUDIO_DEVICE_OUT_SPEAKER:
                sound_type = SOUND_TYPE_SPEAKER;
                break;
            case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
                sound_type = SOUND_TYPE_HEADSET;
                break;
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AUDIO_DEVICE_OUT_ALL_SCO:
                sound_type = SOUND_TYPE_BTVOICE;
                break;
            default:
                sound_type = SOUND_TYPE_VOICE;
        }

        ril_set_call_volume(&adev->ril, sound_type, volume);
    }

    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct audio_device *adev = (struct audio_device *)dev;
    int rc;

    ALOGT("%s: Set volume to %f\n", __func__, volume);

    pthread_mutex_lock(&adev->lock);
    rc = voice_set_volume(dev, volume);
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused,
                                  float volume __unused)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;

    if (adev->mode == mode) {
        return 0;
    }

    pthread_mutex_lock(&adev->lock);
    adev->mode = mode;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        ALOGV("*** %s: Entering IN_CALL mode", __func__);
        start_call(adev);
    } else {
        ALOGV("*** %s: Leaving IN_CALL mode", __func__);
        stop_call(adev);
    }

    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;
    enum _MuteCondition mute_condition = state ? TX_MUTE : TX_UNMUTE;

    ALOGT("%s: Set mic mute: %d\n", __func__, state);

    pthread_mutex_lock(&adev->lock);
    if (adev->in_call) {
        ril_set_mute(&adev->ril, mute_condition);
    }

    adev->mic_mute = state;
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev __unused,
                                         const struct audio_config *config)
{

    return get_input_buffer_size(config->sample_rate, config->format,
                                 audio_channel_count_from_in_mask(config->channel_mask),
                                 false /* is_low_latency: since we don't know, be conservative */);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    *stream_in = NULL;

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO &&
        config->channel_mask != AUDIO_CHANNEL_IN_FRONT_BACK) {
        if (!(adev->in_call && adev->two_mic_control)) {
            /* Not in a call and no explicit FRONT_BACK input requested */
            config->channel_mask = AUDIO_CHANNEL_IN_MONO;
            return -EINVAL;
        }
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (in == NULL) {
        return -ENOMEM;
    }

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->stream.get_capture_position = in_get_capture_position;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->input_source = AUDIO_SOURCE_DEFAULT;
    /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
    in->device = devices & ~AUDIO_DEVICE_BIT_IN;
    in->io_handle = handle;
    in->channel_mask = config->channel_mask;
    in->flags = flags;
    struct pcm_config *pcm_config = flags & AUDIO_INPUT_FLAG_FAST ?
            &pcm_config_in_low_latency : &pcm_config_in;
    in->config = pcm_config;

    in->buffer = malloc(pcm_config->period_size * pcm_config->channels
                                               * audio_stream_in_frame_size(&in->stream));

    if (!in->buffer) {
        ret = -ENOMEM;
        goto err_malloc;
    }

    if (in->requested_rate != pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(pcm_config->rate,
                               in->requested_rate,
                               audio_channel_count_from_in_mask(in->channel_mask),
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err_resampler;
        }

        ALOGV("%s: Created resampler converting %d -> %d\n",
              __func__, pcm_config_in.rate, in->requested_rate);
    }

    ALOGV("%s: Requesting input stream with rate: %d, channels: 0x%x\n",
          __func__, config->sample_rate, config->channel_mask);

    pthread_mutex_init(&in->lock, (const pthread_mutexattr_t *) NULL);
    pthread_mutex_init(&in->pre_lock, (const pthread_mutexattr_t *) NULL);

    *stream_in = &in->stream;
    return 0;

err_resampler:
    free(in->buffer);
err_malloc:
    free(in);
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev __unused,
                                   struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    in_standby(&stream->common);
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }
    free(in->buffer);
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    audio_route_free(adev->audio_route);

    if (adev->hdmi_drv_fd >= 0) {
        close(adev->hdmi_drv_fd);
    }

    /* RIL */
    ril_close(&adev->ril);

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        return -EINVAL;
    }

    adev = calloc(1, sizeof(struct audio_device));
    if (adev == NULL) {
        return -ENOMEM;
    }

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->audio_route = audio_route_init(MIXER_CARD, NULL);
    adev->input_source = AUDIO_SOURCE_DEFAULT;
    adev->active_output.dev_id = -1;
    /* adev->cur_route_id initial value is 0 and such that first device
     * selection is always applied by select_devices() */

    adev->hdmi_drv_fd = -1;

    adev->mode = AUDIO_MODE_NORMAL;
    adev->voice_volume = 1.0f;

    /* RIL */
    ril_open(&adev->ril);
    /* register callback for wideband AMR setting */
    ril_register_set_wb_amr_callback(adev_set_wb_amr_callback, (void *)adev);

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Exynos543x Audio HAL",
        .author = "The LineageOS Project",
        .methods = &hal_module_methods,
    },
};
