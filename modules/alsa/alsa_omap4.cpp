/* alsa_omap4.cpp
 **
 ** Copyright 2009 Texas Instruments
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#define LOG_TAG "Omap4ALSA"
#include <utils/Log.h>

#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>

#ifdef AUDIO_MODEM_TI
#include "audio_modem_interface.h"
#include "alsa_omap4_modem.h"
#endif

#define MM_DEFAULT_DEVICE	"plughw:0,0"
#define BLUETOOTH_SCO_DEVICE	"hw:0,2"
#define FM_TRANSMIT_DEVICE	"hw:0,2"
#define HDMI_DEVICE		"plughw:0,7"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

namespace android
{

static int s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *, uint32_t, int);
static status_t s_close(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int);

#ifdef AUDIO_MODEM_TI
    AudioModemAlsa *audioModem;
#endif

static hw_module_methods_t s_module_methods = {
    open            : s_device_open
};

extern "C" const hw_module_t HAL_MODULE_INFO_SYM = {
    tag             : HARDWARE_MODULE_TAG,
    version_major   : 1,
    version_minor   : 0,
    id              : ALSA_HARDWARE_MODULE_ID,
    name            : "Omap4 ALSA module",
    author          : "Texas Instruments",
    methods         : &s_module_methods,
    dso             : 0,
    reserved        : {0,},
};

static int s_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    alsa_device_t *dev;
    dev = (alsa_device_t *) malloc(sizeof(*dev));
    if (!dev) return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    /* initialize the procs */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *) module;
    dev->common.close = s_device_close;
    dev->init = s_init;
    dev->open = s_open;
    dev->close = s_close;
    dev->route = s_route;

    *device = &dev->common;

    LOGD("OMAP4 ALSA module opened");

    return 0;
}

static int s_device_close(hw_device_t* device)
{
    free(device);
    return 0;
}

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

static void setScoControls(uint32_t devices, int mode);
static void setFmControls(uint32_t devices, int mode);
static void setHDMIControls(uint32_t devices, int mode);
static void setDefaultControls(uint32_t devices, int mode);

typedef void (*AlsaControlSet)(uint32_t devices, int mode);

/*
 * Eclair 2.1 has removed board specific device outputs
 * since omap has FM support, we add it back in here.
 * be sure this stays in sync with hardware/alsa_sound
 */
#define DEVICE_OUT_FM_HEADPHONE 0x800
#define DEVICE_OUT_FM_SPEAKER 0x1000

#define OMAP4_OUT_SCO      (\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)

#define OMAP4_OUT_FM        (\
        DEVICE_OUT_FM_HEADPHONE |\
        DEVICE_OUT_FM_SPEAKER)

#define OMAP4_OUT_HDMI        (\
        AudioSystem::DEVICE_OUT_AUX_DIGITAL)

#define OMAP4_OUT_DEFAULT   (\
        AudioSystem::DEVICE_OUT_ALL &\
        ~OMAP4_OUT_SCO &\
        ~OMAP4_OUT_FM  &\
        ~OMAP4_OUT_HDMI)

#define OMAP4_IN_SCO        (\
        AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)

#define OMAP4_IN_DEFAULT    (\
        AudioSystem::DEVICE_IN_ALL &\
        ~OMAP4_IN_SCO)

static alsa_handle_t _defaults[] = {
    {
        module      : 0,
        devices     : OMAP4_OUT_SCO,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000, // Desired Delay in usec
        bufferSize  : DEFAULT_SAMPLE_RATE / 5, // Desired Number of samples
        modPrivate  : (void *)&setScoControls,
    },
    {
        module      : 0,
        devices     : OMAP4_OUT_FM,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000, // Desired Delay in usec
        bufferSize  : DEFAULT_SAMPLE_RATE / 5, // Desired Number of samples
        modPrivate  : (void *)&setFmControls,
    },
    {
        module      : 0,
        devices     : OMAP4_OUT_HDMI,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000, // Desired Delay in usec
        bufferSize  : DEFAULT_SAMPLE_RATE / 5, // Desired Number of samples
        modPrivate  : (void *)&setHDMIControls,
    },
    {
        module      : 0,
        devices     : OMAP4_OUT_DEFAULT,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000, // Desired Delay in usec
        bufferSize  : DEFAULT_SAMPLE_RATE / 10, // Desired Number of samples
        modPrivate  : (void *)&setDefaultControls,
    },
    {
        module      : 0,
        devices     : OMAP4_IN_SCO,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 250000, // Desired Delay in usec
        bufferSize  : 2048, // Desired Number of samples
        modPrivate  : (void *)&setScoControls,
    },
    {
        module      : 0,
        devices     : OMAP4_IN_DEFAULT,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 250000, // Desired Delay in usec
        bufferSize  : 2048, // Desired Number of samples
        modPrivate  : (void *)&setDefaultControls,
    },
};

// ----------------------------------------------------------------------------

const char *deviceName(alsa_handle_t *handle, uint32_t device, int mode)
{
    if (device & OMAP4_OUT_SCO || device & OMAP4_IN_SCO)
        return BLUETOOTH_SCO_DEVICE;

    if (device & OMAP4_OUT_FM)
        return FM_TRANSMIT_DEVICE;

    if (device & OMAP4_OUT_HDMI)
        return HDMI_DEVICE;

    return MM_DEFAULT_DEVICE;
}

snd_pcm_stream_t direction(alsa_handle_t *handle)
{
    return (handle->devices & AudioSystem::DEVICE_OUT_ALL) ? SND_PCM_STREAM_PLAYBACK
            : SND_PCM_STREAM_CAPTURE;
}

const char *streamName(alsa_handle_t *handle)
{
    return snd_pcm_stream_name(direction(handle));
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    snd_pcm_hw_params_t *hardwareParams;
    status_t err;

    snd_pcm_uframes_t bufferSize = handle->bufferSize;
    unsigned int requestedRate = handle->sampleRate;
    unsigned int latency = handle->latency;

    // snd_pcm_format_description() and snd_pcm_format_name() do not perform
    // proper bounds checking.
    bool validFormat = (static_cast<int> (handle->format)
            > SND_PCM_FORMAT_UNKNOWN) && (static_cast<int> (handle->format)
            <= SND_PCM_FORMAT_LAST);
    const char *formatDesc = validFormat ? snd_pcm_format_description(
            handle->format) : "Invalid Format";
    const char *formatName = validFormat ? snd_pcm_format_name(handle->format)
            : "UNKNOWN";

    if (snd_pcm_hw_params_malloc(&hardwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    err = snd_pcm_hw_params_any(handle->handle, hardwareParams);
    if (err < 0) {
        LOGE("Unable to configure hardware: %s", snd_strerror(err));
        goto done;
    }

    // Set the interleaved read and write format.
    err = snd_pcm_hw_params_set_access(handle->handle, hardwareParams,
            SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOGE("Unable to configure PCM read/write format: %s",
                snd_strerror(err));
        goto done;
    }

    err = snd_pcm_hw_params_set_format(handle->handle, hardwareParams,
            handle->format);
    if (err < 0) {
        LOGE("Unable to configure PCM format %s (%s): %s",
                formatName, formatDesc, snd_strerror(err));
        goto done;
    }

    LOGV("Set %s PCM format to %s (%s)", streamName(handle), formatName, formatDesc);

    err = snd_pcm_hw_params_set_channels(handle->handle, hardwareParams,
            handle->channels);
    if (err < 0) {
        LOGE("Unable to set channel count to %i: %s",
                handle->channels, snd_strerror(err));
        goto done;
    }

    LOGV("Using %i %s for %s.", handle->channels,
            handle->channels == 1 ? "channel" : "channels", streamName(handle));

    err = snd_pcm_hw_params_set_rate_near(handle->handle, hardwareParams,
            &requestedRate, 0);

    if (err < 0)
        LOGE("Unable to set %s sample rate to %u: %s",
                streamName(handle), handle->sampleRate, snd_strerror(err));
    else if (requestedRate != handle->sampleRate)
        // Some devices have a fixed sample rate, and can not be changed.
        // This may cause resampling problems; i.e. PCM playback will be too
        // slow or fast.
        LOGW("Requested rate (%u HZ) does not match actual rate (%u HZ)",
                handle->sampleRate, requestedRate);
    else
        LOGV("Set %s sample rate to %u HZ", streamName(handle), requestedRate);

    // Make sure we have at least the size we originally wanted

    err = snd_pcm_hw_params_set_buffer_size_near(handle->handle, hardwareParams,
            &bufferSize);

    if (err < 0) {
        LOGE("Unable to set buffer size to %d:  %s",
                (int)bufferSize, snd_strerror(err));
        goto done;
    }

    // Setup buffers for latency
    err = snd_pcm_hw_params_set_buffer_time_near(handle->handle,
            hardwareParams, &latency, NULL);
    if (err < 0) {
        /* That didn't work, set the period instead */
        unsigned int periodTime = latency / 4;
        err = snd_pcm_hw_params_set_period_time_near(handle->handle,
                hardwareParams, &periodTime, NULL);
        if (err < 0) {
            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
            goto done;
        }
        snd_pcm_uframes_t periodSize;
        err = snd_pcm_hw_params_get_period_size(hardwareParams, &periodSize,
                NULL);
        if (err < 0) {
            LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
            goto done;
        }
        bufferSize = periodSize * 4;
        if (bufferSize < handle->bufferSize) bufferSize = handle->bufferSize;
        err = snd_pcm_hw_params_set_buffer_size_near(handle->handle,
                hardwareParams, &bufferSize);
        if (err < 0) {
            LOGE("Unable to set the buffer size for latency: %s", snd_strerror(err));
            goto done;
        }
    } else {
        // OK, we got buffer time near what we expect. See what that did for bufferSize.
        err = snd_pcm_hw_params_get_buffer_size(hardwareParams, &bufferSize);
        if (err < 0) {
            LOGE("Unable to get the buffer size for latency: %s", snd_strerror(err));
            goto done;
        }
        // Does set_buffer_time_near change the passed value? It should.
        err = snd_pcm_hw_params_get_buffer_time(hardwareParams, &latency, NULL);
        if (err < 0) {
            LOGE("Unable to get the buffer time for latency: %s", snd_strerror(err));
            goto done;
        }
        unsigned int periodTime = latency / 4;
        err = snd_pcm_hw_params_set_period_time_near(handle->handle,
                hardwareParams, &periodTime, NULL);
        if (err < 0) {
            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
            goto done;
        }
    }

    LOGV("Buffer size: %d", (int)bufferSize);
    LOGV("Latency: %d", (int)latency);

    handle->bufferSize = bufferSize;
    handle->latency = latency;

    // Commit the hardware parameters back to the device.
    err = snd_pcm_hw_params(handle->handle, hardwareParams);
    if (err < 0) LOGE("Unable to set hardware parameters: %s", snd_strerror(err));

    done:
    snd_pcm_hw_params_free(hardwareParams);

    return err;
}

status_t setSoftwareParams(alsa_handle_t *handle)
{
    snd_pcm_sw_params_t * softwareParams;
    int err;

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    snd_pcm_uframes_t startThreshold, stopThreshold;

    if (snd_pcm_sw_params_malloc(&softwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    // Get the current software parameters
    err = snd_pcm_sw_params_current(handle->handle, softwareParams);
    if (err < 0) {
        LOGE("Unable to get software parameters: %s", snd_strerror(err));
        goto done;
    }

    // Configure ALSA to start the transfer when the buffer is almost full.
    snd_pcm_get_params(handle->handle, &bufferSize, &periodSize);

    if (handle->devices & AudioSystem::DEVICE_OUT_ALL) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is full.
        startThreshold = bufferSize - 1;
        stopThreshold = bufferSize;
    } else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = 1;
        stopThreshold = bufferSize;
    }

    err = snd_pcm_sw_params_set_start_threshold(handle->handle, softwareParams,
            startThreshold);
    if (err < 0) {
        LOGE("Unable to set start threshold to %lu frames: %s",
                startThreshold, snd_strerror(err));
        goto done;
    }

    err = snd_pcm_sw_params_set_stop_threshold(handle->handle, softwareParams,
            stopThreshold);
    if (err < 0) {
        LOGE("Unable to set stop threshold to %lu frames: %s",
                stopThreshold, snd_strerror(err));
        goto done;
    }

    // Allow the transfer to start when at least periodSize samples can be
    // processed.
    err = snd_pcm_sw_params_set_avail_min(handle->handle, softwareParams,
            periodSize);
    if (err < 0) {
        LOGE("Unable to configure available minimum to %lu: %s",
                periodSize, snd_strerror(err));
        goto done;
    }

    // Commit the software parameters back to the device.
    err = snd_pcm_sw_params(handle->handle, softwareParams);
    if (err < 0) LOGE("Unable to configure software parameters: %s",
            snd_strerror(err));

    done:
    snd_pcm_sw_params_free(softwareParams);

    return err;
}

void setScoControls(uint32_t devices, int mode)
{
LOGV("%s", __FUNCTION__);
}

void setFmControls(uint32_t devices, int mode)
{
LOGV("%s", __FUNCTION__);
}

void setHDMIControls(uint32_t devices, int mode)
{
LOGV("%s", __FUNCTION__);
}

void setDefaultControls(uint32_t devices, int mode)
{
LOGV("%s", __FUNCTION__);
    ALSAControl control("hw:00");

#ifdef AUDIO_MODEM_TI
    audioModem->voiceCallControls(devices, mode, &control);
#endif

    /* check whether the devices is input or not */
    /* for output devices */
    if (devices & 0x0000FFFF){
        if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            /* OMAP4 ABE */
            control.set("DL2 Media Playback Volume", 118);
            control.set("DL2 Tones Playback Volume", 118);
            control.set("DL2 Voice Playback Volume", 118);
            /* TWL6040 */
            control.set("HF Left Playback", "HF DAC");		// HFDAC L -> HF Mux
            control.set("HF Right Playback", "HF DAC");		// HFDAC R -> HF Mux
            control.set("Handsfree Playback Volume", 29);
        } else {
            /* OMAP4 ABE */
            control.set("DL2 Media Playback Volume", 0, -1);
            control.set("DL2 Tones Playback Volume", 0, -1);
            control.set("DL2 Voice Playback Volume", 0, -1);
            /* TWL6040 */
            control.set("HF Left Playback", "Off");
            control.set("HF Right Playback", "Off");
            control.set("Handsfree Playback Volume", 0, -1);
        }

        if (devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            /* TWL6040 */
            control.set("HS Left Playback", "HS DAC");		// HSDAC L -> HS Mux
            control.set("HS Right Playback", "HS DAC");		// HSDAC R -> HS Mux
            control.set("Headset Playback Volume", 15);
        } else {
            /* TWL6040 */
            control.set("HS Left Playback", "Off");
            control.set("HS Right Playback", "Off");
            control.set("Headset Playback Volume", 0, -1);
        }

        if (devices & AudioSystem::DEVICE_OUT_EARPIECE) {
            /* TWL6040 */
            control.set("Earphone Driver Switch", 1);		// HSDACL -> Earpiece
            control.set("Earphone Playback Volume", 15);
        } else {
            /* TWL6040 */
            control.set("Earphone Driver Switch", 0, 0);
            control.set("Earphone Playback Volume", 0, -1);
        }

	/*
         * ASoC multicomponent doesn't allow us to enable backends
         * on-the-fly, so enable handsfree and headset backends
         * always
         */
        if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) ||
            (devices & AudioSystem::DEVICE_OUT_EARPIECE) ||
            (devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET)) {
            /* OMAP4 ABE */
            /* Headset: DL1 Mixer */
            control.set("DL1 Mixer Multimedia", 1);		// MM_DL    -> DL1 Mixer
            control.set("DL1 Mixer Tones", 1);			// TONES_DL -> DL1 Mixer
            control.set("DL1 Mixer Voice", 1);			// VX_DL    -> DL1 Mixer
            control.set("Sidetone Mixer Playback", 1);		// DL1 Mixer-> Sidetone Mixer
            control.set("DL1 PDM Switch", 1);			// Sidetone Mixer -> PDM_DL1
            control.set("DL1 Media Playback Volume", 118);
            control.set("DL1 Tones Playback Volume", 118);
            control.set("DL1 Voice Playback Volume", 118);

            /* Handsfree: DL2 Mixer */
            control.set("DL2 Mixer Multimedia", 1);		// MM_DL    -> DL2 Mixer
            control.set("DL2 Mixer Tones", 1);			// TONES_DL -> DL2 Mixer
            control.set("DL2 Mixer Voice", 1);			// VX_DL    -> DL2 Mixer
        } else {
            /* OMAP4 ABE */
            /* Headset: DL1 Mixer */
            control.set("DL1 Mixer Multimedia", 0, 0);
            control.set("DL1 Mixer Tones", 0, 0);
            control.set("DL1 Mixer Voice", 0, 0);
            control.set("Sidetone Mixer Playback", 0, 0);
            control.set("DL1 PDM Switch", 0, 0);
            control.set("DL1 Media Playback Volume", 0, -1);
            control.set("DL1 Tones Playback Volume", 0, -1);
            control.set("DL1 Voice Playback Volume", 0, -1);

            /* Handsfree: DL2 Mixer */
            control.set("DL2 Mixer Multimedia", 0, 0);
            control.set("DL2 Mixer Tones", 0, 0);
            control.set("DL2 Mixer Voice", 0, 0);
        }
    }

    /* for input devices */
    if (devices >> 16) {
        if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            /* TWL6040 */
            control.set("Analog Left Capture Route", "Main Mic");	// Main Mic -> Mic Mux
            control.set("Analog Right Capture Route", "Sub Mic");	// Sub Mic  -> Mic Mux
            control.set("Capture Preamplifier Volume", 1);
            control.set("Capture Volume", 4);
        } else if (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
            /* TWL6040 */
            control.set("Analog Left Capture Route", "Headset Mic");	// Headset Mic -> Mic Mux
            control.set("Analog Right Capture Route", "Headset Mic");	// Headset Mic -> Mic Mux
            control.set("Capture Preamplifier Volume", 1);
            control.set("Capture Volume", 4);
        } else {
            /* TWL6040 */
            control.set("Analog Left Capture Route", "Off");
            control.set("Analog Right Capture Route", "Off");
            control.set("Capture Preamplifier Volume", 0, -1);
            control.set("Capture Volume", 0, -1);
        }

        if ((devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) ||
            (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
            /* OMAP4 ABE */
            control.set("AMIC_UL PDM Switch", 1);			// PDM_UL1 -> AMIC_UL
            control.set("MUX_UL00", "AMic1");				// AMIC_UL -> MM_UL00
            control.set("MUX_UL01", "AMic0");				// AMIC_UL -> MM_UL01
            control.set("MUX_UL10", "AMic1");				// AMIC_UL -> MM_UL10
            control.set("MUX_UL11", "AMic0");				// AMIC_UL -> MM_UL11
            control.set("Voice Capture Mixer Capture", 1);		// VX_UL   -> VXREC_MIXER
            control.set("MUX_VX0", "AMic1");				// AMIC_UL -> VX_UL0
            control.set("MUX_VX1", "AMic0");				// AMIC_UL -> VX_UL1
	} else {
            /* OMAP4 ABE */
            control.set("AMIC_UL PDM Switch", 0, 0);
            control.set("MUX_UL00", "None");
            control.set("MUX_UL01", "None");
            control.set("MUX_UL10", "None");
            control.set("MUX_UL11", "None");
            control.set("Voice Capture Mixer Capture", 0, 0);
            control.set("MUX_VX0", "None");
            control.set("MUX_VX1", "None");
	}
    }
}

void setAlsaControls(alsa_handle_t *handle, uint32_t devices, int mode)
{
    AlsaControlSet set = (AlsaControlSet) handle->modPrivate;
    set(devices, mode);
    handle->curDev = devices;
    handle->curMode = mode;
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    LOGD("Initializing devices for OMAP4 ALSA module");

    list.clear();

    for (size_t i = 0; i < ARRAY_SIZE(_defaults); i++) {

        snd_pcm_uframes_t bufferSize = _defaults[i].bufferSize;

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
            bufferSize &= ~b;

        _defaults[i].module = module;
        _defaults[i].bufferSize = bufferSize;

        list.push_back(_defaults[i]);
    }

#ifdef AUDIO_MODEM_TI
    audioModem = new AudioModemAlsa();
#endif

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle, uint32_t devices, int mode)
{
    // Close off previously opened device.
    // It would be nice to determine if the underlying device actually
    // changes, but we might be recovering from an error or manipulating
    // mixer settings (see asound.conf).
    //
    s_close(handle);

    LOGD("open called for devices %08x in mode %d...", devices, mode);

    const char *stream = streamName(handle);
    const char *devName = deviceName(handle, devices, mode);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened
    setAlsaControls(handle, devices, mode);

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    int err = snd_pcm_open(&handle->handle, devName, direction(handle), 0);

    if (err < 0) {
        LOGE("Failed to initialize ALSA %s device '%s': %s", stream, devName, strerror(err));
        return NO_INIT;
    }

    err = setHardwareParams(handle);

    if (err == NO_ERROR) err = setSoftwareParams(handle);

    LOGI("Initialized ALSA %s device '%s'", stream, devName);
    return err;
}

static status_t s_close(alsa_handle_t *handle)
{
    status_t err = NO_ERROR;
    snd_pcm_t *h = handle->handle;
    handle->handle = 0;
    handle->curDev = 0;
    handle->curMode = 0;
    if (h) {
        snd_pcm_drain(h);
        err = snd_pcm_close(h);
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    LOGD("route called for devices %08x in mode %d...", devices, mode);

    if (!devices) {
        LOGV("Ignore the audio routing change as there's no device specified");
        return NO_ERROR;
    }

    if (handle->handle && handle->curDev == devices && handle->curMode == mode)
        ; // Nothing to do
    else if (handle->handle && (handle->devices & devices))
        setAlsaControls(handle, devices, mode);
    else {
        LOGE("Why are we routing to a device that isn't supported by this object?!?!?!?!");
        status = s_open(handle, devices, mode);
        #ifdef AUDIO_MODEM_TI
            ALSAControl control("hw:00");
            status = audioModem->voiceCallControls(devices, mode, &control);
        #endif
    }

    return status;
}
}
