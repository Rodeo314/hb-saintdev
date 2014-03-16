/*
 * decqsv.c
 *
 * Copyright (c) 2003-2014 HandBrake Team
 *
 * This file is part of the HandBrake source code
 *
 * Homepage: <http://handbrake.fr/>.
 *
 * It may be used under the terms of the GNU General Public License v2.
 *
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifdef USE_QSV

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "msdk/mfxvideo.h"

#include "common.h"
#include "hb.h"
#include "hbffmpeg.h"
#include "qsv_common.h"

/*
 * Named return values for frequent, non-error conditions.
 */
#define HEADER_DATA_NOT_FOUND 255
#define BITSTREAM_DECODE_DONE 254

/*
 * Minimum sizes for some data units, based on the applicable specification.
 */
#define H264_PPS_MIN_SIZE     2 // 16 bits or more
#define H264_SPS_MIN_SIZE     5 // 35 bits or more
#define H264_SPS_EXT_MIN_SIZE 1 //  3 bits or more

/*
 * Common data patterns we need to insert frequently.
 */
static const uint8_t h264_empty_idr_slice[] = { 0x65, };
static const uint8_t nal_annexb_startcode[] = { 0x00, 0x00, 0x00, 0x01, };

static void decqsvClose(hb_work_object_t*);
static int  decqsvInit (hb_work_object_t*, hb_job_t*);
static int  decqsvWork (hb_work_object_t*, hb_buffer_t**, hb_buffer_t**);

hb_work_object_t hb_decqsv =
{
    .id    = WORK_DECQSV,
    .name  = "Video decoder (Intel Media SDK)",
    .close = decqsvClose,
    .init  = decqsvInit,
    .work  = decqsvWork,
};

struct hb_work_private_s
{
    hb_job_t *job;

    int initialized;
    hb_qsv_info_t   *info;
    mfxSession       session;
    mfxBitstream     bitstream;
    mfxVideoParam    videoParam;
    int              nsurfaces;
    int              surface_size;
    uint8_t         *surface_data;
    mfxFrameSurface1 surfaces[256];

    struct SwsContext *sws_context;
    hb_list_t *decoded_frames;
    int64_t default_duration;
    int64_t next_pts;

    struct
    {
        uint8_t annexb;
        uint8_t length_size;
    } nalus;
};

static void decqsvClose(hb_work_object_t *w)
{
    hb_work_private_t *pv = w->private_data;
    if (pv != NULL)
    {
        if (pv->sws_context != NULL)
        {
            sws_freeContext(pv->sws_context);
        }

        if (pv->decoded_frames != NULL)
        {
            hb_buffer_t *buf;

            while ((buf = hb_list_item(pv->decoded_frames, 0)) != NULL)
            {
                hb_list_rem(pv->decoded_frames, buf);
                hb_buffer_close(&buf);
            }

            hb_list_close(&pv->decoded_frames);
        }

        MFXVideoDECODE_Close(pv->session);
        MFXClose            (pv->session);

        av_free(pv->bitstream.Data);
        av_free(pv->surface_data);
    }
    free(pv);
    w->private_data = NULL;
}

static size_t nal_unit_annexb_write(uint8_t *buf, const uint8_t *nal_unit,
                                    size_t nal_unit_size)
{
    memcpy(buf, nal_annexb_startcode, sizeof(nal_annexb_startcode));
    memcpy(buf + sizeof(nal_annexb_startcode), nal_unit, nal_unit_size);
    return sizeof(nal_annexb_startcode) + nal_unit_size;
}

static uint8_t* nal_unit_find_next(uint8_t *start, size_t *size)
{
    uint8_t *nal = NULL;
    uint8_t *buf = start;
    uint8_t *end = start + *size;

    /* Look for an Annex B start code prefix (3-byte sequence == 1) */
    while (end - buf > 3)
    {
        if (!buf[0] && !buf[1] && buf[2] == 1)
        {
            nal = (buf += 3); // NAL unit begins after start code
            break;
        }
        buf++;
    }

    if (nal == NULL)
    {
        *size = 0;
        return NULL;
    }

    /*
     * Start code prefix found, look for the next one to determine the size
     *
     * A 4-byte sequence == 1 is also a start code, so check for a 3-byte
     * sequence == 0 too (start code emulation prevention will prevent such a
     * sequence from occurring outside of a start code prefix)
     */
    while (end - buf > 3)
    {
        if (!buf[0] && !buf[1] && (!buf[2] || buf[2] == 1))
        {
            end = buf;
            break;
        }
        buf++;
    }

    *size = end - nal;
    return nal;
}

static int decode_h264_header(hb_work_private_t *pv, uint8_t *data, int size,
                              hb_buffer_t **out)
{
    int ret = 0;
    hb_buffer_t *header = NULL;
    size_t buf_size, header_size, min_size;
    uint8_t *buf, header_data[1024], *nal_unit;

    /* NULL input just means we'll get extradata later */
    if (data == NULL)
    {
        ret = HEADER_DATA_NOT_FOUND;
        goto end;
    }

    buf_size = size;
    nal_unit = nal_unit_find_next(data, &buf_size);

    if (nal_unit != NULL)
    {
        /* ITU-T Recommendation H.264, Annex B */
        uint8_t nal_type, sps_count = 0, pps_count = 0;

        pv->nalus.annexb = 1;

        buf = header_data;

        /* Check all NALs and write any parameter sets to the header */
        do
        {
            nal_type = nal_unit[0] & 0x1f;

            if (nal_type == 7 || nal_type == 8 || nal_type == 13)
            {
                /* We need additional space for the Annex B start code */
                header_size = buf - header_data;
                min_size = header_size + buf_size + sizeof(nal_annexb_startcode);
                if (sizeof(header_data) < min_size)
                {
                    hb_log("decqsv: header buffer too small (size: %lu, min_size: %lu)",
                           sizeof(header_data), min_size);
                    ret = -1;
                    goto end;
                }

                sps_count += nal_type == 7;
                pps_count += nal_type == 8;
                buf       += nal_unit_annexb_write(buf, nal_unit, buf_size);
            }

            nal_unit += buf_size;
            buf_size  = data - nal_unit + size;
            nal_unit  = nal_unit_find_next(nal_unit, &buf_size);
        }
        while (nal_unit != NULL);

        /* Valid input, but without parameter sets, we have to try again */
        if (!sps_count || !pps_count)
        {
            ret = HEADER_DATA_NOT_FOUND;
            goto end;
        }
        header_size = buf - header_data;
    }
    else if (size > 6 && data[0] == 1)
    {
        /* ISO/IEC 14496-15, DecoderConfigurationRecord */
        uint8_t i, numOfSequenceParameterSets, numOfPictureParameterSets;
        size_t pos;

        pv->nalus.annexb      = 0;
        pv->nalus.length_size = (data[4] & 0x3) + 1;

        if (data[5] & 0x80)
        {
            numOfSequenceParameterSets = data[5] & 0x1f; // avcC
        }
        else
        {
            numOfSequenceParameterSets = data[5] & 0xef; // mvcC/svcC
        }

        buf = header_data;
        pos = 6; // first SPS

        /*
         * we need at least enough data for:
         * - numOfSequenceParameterSets SPS NALs of length >= H264_SPS_MIN_SIZE
         * - numOfSequenceParameterSets 2-byte sequenceParameterSetLength fields
         * - 1-byte numOfPictureParameterSets field
         */
        min_size = pos + numOfSequenceParameterSets * (H264_SPS_MIN_SIZE + 2) + 1;
        if (size < min_size)
        {
            hb_log("decqsv: avcC/mvcC/svcC too short (size: %lu, min_size: %lu)",
                   size, min_size);
            ret = -1;
            goto end;
        }

        for (i = 0; i < numOfSequenceParameterSets; i++)
        {
            size_t sps_length = (data[pos] << 8) | data[pos + 1];
            if (sps_length < H264_SPS_MIN_SIZE)
            {
                hb_log("decqsv: SPS NAL unit too short (size: %lu, min_size: %lu)",
                       sps_length, H264_SPS_MIN_SIZE);
                ret = -1;
                goto end;
            }

            /* We need additional space for the Annex B start code */
            header_size = buf - header_data;
            min_size = header_size + sps_length + sizeof(nal_annexb_startcode);
            if (sizeof(header_data) < min_size)
            {
                hb_log("decqsv: header buffer too small (size: %lu, min_size: %lu)",
                       sizeof(header_data), min_size);
                ret = -1;
                goto end;
            }

            buf += nal_unit_annexb_write(buf, &data[pos + 2], sps_length);
            pos += sps_length + 2;
        }

        numOfPictureParameterSets = data[pos++];

        /*
         * we need at least enough data for:
         * - numOfPictureParameterSets PPS NALs of length >= H264_PPS_MIN_SIZE
         * - numOfPictureParameterSets 2-byte pictureParameterSetLength fields
         */
        min_size = pos + numOfPictureParameterSets * (H264_PPS_MIN_SIZE + 2);
        if (size < min_size)
        {
            hb_log("decqsv: avcC/mvcC/svcC too short (size: %lu, min_size: %lu)",
                   size, min_size);
            ret = -1;
            goto end;
        }

        for (i = 0; i < numOfPictureParameterSets; i++)
        {
            size_t pps_length = (data[pos] << 8) | data[pos + 1];
            if (pps_length < H264_PPS_MIN_SIZE)
            {
                hb_log("decqsv: PPS NAL unit too short (size: %lu, min_size: %lu)",
                       pps_length, H264_PPS_MIN_SIZE);
                ret = -1;
                goto end;
            }

            /* We need additional space for the Annex B start code */
            header_size = buf - header_data;
            min_size = header_size + pps_length + sizeof(nal_annexb_startcode);
            if (sizeof(header_data) < min_size)
            {
                hb_log("decqsv: header buffer too small (size: %lu, min_size: %lu)",
                       sizeof(header_data), min_size);
                ret = -1;
                goto end;
            }

            buf += nal_unit_annexb_write(buf, &data[pos + 2], pps_length);
            pos += pps_length + 2;
        }

        /*
         * ISO/IEC 14496-15, Third Edition features these additional fields, but
         * ISO/IEC 14496-15, First Edition doesn't - only attempt to read them
         * if we have more data available, and don't fail if we don't have it
         */
        if ((size >= pos + 4) &&
            (data[1] == 100 || // profile: High
             data[1] == 110 || // profile: High 10
             data[1] == 122 || // profile: High 4:2:2
             data[1] == 144))  // profile: High 4:4:4 (deprecated)
        {
            /*
             * bit(6) reserved = ‘111111’b;
             * unsigned int(2) chroma_format;
             * bit(5) reserved = ‘11111’b;
             * unsigned int(3) bit_depth_luma_minus8;
             * bit(5) reserved = ‘11111’b;
             * unsigned int(3) bit_depth_chroma_minus8;
             * unsigned int(8) numOfSequenceParameterSetExt;
             */
            uint8_t numOfSequenceParameterSetExt = data[pos + 3];
            pos += 4;

            /*
             * we need at least enough data for:
             * - numOfSequenceParameterSetExt SPS_EXT NALs of length >= H264_SPS_EXT_MIN_SIZE
             * - numOfSequenceParameterSetExt 2-byte sequenceParameterSetExtLength fields
             */
            min_size = pos + numOfSequenceParameterSetExt * (H264_SPS_EXT_MIN_SIZE + 2);
            if (size < min_size)
            {
                hb_log("decqsv: avcC/mvcC/svcC too short (size: %lu, min_size: %lu)",
                       size, min_size);
                ret = -1;
                goto end;
            }

            for (i = 0; i < numOfSequenceParameterSetExt; i++)
            {
                size_t sps_ext_length = (data[pos] << 8) | data[pos + 1];
                if (sps_ext_length < H264_SPS_EXT_MIN_SIZE)
                {
                    hb_log("SPS_EXT NAL unit too short (size: %lu, min_size: %lu)",
                           sps_ext_length, H264_SPS_EXT_MIN_SIZE);
                    ret = -1;
                    goto end;
                }

                /* We need additional space for the Annex B start code */
                header_size = buf - header_data;
                min_size = header_size + sps_ext_length + sizeof(nal_annexb_startcode);
                if (sizeof(header_data) < min_size)
                {
                    hb_log("decqsv: header buffer too small (size: %lu, min_size: %lu)",
                           sizeof(header_data), min_size);
                    ret = -1;
                    goto end;
                }

                buf += nal_unit_annexb_write(buf, &data[pos + 2], sps_ext_length);
                pos += sps_ext_length + 2;
            }
        }

        if (size < pos)
        {
            hb_log("decqsv: overread avcC/mvcC/svcC with size: %lu by %lu bytes",
                   size, pos - size);
            ret = -1;
            goto end;
        }

        /* Valid input, but without parameter sets, we have to try again */
        if (!numOfSequenceParameterSets || !numOfPictureParameterSets)
        {
            ret = HEADER_DATA_NOT_FOUND;
            goto end;
        }
        header_size = buf - header_data;
    }
    else
    {
        hb_log("decqsv: invalid H.264 extradata (size: %lu)", size);
        ret = -1;
        goto end;
    }

    /*
     * MFXVideoDECODE_DecodeHeader expects to find a slice after the
     * parameter sets; but an empty slice is enough to make it happy
     */
    min_size = (header_size +
                sizeof(h264_empty_idr_slice) +
                sizeof(nal_annexb_startcode));
    if (sizeof(header_data) < min_size)
    {
        hb_log("decqsv: header buffer too small (size: %lu, min_size: %lu)",
               sizeof(header_data), min_size);
        ret = -1;
        goto end;
    }
    else
    {
        header_size += nal_unit_annexb_write(header_data + header_size,
                                             h264_empty_idr_slice,
                                             sizeof(h264_empty_idr_slice));
    }

    header = hb_buffer_init(header_size);
    if (header == NULL)
    {
        hb_log("decqsv: hb_buffer_init failed");
        ret = -1;
        goto end;
    }
    memcpy(header->data, header_data, header->size);

end:
    *out = header;
    return ret;
}

static int qsv_decode_init(hb_work_private_t *pv, uint8_t *data, int size)
{
    uint8_t *header_data;
    mfxBitstream bitstream;
    mfxVideoParam videoParam;
    hb_buffer_t *header_buf = NULL;
    mfxFrameAllocRequest frameAllocRequest;
    int i, ret, header_size, width, height;

    /* we may have an AVCodecContext from libavformat with extradata */
    if (pv->job->title->opaque_priv != NULL)
    {
        AVFormatContext *ctx  = (AVFormatContext*)pv->job->title->opaque_priv;
        AVCodecContext *avctx = ctx->streams[pv->job->title->video_id]->codec;
        header_size           = avctx->extradata_size;
        header_data           = avctx->extradata;
    }
    else
    {
        header_size = size;
        header_data = data;
    }

    switch (pv->videoParam.mfx.CodecId)
    {
        case MFX_CODEC_AVC:
            ret = decode_h264_header(pv, header_data, header_size, &header_buf);
            break;
        default:
            ret = -1; // unreachable
            break;
    }

    if (ret < 0)
    {
        hb_error("decqsv: failed to decode header");
        ret = -1;
        goto end;
    }

    if (ret == HEADER_DATA_NOT_FOUND)
    {
        if (pv->job->title->opaque_priv != NULL)
        {
            /* Assume extradata must contain a header */
            hb_error("decqsv: no header in private data");
            ret = -1;
        }
        goto end;
    }

    memset(&bitstream,         0, sizeof(mfxBitstream));
    memset(&videoParam,        0, sizeof(mfxVideoParam));
    memset(&frameAllocRequest, 0, sizeof(mfxFrameAllocRequest));

    bitstream.Data       = header_buf->data;
    bitstream.DataLength = header_buf->size;
    bitstream.MaxLength  = header_buf->alloc;
    ret = MFXVideoDECODE_DecodeHeader(pv->session, &bitstream, &pv->videoParam);
    if (ret < MFX_ERR_NONE)
    {
        hb_error("decqsv: MFXVideoDECODE_DecodeHeader failed (%d)", ret);
        ret = -1;
        goto end;
    }

    ret = MFXVideoDECODE_QueryIOSurf(pv->session, &pv->videoParam, &frameAllocRequest);
    if (ret < MFX_ERR_NONE)
    {
        hb_error("decqsvWork: MFXVideoDECODE_QueryIOSurf failed (%d)", ret);
        ret = -1;
        goto end;
    }

    width            = FFALIGN(frameAllocRequest.Info.Width,  32);
    height           = FFALIGN(frameAllocRequest.Info.Height, 32);
    pv->surface_size = width * height * 12 / 8;

    pv->nsurfaces = frameAllocRequest.NumFrameSuggested;
    if (pv->nsurfaces > sizeof(pv->surfaces) / sizeof(pv->surfaces[0]))
    {
        hb_error("decqsv: too many surfaces requested (%d, max: %lu)",
                 pv->nsurfaces, sizeof(pv->surfaces) / sizeof(pv->surfaces[0]));
        ret = -1;
        goto end;
    }

    if (pv->videoParam.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY)
    {
        pv->surface_data = av_malloc_array(pv->surface_size, pv->nsurfaces);
        if (pv->surface_data == NULL)
        {
            hb_error("decqsv: failed to allocate surface_data");
            ret = -1;
            goto end;
        }

        for (i = 0; i < pv->nsurfaces; i++)
        {
            mfxFrameSurface1 *surface = &pv->surfaces[i];

            memset(surface, 0, sizeof(mfxFrameSurface1));

            surface->Info       = pv->videoParam.mfx.FrameInfo;
            surface->Data.Y     = pv->surface_data + pv->surface_size * i;
            surface->Data.U     = surface->Data.Y  + width * height;
            surface->Data.V     = surface->Data.U  + 1;
            surface->Data.Pitch = width;
        }
    }

    ret = MFXVideoDECODE_Init(pv->session, &pv->videoParam);
    if (ret < MFX_ERR_NONE)
    {
        hb_error("decqsv: MFXVideoDECODE_Init failed (%d)", ret);
        ret = -1;
        goto end;
    }

end:
    hb_buffer_close(&header_buf);
    return ret;
}

static int decqsvInit(hb_work_object_t *w, hb_job_t *job)
{
    hb_work_private_t *pv = calloc(1, sizeof(hb_work_private_t));
    pv->info              = hb_qsv_info_get(job->vcodec);
    pv->decoded_frames    = hb_list_init();
    pv->job               = job;
    w->private_data       = pv;
    int ret;

    /*
     * default to any available implementation
     * matching our minimum supported version
     */
    mfxIMPL implementation = MFX_IMPL_AUTO_ANY;
    mfxVersion version     =
    {
        .Major = HB_QSV_MINVERSION_MAJOR,
        .Minor = HB_QSV_MINVERSION_MINOR,
    };

    /*
     * if we're also encoding with QSV, we may need to
     * adjust the initialization parameters for decode
     */
    if (pv->info != NULL)
    {
        implementation = pv->info->implementation;
    }

    if (job->title->has_resolution_change)
    {
        hb_error("decqsvInit: resolution change unsupported");
        return 1;
    }

    /* set the codec ID and parse codec-specific extradata, if any */
    switch (job->title->video_codec_param)
    {
        case AV_CODEC_ID_H264:
            pv->videoParam.mfx.CodecId = MFX_CODEC_AVC;
            break;
        default:
            hb_error("decqsvInit: unsupported codec with ID %d",
                     job->title->video_codec_param);
            return 1;
    }

    ret = MFXInit(implementation, &version, &pv->session);
    if (ret < MFX_ERR_NONE)
    {
        hb_error("decqsvInit: MFXInit failed (%d)", ret);
        return 1;
    }

    pv->videoParam.AsyncDepth = 1; /* job->qsv.async_depth */
    pv->videoParam.IOPattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    if (pv->videoParam.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY)
    {
        /* we can prepare the swscale context here since resolution is constant */
        pv->sws_context = hb_sws_get_context(job->title->width, job->title->height,
                                             AV_PIX_FMT_NV12,
                                             job->title->width, job->title->height,
                                             AV_PIX_FMT_YUV420P,
                                             /* is it worth the speed hit? */
                                             SWS_ACCURATE_RND);

        if (pv->sws_context == NULL)
        {
            hb_error("decqsvInit: failed to initialize color space conversion");
            return 1;
        }
    }

    /*
     * Default frame duration (in ticks of a 90 kHz time base).
     *
     * Used to guess the input PTS when it isn't set (can happen
     * for e.g. MPEG transport streams with discontinuities).
     */
    pv->default_duration = ((double)90000            /
                            (double)job->title->rate *
                            (double)job->title->rate_base);

    return 0;
}

static hb_buffer_t* bitstream_mp4toannexb(hb_work_private_t *pv, hb_buffer_t *in)
{
    int i;
    hb_buffer_t *out;
    size_t nal_unit_size;
    uint8_t *buf, *end, *data;

    buf = in->data;
    end = in->data + in->size;
    out = hb_buffer_init(in->size);

    if (out == NULL)
    {
        hb_error("decqsv: memory allocation failure");
        *pv->job->done_error = HB_ERROR_UNKNOWN;
        *pv->job->die        = 1;
        return NULL;
    }
    data = out->data;

    while (end - buf > pv->nalus.length_size)
    {
        nal_unit_size = 0;

        /* In MP4, NAL units are preceded by a 2-4 byte length field */
        for (i = 0; i < pv->nalus.length_size; i++)
        {
            nal_unit_size |= buf[i] << (8 * (pv->nalus.length_size - 1 - i));
        }
        buf += pv->nalus.length_size;

        if (end - buf < nal_unit_size)
        {
            hb_log("decqsv: truncated bitstream (remaining: %lu, expected: %lu)",
                   end - buf, nal_unit_size);
            hb_buffer_close(&out);
            return NULL;
        }

        /* If out is already large enough, this has no effect */
        hb_buffer_realloc(out, data - out->data + nal_unit_size +
                          sizeof(nal_annexb_startcode));

        data += nal_unit_annexb_write(data, buf, nal_unit_size);
        buf  += nal_unit_size;
    }

    if (end - buf > 0)
    {
        hb_log("decqsv: bitstream has %lu bytes of trailing data", end - buf);
    }

    out->size = data - out->data;
    return out;
}

static int bitstream_append_data(hb_work_private_t *pv, hb_buffer_t *in)
{
    int size;
    uint8_t *data;
    hb_buffer_t *buf = NULL;
    pv->bitstream.TimeStamp = in->s.start;
    mfxBitstream *bitstream = &pv->bitstream;

    if (pv->nalus.annexb)
    {
        data = in->data;
        size = in->size;
    }
    else
    {
        buf = bitstream_mp4toannexb(pv, in);
        if (buf == NULL)
        {
            size = 0;
            goto end;
        }

        data = buf->data;
        size = buf->size;
    }

    //debug
    size_t len   = size;
    uint8_t *tmp = data;
    while (((tmp = nal_unit_find_next(tmp, &len)) != NULL))
    {
        hb_deep_log(2,
                    "decqsv, NAL unit: %"PRIu8" - %"PRIu8" - %02"PRIu8" (%8lu)",
                    ((tmp[0] & 0x80) >> 7),
                    ((tmp[0] & 0x60) >> 5),
                    ((tmp[0] & 0x1f) >> 0), len);
        tmp += len;
        len  = data - tmp + size;
    }

    /* Allocate or enlarge the bitstream buffer if necessary */
    if (bitstream->MaxLength - bitstream->DataLength < size)
    {
        size_t max_length = FFALIGN(bitstream->MaxLength  -
                                    bitstream->DataLength + size, 32);

        if (av_reallocp(&bitstream->Data, max_length) < 0)
        {
            hb_error("decqsv: failed to (re)allocate bitstream data buffer");
            size = -1;
            goto end;
        }

        bitstream->MaxLength = max_length;
    }

    /*
     * If we don't have enough room left to append the data, make
     * some; otherwise do nothing (for performance reasons).
     */
    if (bitstream->MaxLength - bitstream->DataOffset - bitstream->DataLength <
        size)
    {
        memmove(bitstream->Data, bitstream->Data + bitstream->DataOffset,
                bitstream->DataLength);
        bitstream->DataOffset = 0;
    }

    memcpy(bitstream->Data + bitstream->DataOffset + bitstream->DataLength,
           data, size);
    bitstream->DataLength += size;

end:
    hb_buffer_close(&buf);
    return size;
}

static mfxFrameSurface1* get_unlocked_surface(mfxFrameSurface1 *surfaces,
                                              int nsurfaces)
{
    int i;

    for (i = 0; i < nsurfaces; i++)
    {
        if (!surfaces[i].Data.Locked)
        {
            return &surfaces[i];
        }
    }

    return NULL;
}

static int buffer_fill_from_surface(hb_work_private_t *pv,
                                    mfxFrameSurface1 *surface, hb_buffer_t *buf)
{
    AVPicture pic_in, pic_cropped, pic_out;

    /*
     * Fill the output buffer and prepare input for color space conversion.
     */
    if (buf->size != hb_avpicture_fill(&pic_out, buf))
    {
        hb_log("decqsv: hb_avpicture_fill failed");
        return -1;
    }
    if (av_image_fill_linesizes(pic_in.linesize, AV_PIX_FMT_NV12,
                                surface->Info.Width) < 0)
    {
        hb_log("decqsv: av_image_fill_linesizes failed");
        return -1;
    }
    if (av_image_fill_pointers(pic_in.data, AV_PIX_FMT_NV12,
                               surface->Info.Height, surface->Data.Y,
                               pic_in.linesize) != pv->surface_size)
    {
        hb_log("decqsv: av_image_fill_pointers failed");
        return -1;
    }

    /*
     * The actual image may not start at (0, 0) in the decoded output, but our
     * buffers don't allow for "soft" cropping (unlike MFX surfaces), so we have
     * to crop before colorspace conversion happens (our output buffer is too
     * small to hold the uncropped source frame if the offset is non-zero).
     *
     * av_picture_crop just alters the data pointer,
     * there is no need for an intermediate buffer.
     */
    if (av_picture_crop(&pic_cropped, &pic_in, AV_PIX_FMT_NV12,
                        surface->Info.CropX, surface->Info.CropY) < 0)
    {
        hb_log("decqsv: av_picture_crop failed");
        return -1;
    }
    if (sws_scale(pv->sws_context, (const uint8_t* const*)pic_cropped.data,
                  pic_cropped.linesize, 0, pv->job->title->height, pic_out.data,
                  pic_out.linesize) <= 0)
    {
        hb_log("decqsv: sws_scale failed");
        return -1;
    }

    return 0;
}

static void buffer_set_properties(hb_work_private_t *pv,
                                  mfxFrameSurface1 *surface, hb_buffer_t *buf)
{
    int64_t duration = pv->default_duration;

    /*
     * PicStruct flags. HB's flags can only represent a subset of what Media SDK can
     * indicate, especially if we request it via mfxVideoParam.mfx.ExtendedPicStruct.
     *
     * Let's leave it unset and keep things simple for now.
     */
    if (pv->videoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE)
    {
        buf->s.flags = PIC_FLAG_PROGRESSIVE_FRAME;
    }
    if (pv->videoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_FIELD_TFF)
    {
        buf->s.flags = PIC_FLAG_TOP_FIELD_FIRST;
    }
    if (pv->videoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED)
    {
        buf->s.flags |= PIC_FLAG_REPEAT_FIRST_FIELD;
        duration     += duration / 2;
    }
    if (pv->videoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING)
    {
        buf->s.flags |= PIC_FLAG_REPEAT_FRAME;
        duration     *= 2;
    }
    if (pv->videoParam.mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING)
    {
        buf->s.flags |= PIC_FLAG_REPEAT_FRAME;
        duration     *= 3;
    }

    if (surface->Data.TimeStamp == AV_NOPTS_VALUE)
    {
        /*
         * If there was no  PTS for this frame, assume video is CFR
         * and use the next PTS guesstimate as the frame's start time.
         */
        hb_deep_log(2, "decqsv: no input PTS");//debug
        buf->s.start = pv->next_pts;
    }
    else
    {
        buf->s.start = surface->Data.TimeStamp;
    }
    pv->next_pts = buf->s.start + duration;
}

static int bitstream_decode(hb_work_private_t *pv, mfxBitstream *bitstream)
{
    mfxStatus status;
    hb_buffer_t *out;
    mfxSyncPoint syncpoint = (mfxSyncPoint)0;
    mfxFrameSurface1 *surface_work, *surface_out = NULL;

    do
    {
        surface_work = get_unlocked_surface(pv->surfaces, pv->nsurfaces);
        if (surface_work == NULL)
        {
            hb_log("decqsv: no more frame surfaces available");
            return -1;
        }

        status = MFXVideoDECODE_DecodeFrameAsync(pv->session,
                                                 bitstream,
                                                 surface_work,
                                                 &surface_out,
                                                 &syncpoint);

        switch (status)
        {
            case MFX_ERR_MORE_DATA:
                return BITSTREAM_DECODE_DONE;

            case MFX_ERR_MORE_SURFACE:
                continue;

            case MFX_WRN_DEVICE_BUSY:
                av_usleep(1000);
                continue;

            default:
                break;
        }

        if (status < MFX_ERR_NONE)
        {
            hb_log("decqsv: MFXVideoDECODE_DecodeFrameAsync failed (%d)", status);
            return -1;
        }

        /* If video parameters have changed, we need to update PicStruct info */
        if (status == MFX_WRN_VIDEO_PARAM_CHANGED)
        {
            mfxVideoParam videoParam;

            memset(&videoParam, 0, sizeof(mfxVideoParam));

            if (MFXVideoDECODE_GetVideoParam(pv->session,
                                             &videoParam) == MFX_ERR_NONE)
            {
                memcpy(&pv->videoParam, &videoParam, sizeof(mfxVideoParam));
            }
        }

        if (syncpoint)
        {
            if (pv->videoParam.IOPattern == MFX_IOPATTERN_OUT_SYSTEM_MEMORY)
            {
                do
                {
                    status = MFXVideoCORE_SyncOperation(pv->session, syncpoint, 100);
                }
                while (status == MFX_WRN_IN_EXECUTION);

                if (status != MFX_ERR_NONE)
                {
                    hb_log("decqsv: MFXVideoCORE_SyncOperation failed (%d)", status);
                    return -1;
                }

                break; // we have a decoded frame, copy it
            }
        }
    }
    while (1);

    /*
     * We quit the loop without returning, which means a frame is available.
     */
    out = hb_video_buffer_init(pv->job->title->width, pv->job->title->height);
    if (out == NULL)
    {
        hb_log("decqsv: failed to create output frame buffer");
        return -1;
    }
    if (buffer_fill_from_surface(pv, surface_out, out) < 0)
    {
        hb_log("decqsv: failed to fill output frame buffer");
        return -1;
    }
    buffer_set_properties(pv, surface_out, out);

    hb_list_add(pv->decoded_frames, out);
    return 0;
}

static hb_buffer_t* link_buffer_list(hb_list_t *list)
{
    hb_buffer_t *buf, *out = NULL, *prev = NULL;

    while ((buf = hb_list_item(list, 0)) != NULL)
    {
        hb_list_rem(list, buf);

        if (prev == NULL)
        {
            prev = out = buf;
        }
        else
        {
            prev->next = buf;
            prev       = buf;
        }
    }

    return out;
}

static int decqsvWork(hb_work_object_t *w,
                      hb_buffer_t **buf_in, hb_buffer_t **buf_out)
{
    hb_work_private_t *pv = w->private_data;
    hb_buffer_t       *in = *buf_in;
    *buf_in               = NULL;
    int ret, err = HB_ERROR_NONE;

    if (*pv->job->die)
    {//fixme: still crashes
        err = HB_ERROR_UNKNOWN;
        goto error;
    }

    if (!pv->initialized)
    {
        if (in->size <= 0)
        {
            /* we can't start flushing before we're done initializing */
            hb_error("decqsvWork: EOF found before we could initialize");
            *buf_out = in;
            return HB_WORK_DONE;
        }

        ret = qsv_decode_init(pv, in->data, in->size);

        if (ret < 0)
        {
            err = HB_ERROR_INIT;
            goto error;
        }

        if (ret == HEADER_DATA_NOT_FOUND)
        {
            /*
             * we didn't find any header data, discard this
             * frame but keep decoding until we get a header
             */
            *buf_out = NULL;
            hb_buffer_close(&in);
            return HB_WORK_OK;
        }

        pv->initialized = 1;
    }

    if (in->size <= 0)
    {
        /* EOF on input - flush, send it downstream & say that we're done */
        do
        {
            ret = bitstream_decode(pv, NULL);
        }
        while (ret >= 0 && ret != BITSTREAM_DECODE_DONE);

        hb_list_add(pv->decoded_frames, in);
        *buf_out = link_buffer_list(pv->decoded_frames);
        return HB_WORK_DONE;
    }

    if (bitstream_append_data(pv, in) < 0)
    {
        err = HB_ERROR_UNKNOWN;
        goto error;
    }

    do
    {
        ret = bitstream_decode(pv, &pv->bitstream);
    }
    while (ret >= 0 && ret != BITSTREAM_DECODE_DONE);

    if (ret != BITSTREAM_DECODE_DONE)
    {
        err = HB_ERROR_UNKNOWN;
        goto error;
    }

    *buf_out = link_buffer_list(pv->decoded_frames);
    hb_buffer_close(&in);
    return HB_WORK_OK;

error:
    *buf_out = NULL;
    *pv->job->die = 1;
    *pv->job->done_error = err;
    hb_buffer_close(&in);
    return HB_WORK_ERROR;
}

#endif // USE_QSV
