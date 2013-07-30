/* qsv_common.c
 *
 * Copyright (c) 2003-2013 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "common.h"
#include "hb_dict.h"
#include "qsv_common.h"
#include "h264_common.h"

// avoids a warning
#include "libavutil/cpu.h"
extern void ff_cpu_cpuid(int index, int *eax, int *ebx, int *ecx, int *edx);

// make the Intel QSV information available to the UIs
hb_qsv_info_t *hb_qsv_info = NULL;

// availability and versions
static mfxVersion qsv_hardware_version;
static mfxVersion qsv_software_version;
static mfxVersion qsv_minimum_version;
static int qsv_hardware_available = 0;
static int qsv_software_available = 0;
static char cpu_name_buf[48];

// check available Intel Media SDK version against a minimum
#define HB_CHECK_MFX_VERSION(MFX_VERSION, MAJOR, MINOR) \
    (MFX_VERSION.Major == MAJOR  && MFX_VERSION.Minor >= MINOR)

int hb_qsv_available()
{
    return hb_qsv_info != NULL && (qsv_hardware_available ||
                                   qsv_software_available);
}

int hb_qsv_info_init()
{
    static int init_done = 0;
    if (init_done)
        return (hb_qsv_info == NULL);
    init_done = 1;

    hb_qsv_info = calloc(1, sizeof(*hb_qsv_info));
    if (hb_qsv_info == NULL)
    {
        hb_error("hb_qsv_info_init: alloc failure");
        return -1;
    }

    hb_qsv_info->cpu_name = NULL;
    // detect the CPU platform to check for hardware-specific capabilities
    if (av_get_cpu_flags() & AV_CPU_FLAG_SSE)
    {
        int eax, ebx, ecx, edx;
        int family = 0, model = 0;

        ff_cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
        family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
        model  = ((eax >> 4) & 0xf) + ((eax >> 12) & 0xf0);

        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A
        // Figure 3-8: Determination of Support for the Processor Brand String
        // Table 3-17: Information Returned by CPUID Instruction
        ff_cpu_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if ((eax & 0x80000004) < 0x80000004)
        {
            ff_cpu_cpuid(0x80000002,
                         (int*)&cpu_name_buf[ 0], (int*)&cpu_name_buf[ 4],
                         (int*)&cpu_name_buf[ 8], (int*)&cpu_name_buf[12]);
            ff_cpu_cpuid(0x80000003,
                         (int*)&cpu_name_buf[16], (int*)&cpu_name_buf[20],
                         (int*)&cpu_name_buf[24], (int*)&cpu_name_buf[28]);
            ff_cpu_cpuid(0x80000004,
                         (int*)&cpu_name_buf[32], (int*)&cpu_name_buf[36],
                         (int*)&cpu_name_buf[40], (int*)&cpu_name_buf[44]);

            cpu_name_buf[47]      = '\0'; // just in case
            hb_qsv_info->cpu_name = (const char*)cpu_name_buf;
            while (isspace(*hb_qsv_info->cpu_name))
            {
                // skip leading whitespace to prettify
                hb_qsv_info->cpu_name++;
            }
        }
        
        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3C
        // Table 35-1: CPUID Signature Values of DisplayFamily_DisplayModel
        if (family == 0x06)
        {
            switch (model)
            {
                case 0x2A:
                case 0x2D:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_SNB;
                    break;
                case 0x3A:
                case 0x3E:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_IVB;
                    break;
                case 0x3C:
                case 0x45:
                case 0x46:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_HSW;
                    break;
                default:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_UNSPECIFIED;
                    break;
            }
        }
    }

    mfxSession session;
    qsv_minimum_version.Major = HB_QSV_MINVERSION_MAJOR;
    qsv_minimum_version.Minor = HB_QSV_MINVERSION_MINOR;

    // check for software fallback
    if (MFXInit(MFX_IMPL_SOFTWARE,
                &qsv_minimum_version, &session) == MFX_ERR_NONE)
    {
        qsv_software_available = 1;
        // our minimum is supported, but query the actual version
        MFXQueryVersion(session, &qsv_software_version);
        MFXClose(session);
    }

    // check for actual hardware support
    if (MFXInit(MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY,
                &qsv_minimum_version, &session) == MFX_ERR_NONE)
    {
        qsv_hardware_available = 1;
        // our minimum is supported, but query the actual version
        MFXQueryVersion(session, &qsv_hardware_version);
        MFXClose(session);
    }

    // check for version-specific or hardware-specific capabilities
    // we only use software as a fallback, so check hardware first
    if (qsv_hardware_available)
    {
        if (HB_CHECK_MFX_VERSION(qsv_hardware_version, 1, 6))
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_BRC;
            hb_qsv_info->capabilities |= HB_QSV_CAP_BITSTREAM_DTS;
        }
        if (HB_CHECK_MFX_VERSION(qsv_hardware_version, 1, 7))
        {
            if (hb_qsv_info->cpu_platform == HB_CPU_PLATFORM_INTEL_HSW)
            {
                hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_LOOKAHEAD;
            }
        }
        if (hb_qsv_info->cpu_platform == HB_CPU_PLATFORM_INTEL_HSW)
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_H264_BPYRAMID;
        }
    }
    else if (qsv_software_available)
    {
        if (HB_CHECK_MFX_VERSION(qsv_software_version, 1, 6))
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_BRC;
            hb_qsv_info->capabilities |= HB_QSV_CAP_BITSTREAM_DTS;
            hb_qsv_info->capabilities |= HB_QSV_CAP_H264_BPYRAMID;
        }
    }

    // note: we pass a pointer to MFXInit but it never gets modified
    //       let's make sure of it just to be safe though
    if (qsv_minimum_version.Major != HB_QSV_MINVERSION_MAJOR ||
        qsv_minimum_version.Minor != HB_QSV_MINVERSION_MINOR)
    {
        hb_error("hb_qsv_info_init: minimum version (%d.%d) was modified",
                 qsv_minimum_version.Major,
                 qsv_minimum_version.Minor);
    }

    // success
    return 0;
}

// we don't need it beyond this point
#undef HB_CHECK_MFX_VERSION

void hb_qsv_info_print()
{
    if (hb_qsv_info == NULL)
        return;

    // is QSV available?
    hb_log("Intel Quick Sync Video support: %s",
           hb_qsv_available() ? "yes": "no");

    // print the hardware summary too
    hb_log(" - CPU name: %s", hb_qsv_info->cpu_name);
    switch (hb_qsv_info->cpu_platform)
    {
        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3C
        // Table 35-1: CPUID Signature Values of DisplayFamily_DisplayModel
        case HB_CPU_PLATFORM_INTEL_SNB:
            hb_log(" - Intel microarchitecture Sandy Bridge");
            break;
        case HB_CPU_PLATFORM_INTEL_IVB:
            hb_log(" - Intel microarchitecture Ivy Bridge");
            break;
        case HB_CPU_PLATFORM_INTEL_HSW:
            hb_log(" - Intel microarchitecture Haswell");
            break;
        default:
            break;
    }

    // if we have Quick Sync Video support, also print the details
    if (hb_qsv_available())
    {
        if (qsv_hardware_available)
        {
            hb_log(" - Intel Media SDK hardware: API %d.%d (minimum: %d.%d)",
                   qsv_hardware_version.Major,
                   qsv_hardware_version.Minor,
                   qsv_minimum_version.Major,
                   qsv_minimum_version.Minor);
        }
        if (qsv_software_available)
        {
            hb_log(" - Intel Media SDK software: API %d.%d (minimum: %d.%d)",
                   qsv_software_version.Major,
                   qsv_software_version.Minor,
                   qsv_minimum_version.Major,
                   qsv_minimum_version.Minor);
        }
    }
}

int hb_qsv_decode_setup(AVCodec **codec, enum AVCodecID codec_id)
{
    if (codec == NULL)
    {
        hb_error("hb_qsv_decode_setup: invalid codec");
        goto fail;
    }

    const char *codec_name = hb_qsv_decode_get_codec_name(codec_id);
    if (codec_name == NULL)
    {
        hb_error("hb_qsv_decode_setup: unsupported codec_id %d", codec_id);
        goto fail;
    }
    *codec = avcodec_find_decoder_by_name(codec_name);
    return (*codec != NULL);

fail:
    return 0;
}

int hb_qsv_decode_is_enabled(hb_job_t *job)
{
    return ((job != NULL && job->title->qsv_decode_support && job->qsv_decode) &&
            (job->vcodec & HB_VCODEC_QSV_MASK));
}

int hb_qsv_decode_is_supported(enum AVCodecID codec_id,
                               enum AVPixelFormat pix_fmt)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return (pix_fmt == AV_PIX_FMT_YUV420P);

        default:
            return 0;
    }
}

void hb_qsv_decode_init(AVCodecContext *context, av_qsv_config *qsv_config)
{
    if (context == NULL || qsv_config == NULL)
    {
        hb_error("hb_qsv_decode_init: invalid context or config");
        return;
    }

    context->hwaccel_context       = qsv_config;
    qsv_config->impl_requested     = MFX_IMPL_AUTO_ANY|MFX_IMPL_VIA_ANY;
    qsv_config->io_pattern         = MFX_IOPATTERN_OUT_OPAQUE_MEMORY;
    qsv_config->sync_need          = 0;
    qsv_config->usage_threaded     = 1;
    qsv_config->additional_buffers = 64; // FIFO_LARGE
    if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_LOOKAHEAD)
    {
        // more surfaces may be needed for the lookahead
        qsv_config->additional_buffers = 160;
    }
}

const char* hb_qsv_decode_get_codec_name(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return "h264_qsv";

        default:
            return NULL;
    }
}

int hb_qsv_codingoption_xlat(int val)
{
    switch (HB_QSV_CLIP3(-1, 2, val))
    {
        case 0:
            return MFX_CODINGOPTION_OFF;
        case 1:
        case 2: // MFX_CODINGOPTION_ADAPTIVE, reserved
            return MFX_CODINGOPTION_ON;
        case -1:
        default:
            return MFX_CODINGOPTION_UNKNOWN;
    }
}
int hb_qsv_atoindex(const char* const *arr, const char *str, int *err)
{
    int i;
    for (i = 0; arr[i] != NULL; i++)
    {
        if (!strcasecmp(arr[i], str))
        {
            break;
        }
    }
    *err = (arr[i] == NULL);
    return i;
}
// adapted from libx264
int hb_qsv_atobool(const char *str, int *err)
{
    if (!strcasecmp(str,    "1") ||
        !strcasecmp(str,  "yes") ||
        !strcasecmp(str, "true"))
    {
        return 1;
    }
    if (!strcasecmp(str,     "0") ||
        !strcasecmp(str,    "no") ||
        !strcasecmp(str, "false"))
    {
        return 0;
    }
    *err = 1;
    return 0;
}
int hb_qsv_atoi(const char *str, int *err)
{
    char *end;
    int v = strtol(str, &end, 0);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}
float hb_qsv_atof(const char *str, int *err)
{
    char *end;
    float v = strtod(str, &end);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}

void hb_qsv_param_parse_all(hb_qsv_param_t *param,
                            const char *advanced_opts, int vcodec)
{
    if (advanced_opts != NULL && advanced_opts[0] != '\0')
    {
        hb_dict_entry_t *option = NULL;
        hb_dict_t *options_list = hb_encopts_to_dict(advanced_opts, vcodec);
        while ((option = hb_dict_next(options_list, option)) != NULL)
        {
            switch (hb_qsv_param_parse(param, option->key, option->value, vcodec))
            {
                case HB_QSV_PARAM_OK:
                    break;
                case HB_QSV_PARAM_BAD_NAME:
                    hb_log("hb_qsv_param_parse_all: bad key %s", option->key);
                    break;
                case HB_QSV_PARAM_BAD_VALUE:
                    hb_log("hb_qsv_param_parse_all: bad value %s for key %s",
                           option->value, option->key);
                    break;
                case HB_QSV_PARAM_UNSUPPORTED:
                    hb_log("hb_qsv_param_parse_all: unsupported option %s",
                           option->key);
                    break;
                case HB_QSV_PARAM_ERROR:
                default:
                    hb_log("hb_qsv_param_parse_all: unknown error");
                    break;
            }
        }
        hb_dict_free(&options_list);
    }
}

int hb_qsv_param_parse(hb_qsv_param_t *param,
                       const char *key, const char *value, int vcodec)
{
    float fvalue;
    int ivalue, error = 0;
    if (param == NULL)
    {
        return HB_QSV_PARAM_ERROR;
    }
    if (value == NULL || value[0] == '\0')
    {
        value = "true";
    }
    else if (value[0] == '=')
    {
        value++;
    }
    if (key == NULL || key[0] == '\0')
    {
        return HB_QSV_PARAM_BAD_NAME;
    }
    else if (!strncasecmp(key, "no-", 3))
    {
        key  += 3;
        value = hb_qsv_atobool(value, &error) ? "false" : "true";
        if (error)
        {
            return HB_QSV_PARAM_BAD_VALUE;
        }
    }
    if (!strcasecmp(key, "async-depth"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam.AsyncDepth = HB_QSV_CLIP3(0, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "target-usage"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam.mfx.TargetUsage = HB_QSV_CLIP3(1, 7, ivalue);
        }
    }
    else if (!strcasecmp(key, "num-ref-frame"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam.mfx.NumRefFrame = HB_QSV_CLIP3(0, 16, ivalue);
        }
    }
    else if (!strcasecmp(key, "gop-ref-dist"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->videoParam.mfx.GopRefDist = HB_QSV_CLIP3(0, 32, ivalue);
        }
    }
    else if (!strcasecmp(key, "gop-pic-size"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->gop.gop_pic_size = HB_QSV_CLIP3(-1, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "scenecut"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            if (!ivalue)
            {
                param->videoParam.mfx.GopOptFlag |= MFX_GOP_STRICT;
            }
            else
            {
                param->videoParam.mfx.GopOptFlag &= ~MFX_GOP_STRICT;
            }
        }
    }
    else if (!strcasecmp(key, "cqp-offset-i"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[0] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-p"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[1] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-b"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[2] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-init"))
    {
        fvalue = hb_qsv_atof(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_init = HB_QSV_CLIP3(0, UINT16_MAX, fvalue);
        }
    }
    else if (!strcasecmp(key, "vbv-bufsize"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_size = HB_QSV_CLIP3(0, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-maxrate"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_max_bitrate = HB_QSV_CLIP3(0, UINT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cabac"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            param->codingOption.CAVLC = hb_qsv_codingoption_xlat(ivalue);
        }
    }
    else if (!strcasecmp(key, "rdo"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            param->codingOption.RateDistortionOpt = hb_qsv_codingoption_xlat(ivalue);
        }
    }
    else if (!strcasecmp(key, "videoformat"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atoindex(x264_vidformat_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.VideoFormat = ivalue;
        }
    }
    else if (!strcasecmp(key, "fullrange"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atoindex(x264_fullrange_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.VideoFullRange = ivalue;
        }
    }
    else if (!strcasecmp(key, "colorprim"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atoindex(x264_colorprim_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.ColourPrimaries = ivalue;
        }
    }
    else if (!strcasecmp(key, "transfer"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atoindex(x264_transfer_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.TransferCharacteristics = ivalue;
        }
    }
    else if (!strcasecmp(key, "colormatrix"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atoindex(x264_colmatrix_names, value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            param->videoSignalInfo.ColourDescriptionPresent = 1;
            param->videoSignalInfo.MatrixCoefficients = ivalue;
        }
    }
    else if (!strcasecmp(key, "tff") ||
             !strcasecmp(key, "interlaced"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            if (ivalue)
            {
                param->videoParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_FIELD_TFF;
            }
            else
            {
                param->videoParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
            }
        }
    }
    else if (!strcasecmp(key, "bff"))
    {
        switch (vcodec)
        {
            case HB_VCODEC_QSV_H264:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            if (ivalue)
            {
                param->videoParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_FIELD_BFF;
            }
            else
            {
                param->videoParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
            }
        }
    }
    else if (!strcasecmp(key, "mbbrc"))
    {
        if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_BRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.MBBRC = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "extbrc"))
    {
        if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_BRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->codingOption2.ExtBRC = hb_qsv_codingoption_xlat(ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead"))
    {
        if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_LOOKAHEAD)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->rc.lookahead = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead-depth"))
    {
        if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_LOOKAHEAD)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                param->codingOption2.LookAheadDepth = HB_QSV_CLIP3(10, 100, ivalue);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else
    {
        /*
         * TODO:
         * - trellis
         * - slice count control
         * - open-gop
         * - fake-interlaced (mfxExtCodingOption.FramePicture???)
         * - intra-refresh
         */
        return HB_QSV_PARAM_BAD_NAME;
    }
    return error ? HB_QSV_PARAM_BAD_VALUE : HB_QSV_PARAM_OK;
}

void hb_qsv_param_default(hb_qsv_param_t *param)
{
    if (param != NULL)
    {
        // introduced in API 1.0
        memset(&param->codingOption, 0, sizeof(mfxExtCodingOption));
        param->codingOption.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
        param->codingOption.Header.BufferSz      = sizeof(mfxExtCodingOption);
        param->codingOption.MECostType           = 0; // reserved, must be 0
        param->codingOption.MESearchType         = 0; // reserved, must be 0
        param->codingOption.MVSearchWindow.x     = 0; // reserved, must be 0
        param->codingOption.MVSearchWindow.y     = 0; // reserved, must be 0
        param->codingOption.RefPicListReordering = 0; // reserved, must be 0
        param->codingOption.IntraPredBlockSize   = 0; // reserved, must be 0
        param->codingOption.InterPredBlockSize   = 0; // reserved, must be 0
        param->codingOption.MVPrecision          = 0; // reserved, must be 0
        param->codingOption.EndOfSequence        = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.RateDistortionOpt    = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.CAVLC                = MFX_CODINGOPTION_OFF;
        param->codingOption.ResetRefList         = MFX_CODINGOPTION_OFF;
        param->codingOption.MaxDecFrameBuffering = 0; // unspecified
        param->codingOption.AUDelimiter          = MFX_CODINGOPTION_OFF;
        param->codingOption.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.PicTimingSEI         = MFX_CODINGOPTION_OFF;
        param->codingOption.VuiNalHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.FramePicture         = MFX_CODINGOPTION_UNKNOWN;
        // introduced in API 1.3
        param->codingOption.RefPicMarkRep        = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.FieldOutput          = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.NalHrdConformance    = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption.VuiVclHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
        // introduced in API 1.4
        param->codingOption.ViewOutput           = MFX_CODINGOPTION_UNKNOWN;
        // introduced in API 1.6
        param->codingOption.RecoveryPointSEI     = MFX_CODINGOPTION_UNKNOWN;
        
        // introduced in API 1.3
        memset(&param->videoSignalInfo, 0, sizeof(mfxExtVideoSignalInfo));
        param->videoSignalInfo.Header.BufferId          = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
        param->videoSignalInfo.Header.BufferSz          = sizeof(mfxExtVideoSignalInfo);
        param->videoSignalInfo.VideoFormat              = 5; // undefined
        param->videoSignalInfo.VideoFullRange           = 0; // TV range
        param->videoSignalInfo.ColourDescriptionPresent = 0; // don't write to bitstream
        param->videoSignalInfo.ColourPrimaries          = 2; // undefined
        param->videoSignalInfo.TransferCharacteristics  = 2; // undefined
        param->videoSignalInfo.MatrixCoefficients       = 2; // undefined

        // introduced in API 1.6
        memset(&param->codingOption2, 0, sizeof(mfxExtCodingOption2));
        param->codingOption2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        param->codingOption2.Header.BufferSz = sizeof(mfxExtCodingOption2);
        param->codingOption2.IntRefType      = 0;
        param->codingOption2.IntRefCycleSize = 2;
        param->codingOption2.IntRefQPDelta   = 0;
        param->codingOption2.MaxFrameSize    = 0;
        param->codingOption2.BitrateLimit    = MFX_CODINGOPTION_ON;
        param->codingOption2.MBBRC           = MFX_CODINGOPTION_UNKNOWN;
        param->codingOption2.ExtBRC          = MFX_CODINGOPTION_OFF;
        // introduced in API 1.7
        param->codingOption2.LookAheadDepth  = 40;
        param->codingOption2.Trellis         = MFX_TRELLIS_UNKNOWN;

        // introduced in API 1.0
        memset(&param->videoParam, 0, sizeof(mfxVideoParam));
        param->videoParam.Protected        = 0; // reserved, must be 0
        param->videoParam.NumExtParam      = 0;
        param->videoParam.IOPattern        = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        param->videoParam.mfx.TargetUsage  = MFX_TARGETUSAGE_2;
        param->videoParam.mfx.GopOptFlag   = MFX_GOP_CLOSED;
        param->videoParam.mfx.NumThread    = 0; // deprecated, must be 0
        param->videoParam.mfx.EncodedOrder = 0; // input is in display order
        param->videoParam.mfx.IdrInterval  = 0; // all I-frames are IDR
        param->videoParam.mfx.NumSlice     = 0; // use Media SDK default
        param->videoParam.mfx.NumRefFrame  = 0; // use Media SDK default
        param->videoParam.mfx.GopPicSize   = 0; // use Media SDK default
        param->videoParam.mfx.GopRefDist   = 4; // power of 2, >= 4: B-pyramid
        // introduced in API 1.1
        param->videoParam.AsyncDepth = AV_QSV_ASYNC_DEPTH_DEFAULT;
        // introduced in API 1.3
        param->videoParam.mfx.BRCParamMultiplier = 0; // no multiplier

        // FrameInfo: dummy default values
        param->videoParam.mfx.FrameInfo.FrameRateExtN = 25;
        param->videoParam.mfx.FrameInfo.FrameRateExtD =  1;
        param->videoParam.mfx.FrameInfo.AspectRatioW  =  1;
        param->videoParam.mfx.FrameInfo.AspectRatioH  =  1;
        param->videoParam.mfx.FrameInfo.CropX         =  0;
        param->videoParam.mfx.FrameInfo.CropY         =  0;
        param->videoParam.mfx.FrameInfo.CropW         = 32;
        param->videoParam.mfx.FrameInfo.CropH         = 32;
        param->videoParam.mfx.FrameInfo.Width         = 32;
        param->videoParam.mfx.FrameInfo.Height        = 32;
        param->videoParam.mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
        param->videoParam.mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
        param->videoParam.mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;

        // GOP & rate control
        param->gop.gop_pic_size       = -1; // set automatically
        param->gop.int_ref_cycle_size = -1; // set automatically
        param->rc.lookahead           = -1; // set automatically
        param->rc.cqp_offsets[0]      =  0;
        param->rc.cqp_offsets[1]      =  2;
        param->rc.cqp_offsets[2]      =  4;
        param->rc.vbv_max_bitrate     =  0;
        param->rc.vbv_buffer_size     =  0;
        param->rc.vbv_buffer_init     = .9;

        // attach supported mfxExtBuffer structures to the mfxVideoParam
        param->videoParam.NumExtParam = 0;
        param->videoParam.ExtParam = param->ExtParamArray;
        param->videoParam.ExtParam[param->videoParam.NumExtParam++] = (mfxExtBuffer*)&param->codingOption;
        param->videoParam.ExtParam[param->videoParam.NumExtParam++] = (mfxExtBuffer*)&param->videoSignalInfo;
        if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_BRC)
        {
            param->videoParam.ExtParam[param->videoParam.NumExtParam++] = (mfxExtBuffer*)&param->codingOption2;
        }
    }
}
