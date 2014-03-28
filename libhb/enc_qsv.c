/* enc_qsv.c
 *
 * Copyright (c) 2013-2014 Intel Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * - Neither the name of Intel Corporation nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY INTEL CORPORATION "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL INTEL CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef USE_QSV

#include "libavutil/time.h"

#include "hb.h"
#include "nal_units.h"
#include "qsv_common.h"
#include "qsv_memory.h"

int  encqsvInit (hb_work_object_t*, hb_job_t*);
int  encqsvWork (hb_work_object_t*, hb_buffer_t**, hb_buffer_t**);
void encqsvClose(hb_work_object_t*);

hb_work_object_t hb_encqsv =
{
    WORK_ENCQSV,
    "Quick Sync Video encoder (Intel Media SDK)",
    encqsvInit,
    encqsvWork,
    encqsvClose
};

struct hb_work_private_s
{
    hb_job_t *job;
    uint32_t  frames_in;
    uint32_t  frames_out;
    int64_t   last_start;

    hb_qsv_param_t param;
    av_qsv_space enc_space;
    hb_qsv_info_t *qsv_info;

    mfxEncodeCtrl force_keyframe;
    hb_list_t *delayed_chapters;
    int64_t next_chapter_pts;

    int64_t default_duration;

#define BFRM_DELAY_MAX 16
    // for DTS generation (when MSDK API < 1.6 or VFR)
    int            bfrm_delay;
    int            bfrm_workaround;
    int64_t        init_pts[BFRM_DELAY_MAX + 1];
    hb_list_t     *list_dts;

    int async_depth;
    int max_async_depth;

    // if encode-only, system memory used
    int is_sys_mem;
    mfxSession mfx_session;
    struct SwsContext *sws_context_to_nv12;

    // whether to expect input from VPP or from QSV decode
    int is_vpp_present;

    // whether the encoder is initialized
    int init_done;

    hb_list_t *delayed_processing;
    hb_list_t *encoded_frames;
};

// used in delayed_chapters list
struct chapter_s
{
    int     index;
    int64_t start;
};

// for DTS generation (when MSDK API < 1.6 or VFR)
static void hb_qsv_add_new_dts(hb_list_t *list, int64_t new_dts)
{
    if (list != NULL)
    {
        int64_t *item = malloc(sizeof(int64_t));
        if (item != NULL)
        {
            *item = new_dts;
            hb_list_add(list, item);
        }
    }
}
static int64_t hb_qsv_pop_next_dts(hb_list_t *list)
{
    int64_t next_dts = INT64_MIN;
    if (list != NULL && hb_list_count(list) > 0)
    {
        int64_t *item = hb_list_item(list, 0);
        if (item != NULL)
        {
            next_dts = *item;
            hb_list_rem(list, item);
            free(item);
        }
    }
    return next_dts;
}

static int qsv_hevc_make_header(hb_work_object_t *w, mfxSession session)
{
    size_t len;
    int ret = 0;
    uint8_t *buf, *end;
    mfxBitstream bitstream;
    hb_buffer_t *bitstream_buf;
    mfxStatus status;
    mfxSyncPoint syncPoint;
    mfxFrameSurface1 frameSurface1;
    hb_work_private_t *pv = w->private_data;

    memset(&bitstream,     0, sizeof(mfxBitstream));
    memset(&syncPoint,     0, sizeof(mfxSyncPoint));
    memset(&frameSurface1, 0, sizeof(mfxFrameSurface1));

    /* The bitstream buffer should be able to hold any encoded frame */
    bitstream_buf = hb_video_buffer_init(pv->job->width, pv->job->height);
    if (bitstream_buf == NULL)
    {
        hb_log("qsv_hevc_make_header: hb_buffer_init failed");
        ret = -1;
        goto end;
    }
    bitstream.Data      = bitstream_buf->data;
    bitstream.MaxLength = bitstream_buf->size;

    /* We only need to encode one frame, so we only need one surface */
    mfxU16 Height            = pv->param.videoParam->mfx.FrameInfo.Height;
    mfxU16 Width             = pv->param.videoParam->mfx.FrameInfo.Width;
    frameSurface1.Info       = pv->param.videoParam->mfx.FrameInfo;
    frameSurface1.Data.VU    = av_mallocz(Width * Height / 2);
    frameSurface1.Data.Y     = av_mallocz(Width * Height);
    frameSurface1.Data.Pitch = Width;

    /* Encode our only frame */
    do
    {
        status = MFXVideoENCODE_EncodeFrameAsync(session, NULL, &frameSurface1,
                                                 &bitstream, &syncPoint);

        if (status == MFX_WRN_DEVICE_BUSY)
        {
            av_usleep(1000);
        }
    }
    while (status == MFX_WRN_DEVICE_BUSY);

    if (status < MFX_ERR_NONE && status != MFX_ERR_MORE_DATA)
    {
        hb_log("qsv_hevc_make_header: MFXVideoENCODE_EncodeFrameAsync failed (%d)", status);
        ret = -1;
        goto end;
    }

    /* We may already have some output */
    if (syncPoint)
    {
        do
        {
            status = MFXVideoCORE_SyncOperation(session, syncPoint, 100);
        }
        while (status == MFX_WRN_IN_EXECUTION);

        if (status != MFX_ERR_NONE)
        {
            hb_log("qsv_hevc_make_header: MFXVideoCORE_SyncOperation failed (%d)", status);
            ret = -1;
            goto end;
        }
    }

    /*
     * If there is an encoding delay (because of e.g. lookahead),
     * we may need to flush the encoder to get the output frame.
     */
    do
    {
        status = MFXVideoENCODE_EncodeFrameAsync(session, NULL, NULL,
                                                 &bitstream, &syncPoint);

        if (status == MFX_WRN_DEVICE_BUSY)
        {
            av_usleep(1000);
        }
    }
    while (status >= MFX_ERR_NONE);

    if (status != MFX_ERR_MORE_DATA)
    {
        hb_log("qsv_hevc_make_header: MFXVideoENCODE_EncodeFrameAsync failed (%d)", status);
        ret = -1;
        goto end;
    }

    /* If we didn't have any output before, now we should */
    if (syncPoint)
    {
        do
        {
            status = MFXVideoCORE_SyncOperation(session, syncPoint, 100);
        }
        while (status == MFX_WRN_IN_EXECUTION);

        if (status != MFX_ERR_NONE)
        {
            hb_log("qsv_hevc_make_header: MFXVideoCORE_SyncOperation failed (%d)", status);
            ret = -1;
            goto end;
        }
    }

    if (!bitstream.DataLength)
    {
        hb_log("qsv_hevc_make_header: no output data found");
        ret = -1;
        goto end;
    }

    /* Include any parameter sets and SEI NAL units in the headers. */
    len = bitstream.DataLength;
    buf = bitstream.Data + bitstream.DataOffset;
    end = bitstream.Data + bitstream.DataOffset + bitstream.DataLength;
    w->config->h265.headers_length = 0;

    while ((buf = hb_annexb_find_next_nalu(buf, &len)) != NULL)
    {
        switch ((buf[0] >> 1) & 0x3f)
        {
            case 32: // VPS_NUT
            case 33: // SPS_NUT
            case 34: // PPS_NUT
            case 39: // PREFIX_SEI_NUT
            case 40: // SUFFIX_SEI_NUT
                break;
            default:
                len = end - buf;
                continue;
        }

        size_t size = hb_nal_unit_write_annexb(NULL, buf, len) + w->config->h265.headers_length;
        if (sizeof(w->config->h265.headers) < size)
        {
            /* Will never happen in practice */
            hb_log("qsv_hevc_make_header: header too large (size: %lu, max: %lu)",
                   size, sizeof(w->config->h265.headers));
        }

        w->config->h265.headers_length += hb_nal_unit_write_annexb(w->config->h265.headers +
                                                                   w->config->h265.headers_length, buf, len);
        len = end - buf;
    }

end:
    hb_buffer_close(&bitstream_buf);
    av_free(frameSurface1.Data.VU);
    av_free(frameSurface1.Data.Y);
    return ret;
}

int qsv_enc_init(av_qsv_context *qsv, hb_work_private_t *pv)
{
    int i = 0;
    mfxIMPL impl;
    mfxStatus sts;
    mfxVersion version;
    hb_job_t *job = pv->job;

    if (pv->init_done)
    {
        return 0;
    }

    if (qsv == NULL)
    {
        if (!pv->is_sys_mem)
        {
            hb_error("qsv_enc_init: decode enabled but no context!");
            return 3;
        }
        job->qsv.ctx = qsv = av_mallocz(sizeof(av_qsv_context));
    }

    av_qsv_space *qsv_encode = qsv->enc_space;
    if (qsv_encode == NULL)
    {
        // if only for encode
        if (pv->is_sys_mem)
        {
            // no need to use additional sync as encode only -> single thread
            av_qsv_add_context_usage(qsv, 0);

            // re-use the session from encqsvInit
            qsv->mfx_session = pv->mfx_session;
        }
        qsv->enc_space = qsv_encode = &pv->enc_space;
    }

    /* We need the actual API version for hb_qsv_plugin_load */
    if ((MFXQueryIMPL   (qsv->mfx_session, &impl)    == MFX_ERR_NONE) &&
        (MFXQueryVersion(qsv->mfx_session, &version) == MFX_ERR_NONE))
    {
        /* log actual implementation details now that we know them */
        hb_log("qsv_enc_init: using '%s' implementation, API: %"PRIu16".%"PRIu16"",
               hb_qsv_impl_get_name(impl), version.Major, version.Minor);
    }
    else
    {
        hb_error("qsv_enc_init: MFXQueryIMPL/MFXQueryVersion failure");
        *job->done_error = HB_ERROR_INIT;
        *job->die = 1;
        return -1;
    }

    /*
     * Load optional codec plug-ins.
     *
     * Note: in the encode-only path, hb_qsv_plugin_load was already called.
     */
    if (!pv->is_sys_mem)
    {
        sts = hb_qsv_plugin_load(qsv->mfx_session, version, pv->qsv_info->codec_id);
        if (sts < MFX_ERR_NONE)
        {
            hb_error("qsv_enc_init: hb_qsv_plugin_load failed (%d)", sts);
            *job->done_error = HB_ERROR_INIT;
            *job->die = 1;
            return -1;
        }
    }

    if (!pv->is_sys_mem)
    {
        if (!pv->is_vpp_present && job->list_filter != NULL)
        {
            for (i = 0; i < hb_list_count(job->list_filter); i++)
            {
                hb_filter_object_t *filter = hb_list_item(job->list_filter, i);
                if (filter->id == HB_FILTER_QSV_PRE  ||
                    filter->id == HB_FILTER_QSV_POST ||
                    filter->id == HB_FILTER_QSV)
                {
                    pv->is_vpp_present = 1;
                    break;
                }
            }
        }

        if (pv->is_vpp_present)
        {
            if (qsv->vpp_space == NULL)
            {
                return 2;
            }
            for (i = 0; i < av_qsv_list_count(qsv->vpp_space); i++)
            {
                av_qsv_space *vpp = av_qsv_list_item(qsv->vpp_space, i);
                if (!vpp->is_init_done)
                {
                    return 2;
                }
            }
        }

        av_qsv_space *dec_space = qsv->dec_space;
        if (dec_space == NULL || !dec_space->is_init_done)
        {
            return 2;
        }
    }
    else
    {
        pv->sws_context_to_nv12 = hb_sws_get_context(job->width, job->height,
                                                     AV_PIX_FMT_YUV420P,
                                                     job->width, job->height,
                                                     AV_PIX_FMT_NV12,
                                                     SWS_LANCZOS|SWS_ACCURATE_RND);
    }

    // allocate tasks
    qsv_encode->p_buf_max_size = AV_QSV_BUF_SIZE_DEFAULT;
    qsv_encode->tasks          = av_qsv_list_init(HAVE_THREADS);
    for (i = 0; i < pv->max_async_depth; i++)
    {
        av_qsv_task *task    = av_mallocz(sizeof(av_qsv_task));
        task->bs             = av_mallocz(sizeof(mfxBitstream));
        task->bs->Data       = av_mallocz(sizeof(uint8_t) * qsv_encode->p_buf_max_size);
        task->bs->MaxLength  = qsv_encode->p_buf_max_size;
        task->bs->DataLength = 0;
        task->bs->DataOffset = 0;
        av_qsv_list_add(qsv_encode->tasks, task);
    }

    // setup surface allocation
    qsv_encode->m_mfxVideoParam.IOPattern = (pv->is_sys_mem                 ?
                                             MFX_IOPATTERN_IN_SYSTEM_MEMORY :
                                             MFX_IOPATTERN_IN_OPAQUE_MEMORY);

    memset(&qsv_encode->request, 0, sizeof(mfxFrameAllocRequest) * 2);

    sts = MFXVideoENCODE_QueryIOSurf(qsv->mfx_session,
                                     &qsv_encode->m_mfxVideoParam,
                                     &qsv_encode->request[0]);
    if (sts < MFX_ERR_NONE) // ignore warnings
    {
        hb_error("qsv_enc_init: MFXVideoENCODE_QueryIOSurf failed (%d)", sts);
        *job->done_error = HB_ERROR_INIT;
        *job->die = 1;
        return -1;
    }

    // allocate surfaces
    if (pv->is_sys_mem)
    {
        qsv_encode->surface_num = FFMIN(qsv_encode->request[0].NumFrameSuggested +
                                        pv->max_async_depth, AV_QSV_SURFACE_NUM);
        if (qsv_encode->surface_num <= 0)
        {
            qsv_encode->surface_num = AV_QSV_SURFACE_NUM;
        }
        for (i = 0; i < qsv_encode->surface_num; i++)
        {
            mfxFrameSurface1 *surface = av_mallocz(sizeof(mfxFrameSurface1));
            if (surface == NULL)
            {
                hb_error("qsv_enc_init: av_mallocz failed");
                *job->done_error = HB_ERROR_INIT;
                *job->die = 1;
                return -1;
            }

            qsv_encode->p_surfaces[i] = surface;
            surface->Info             = qsv_encode->request[0].Info;
            mfxU16 Width              = qsv_encode->request[0].Info.Width;
            mfxU16 Height             = qsv_encode->request[0].Info.Height;
            surface->Data.Y           = calloc(1, Width * Height);
            surface->Data.VU          = calloc(1, Width * Height / 2);
            surface->Data.Pitch       = Width;
        }
    }
    else
    {
        av_qsv_space *in_space = qsv->dec_space;
        if (pv->is_vpp_present)
        {
            // we get our input from VPP instead
            in_space = av_qsv_list_item(qsv->vpp_space,
                                        av_qsv_list_count(qsv->vpp_space) - 1);
        }
        // introduced in API 1.3
        memset(&qsv_encode->ext_opaque_alloc, 0, sizeof(mfxExtOpaqueSurfaceAlloc));
        qsv_encode->ext_opaque_alloc.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
        qsv_encode->ext_opaque_alloc.Header.BufferSz = sizeof(mfxExtOpaqueSurfaceAlloc);
        qsv_encode->ext_opaque_alloc.In.Surfaces     = in_space->p_surfaces;
        qsv_encode->ext_opaque_alloc.In.NumSurface   = in_space->surface_num;
        qsv_encode->ext_opaque_alloc.In.Type         = qsv_encode->request[0].Type;
        qsv_encode->m_mfxVideoParam.ExtParam[qsv_encode->m_mfxVideoParam.NumExtParam++] = (mfxExtBuffer*)&qsv_encode->ext_opaque_alloc;
    }

    // allocate sync points
    qsv_encode->sync_num = (qsv_encode->surface_num                         ?
                            FFMIN(qsv_encode->surface_num, AV_QSV_SYNC_NUM) :
                            AV_QSV_SYNC_NUM);
    for (i = 0; i < qsv_encode->sync_num; i++)
    {
        qsv_encode->p_syncp[i] = av_mallocz(sizeof(av_qsv_sync));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i], MFX_ERR_MEMORY_ALLOC);
        qsv_encode->p_syncp[i]->p_sync = av_mallocz(sizeof(mfxSyncPoint));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i]->p_sync, MFX_ERR_MEMORY_ALLOC);
    }

    // initialize the encoder
    sts = MFXVideoENCODE_Init(qsv->mfx_session, &qsv_encode->m_mfxVideoParam);
    if (sts < MFX_ERR_NONE) // ignore warnings
    {
        hb_error("qsv_enc_init: MFXVideoENCODE_Init failed (%d)", sts);
        *job->done_error = HB_ERROR_INIT;
        *job->die = 1;
        return -1;
    }
    qsv_encode->is_init_done = 1;

    pv->init_done = 1;
    return 0;
}

/***********************************************************************
 * encqsvInit
 ***********************************************************************
 *
 **********************************************************************/
int encqsvInit(hb_work_object_t *w, hb_job_t *job)
{
    hb_work_private_t *pv = calloc(1, sizeof(hb_work_private_t));
    w->private_data       = pv;

    pv->job                = job;
    pv->is_sys_mem         = !hb_qsv_decode_is_enabled(job);
    pv->qsv_info           = hb_qsv_info_get(job->vcodec);
    pv->delayed_processing = hb_list_init();
    pv->encoded_frames     = hb_list_init();
    pv->last_start         = INT64_MIN;
    pv->frames_in          = 0;
    pv->frames_out         = 0;
    pv->init_done          = 0;
    pv->is_vpp_present     = 0;

    // set up a re-usable mfxEncodeCtrl to force keyframes (e.g. for chapters)
    pv->force_keyframe.QP          = 0;
    pv->force_keyframe.FrameType   = MFX_FRAMETYPE_I|MFX_FRAMETYPE_IDR|MFX_FRAMETYPE_REF;
    pv->force_keyframe.NumExtParam = 0;
    pv->force_keyframe.NumPayload  = 0;
    pv->force_keyframe.ExtParam    = NULL;
    pv->force_keyframe.Payload     = NULL;

    pv->next_chapter_pts = AV_NOPTS_VALUE;
    pv->delayed_chapters = hb_list_init();

    // default frame duration in ticks (based on average frame rate)
    pv->default_duration = (double)job->vrate_base / (double)job->vrate * 90000.;

    // default encoding parameters
    if (hb_qsv_param_default_preset(&pv->param, &pv->enc_space.m_mfxVideoParam,
                                     pv->qsv_info, job->encoder_preset))
    {
        hb_error("encqsvInit: hb_qsv_param_default_preset failed");
        return -1;
    }

    // set AsyncDepth to match that of decode and VPP
    pv->param.videoParam->AsyncDepth = job->qsv.async_depth;

    // enable and set colorimetry (video signal information)
    switch (job->color_matrix_code)
    {
        case 4:
            // custom
            pv->param.videoSignalInfo.ColourPrimaries         = job->color_prim;
            pv->param.videoSignalInfo.TransferCharacteristics = job->color_transfer;
            pv->param.videoSignalInfo.MatrixCoefficients      = job->color_matrix;
            break;
        case 3:
            // ITU BT.709 HD content
            pv->param.videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_BT709;
            pv->param.videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
            pv->param.videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_BT709;
            break;
        case 2:
            // ITU BT.601 DVD or SD TV content (PAL)
            pv->param.videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_EBUTECH;
            pv->param.videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
            pv->param.videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_SMPTE170M;
            break;
        case 1:
            // ITU BT.601 DVD or SD TV content (NTSC)
            pv->param.videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_SMPTEC;
            pv->param.videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
            pv->param.videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_SMPTE170M;
            break;
        default:
            // detected during scan
            pv->param.videoSignalInfo.ColourPrimaries         = job->title->color_prim;
            pv->param.videoSignalInfo.TransferCharacteristics = job->title->color_transfer;
            pv->param.videoSignalInfo.MatrixCoefficients      = job->title->color_matrix;
            break;
    }
    pv->param.videoSignalInfo.ColourDescriptionPresent = 1;

    // parse user-specified encoder options, if present
    if (job->encoder_options != NULL && *job->encoder_options)
    {
        hb_dict_t *options_list;
        hb_dict_entry_t *option = NULL;
        options_list = hb_encopts_to_dict(job->encoder_options, job->vcodec);
        while ((option = hb_dict_next(options_list, option)) != NULL)
        {
            switch (hb_qsv_param_parse(&pv->param,  pv->qsv_info,
                                       option->key, option->value))
            {
                case HB_QSV_PARAM_OK:
                    break;

                case HB_QSV_PARAM_BAD_NAME:
                    hb_log("encqsvInit: hb_qsv_param_parse: bad key %s",
                           option->key);
                    break;
                case HB_QSV_PARAM_BAD_VALUE:
                    hb_log("encqsvInit: hb_qsv_param_parse: bad value %s for key %s",
                           option->value, option->key);
                    break;
                case HB_QSV_PARAM_UNSUPPORTED:
                    hb_log("encqsvInit: hb_qsv_param_parse: unsupported option %s",
                           option->key);
                    break;

                case HB_QSV_PARAM_ERROR:
                default:
                    hb_log("encqsvInit: hb_qsv_param_parse: unknown error");
                    break;
            }
        }
        hb_dict_free(&options_list);
    }

    // reload colorimetry in case values were set in encoder_options
    if (pv->param.videoSignalInfo.ColourDescriptionPresent)
    {
        job->color_matrix_code = 4;
        job->color_prim        = pv->param.videoSignalInfo.ColourPrimaries;
        job->color_transfer    = pv->param.videoSignalInfo.TransferCharacteristics;
        job->color_matrix      = pv->param.videoSignalInfo.MatrixCoefficients;
    }
    else
    {
        job->color_matrix_code = 0;
        job->color_prim        = HB_COLR_PRI_UNDEF;
        job->color_transfer    = HB_COLR_TRA_UNDEF;
        job->color_matrix      = HB_COLR_MAT_UNDEF;
    }

    // sanitize values that may exceed the Media SDK variable size
    int64_t vrate, vrate_base;
    int64_t par_width, par_height;
    hb_limit_rational64(&vrate, &vrate_base,
                        job->vrate, job->vrate_base, UINT32_MAX);
    hb_limit_rational64(&par_width, &par_height,
                        job->anamorphic.par_width,
                        job->anamorphic.par_height, UINT16_MAX);

    // some encoding parameters are used by filters to configure their output
    if (pv->param.videoParam->mfx.FrameInfo.PicStruct != MFX_PICSTRUCT_PROGRESSIVE)
    {
        job->qsv.enc_info.align_height = AV_QSV_ALIGN32(job->height);
    }
    else
    {
        job->qsv.enc_info.align_height = AV_QSV_ALIGN16(job->height);
    }
    job->qsv.enc_info.align_width  = AV_QSV_ALIGN16(job->width);
    job->qsv.enc_info.pic_struct   = pv->param.videoParam->mfx.FrameInfo.PicStruct;
    job->qsv.enc_info.is_init_done = 1;

    // set codec, profile/level and FrameInfo
    pv->param.videoParam->mfx.CodecId                 = pv->qsv_info->codec_id;
    pv->param.videoParam->mfx.CodecLevel              = MFX_LEVEL_UNKNOWN;
    pv->param.videoParam->mfx.CodecProfile            = MFX_PROFILE_UNKNOWN;
    pv->param.videoParam->mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    pv->param.videoParam->mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    pv->param.videoParam->mfx.FrameInfo.FrameRateExtN = vrate;
    pv->param.videoParam->mfx.FrameInfo.FrameRateExtD = vrate_base;
    pv->param.videoParam->mfx.FrameInfo.AspectRatioW  = par_width;
    pv->param.videoParam->mfx.FrameInfo.AspectRatioH  = par_height;
    pv->param.videoParam->mfx.FrameInfo.CropX         = 0;
    pv->param.videoParam->mfx.FrameInfo.CropY         = 0;
    pv->param.videoParam->mfx.FrameInfo.CropW         = job->width;
    pv->param.videoParam->mfx.FrameInfo.CropH         = job->height;
    pv->param.videoParam->mfx.FrameInfo.PicStruct     = job->qsv.enc_info.pic_struct;
    pv->param.videoParam->mfx.FrameInfo.Width         = job->qsv.enc_info.align_width;
    pv->param.videoParam->mfx.FrameInfo.Height        = job->qsv.enc_info.align_height;

    // set encoder profile and level
    if (hb_qsv_profile_parse(&pv->param, pv->qsv_info, job->encoder_profile) < 0)
    {
        hb_error("encqsvInit: bad profile %s", job->encoder_profile);
        return -1;
    }
    if (hb_qsv_level_parse(&pv->param, pv->qsv_info, job->encoder_level) < 0)
    {
        hb_error("encqsvInit: bad level %s", job->encoder_level);
        return -1;
    }

    // interlaced encoding is not always possible
    if (pv->param.videoParam->mfx.CodecId             == MFX_CODEC_AVC &&
        pv->param.videoParam->mfx.FrameInfo.PicStruct != MFX_PICSTRUCT_PROGRESSIVE)
    {
        if (pv->param.videoParam->mfx.CodecProfile == MFX_PROFILE_AVC_CONSTRAINED_BASELINE ||
            pv->param.videoParam->mfx.CodecProfile == MFX_PROFILE_AVC_BASELINE             ||
            pv->param.videoParam->mfx.CodecProfile == MFX_PROFILE_AVC_PROGRESSIVE_HIGH)
        {
            hb_error("encqsvInit: profile %s doesn't support interlaced encoding",
                     hb_qsv_profile_name(MFX_CODEC_AVC,
                                         pv->param.videoParam->mfx.CodecProfile));
            return -1;
        }
        if ((pv->param.videoParam->mfx.CodecLevel >= MFX_LEVEL_AVC_1b &&
             pv->param.videoParam->mfx.CodecLevel <= MFX_LEVEL_AVC_2) ||
            (pv->param.videoParam->mfx.CodecLevel >= MFX_LEVEL_AVC_42))
        {
            hb_error("encqsvInit: level %s doesn't support interlaced encoding",
                     hb_qsv_level_name(MFX_CODEC_AVC,
                                       pv->param.videoParam->mfx.CodecLevel));
            return -1;
        }
    }

    // sanitize ICQ
    if (!(pv->qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_ICQ))
    {
        // ICQ not supported
        pv->param.rc.icq = 0;
    }
    else
    {
        pv->param.rc.icq = !!pv->param.rc.icq;
    }

    // sanitize lookahead
    if (!(pv->qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_LA))
    {
        // lookahead not supported
        pv->param.rc.lookahead = 0;
    }
    else if ((pv->param.rc.lookahead)                                       &&
             (pv->qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_LAi) == 0 &&
             (pv->param.videoParam->mfx.FrameInfo.PicStruct != MFX_PICSTRUCT_PROGRESSIVE))
    {
        // lookahead enabled but we can't use it
        hb_log("encqsvInit: LookAhead not used (LookAhead is progressive-only)");
        pv->param.rc.lookahead = 0;
    }
    else
    {
        pv->param.rc.lookahead = !!pv->param.rc.lookahead;
    }

    // set VBV here (this will be overridden for CQP and ignored for LA)
    // only set BufferSizeInKB, InitialDelayInKB and MaxKbps if we have
    // them - otheriwse Media SDK will pick values for us automatically
    if (pv->param.rc.vbv_buffer_size > 0)
    {
        if (pv->param.rc.vbv_buffer_init > 1.0)
        {
            pv->param.videoParam->mfx.InitialDelayInKB = (pv->param.rc.vbv_buffer_init / 8);
        }
        else if (pv->param.rc.vbv_buffer_init > 0.0)
        {
            pv->param.videoParam->mfx.InitialDelayInKB = (pv->param.rc.vbv_buffer_size *
                                                          pv->param.rc.vbv_buffer_init / 8);
        }
        pv->param.videoParam->mfx.BufferSizeInKB = (pv->param.rc.vbv_buffer_size / 8);
    }
    if (pv->param.rc.vbv_max_bitrate > 0)
    {
        pv->param.videoParam->mfx.MaxKbps = pv->param.rc.vbv_max_bitrate;
    }

    // set rate control paremeters
    if (job->vquality >= 0)
    {
        if (pv->param.rc.icq)
        {
            // introduced in API 1.8
            if (pv->param.rc.lookahead)
            {
                pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_LA_ICQ;
            }
            else
            {
                pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_ICQ;
            }
            pv->param.videoParam->mfx.ICQQuality = HB_QSV_CLIP3(1, 51, job->vquality);
        }
        else
        {
            // introduced in API 1.1
            pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_CQP;
            pv->param.videoParam->mfx.QPI = HB_QSV_CLIP3(0, 51, job->vquality + pv->param.rc.cqp_offsets[0]);
            pv->param.videoParam->mfx.QPP = HB_QSV_CLIP3(0, 51, job->vquality + pv->param.rc.cqp_offsets[1]);
            pv->param.videoParam->mfx.QPB = HB_QSV_CLIP3(0, 51, job->vquality + pv->param.rc.cqp_offsets[2]);
            // CQP + ExtBRC can cause bad output
            pv->param.codingOption2.ExtBRC = MFX_CODINGOPTION_OFF;
        }
    }
    else if (job->vbitrate > 0)
    {
        if (pv->param.rc.lookahead)
        {
            // introduced in API 1.7
            pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_LA;
            pv->param.videoParam->mfx.TargetKbps        = job->vbitrate;
            // ignored, but some drivers will change AsyncDepth because of it
            pv->param.codingOption2.ExtBRC = MFX_CODINGOPTION_OFF;
        }
        else
        {
            // introduced in API 1.0
            if (job->vbitrate == pv->param.rc.vbv_max_bitrate)
            {
                pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            }
            else
            {
                pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_VBR;
            }
            pv->param.videoParam->mfx.TargetKbps = job->vbitrate;
        }
    }
    else
    {
        hb_error("encqsvInit: invalid rate control (%d, %d)",
                 job->vquality, job->vbitrate);
        return -1;
    }

    // if VBV is enabled but ignored, log it
    if (pv->param.rc.vbv_max_bitrate > 0 || pv->param.rc.vbv_buffer_size > 0)
    {
        switch (pv->param.videoParam->mfx.RateControlMethod)
        {
            case MFX_RATECONTROL_LA:
            case MFX_RATECONTROL_LA_ICQ:
                hb_log("encqsvInit: LookAhead enabled, ignoring VBV");
                break;
            case MFX_RATECONTROL_ICQ:
                hb_log("encqsvInit: ICQ rate control, ignoring VBV");
                break;
            default:
                break;
        }
    }

    // set B-pyramid
    if (pv->param.gop.b_pyramid < 0)
    {
        if (pv->param.videoParam->mfx.RateControlMethod == MFX_RATECONTROL_CQP)
        {
            pv->param.gop.b_pyramid = 1;
        }
        else
        {
            pv->param.gop.b_pyramid = 0;
        }
    }
    pv->param.gop.b_pyramid = !!pv->param.gop.b_pyramid;

    // set the GOP structure
    if (pv->param.gop.gop_ref_dist < 0)
    {
        if (pv->param.videoParam->mfx.RateControlMethod == MFX_RATECONTROL_CQP)
        {
            pv->param.gop.gop_ref_dist = 4;
        }
        else
        {
            pv->param.gop.gop_ref_dist = 3;
        }
    }
    pv->param.videoParam->mfx.GopRefDist = pv->param.gop.gop_ref_dist;

    // set the keyframe interval
    if (pv->param.gop.gop_pic_size < 0)
    {
        int rate = (int)((double)job->vrate / (double)job->vrate_base + 0.5);
        if (pv->param.videoParam->mfx.RateControlMethod == MFX_RATECONTROL_CQP)
        {
            // ensure B-pyramid is enabled for CQP on Haswell
            pv->param.gop.gop_pic_size = 32;
        }
        else
        {
            // set the keyframe interval based on the framerate
            pv->param.gop.gop_pic_size = rate;
        }
    }
    pv->param.videoParam->mfx.GopPicSize = pv->param.gop.gop_pic_size;

    // sanitize some settings that affect memory consumption
    if (!pv->is_sys_mem)
    {
        // limit these to avoid running out of resources (causes hang)
        pv->param.videoParam->mfx.GopRefDist   = FFMIN(pv->param.videoParam->mfx.GopRefDist,
                                                       pv->param.rc.lookahead ? 8 : 16);
        pv->param.codingOption2.LookAheadDepth = FFMIN(pv->param.codingOption2.LookAheadDepth,
                                                       pv->param.rc.lookahead ? (48 - pv->param.videoParam->mfx.GopRefDist -
                                                                                 3 * !pv->param.videoParam->mfx.GopRefDist) : 0);
    }
    else
    {
        // encode-only is a bit less sensitive to memory issues
        pv->param.videoParam->mfx.GopRefDist   = FFMIN(pv->param.videoParam->mfx.GopRefDist, 16);
        pv->param.codingOption2.LookAheadDepth = FFMIN(pv->param.codingOption2.LookAheadDepth,
                                                       pv->param.rc.lookahead ? 60 : 0);
    }

    if ((pv->qsv_info->capabilities & HB_QSV_CAP_B_REF_PYRAMID) &&
        (pv->param.videoParam->mfx.CodecProfile != MFX_PROFILE_AVC_BASELINE         &&
         pv->param.videoParam->mfx.CodecProfile != MFX_PROFILE_AVC_CONSTRAINED_HIGH &&
         pv->param.videoParam->mfx.CodecProfile != MFX_PROFILE_AVC_CONSTRAINED_BASELINE))
    {
        int gop_ref_dist = 4;
        /*
         * B-pyramid is supported.
         *
         * Set gop_ref_dist to a power of two, >= 4 and <= GopRefDist to ensure
         * Media SDK will not disable B-pyramid if we end up using it below.
         */
        while (pv->param.videoParam->mfx.GopRefDist >= gop_ref_dist * 2)
        {
            gop_ref_dist *= 2;
        }
        /*
         * XXX: B-pyramid + forced keyframes will cause visual artifacts,
         *      force-disable B-pyramid until we insert keyframes properly
         */
        if (pv->param.gop.b_pyramid && job->chapter_markers)
        {
            pv->param.gop.b_pyramid = 0;
            hb_log("encqsvInit: chapter markers enabled, disabling B-pyramid "
                   "to work around a bug in our keyframe insertion code");
        }
        if ((pv->param.gop.b_pyramid) &&
            (pv->param.videoParam->mfx.GopPicSize == 0 ||
             pv->param.videoParam->mfx.GopPicSize > gop_ref_dist))
        {
            /*
             * B-pyramid enabled and GopPicSize is long enough for gop_ref_dist.
             *
             * Use gop_ref_dist. GopPicSize must be a multiple of GopRefDist.
             * NumRefFrame should be >= (GopRefDist / 2) and >= 3, otherwise
             * Media SDK may sometimes decide to disable B-pyramid too (whereas
             * sometimes it will just sanitize NumrefFrame instead).
             *
             * Notes: Media SDK handles the NumRefFrame == 0 case for us.
             *        Also, GopPicSize == 0 should always result in a value that
             *        does NOT cause Media SDK to disable B-pyramid, so it's OK.
             */
            pv->param.videoParam->mfx.GopRefDist = gop_ref_dist;
            pv->param.videoParam->mfx.GopPicSize = (pv->param.videoParam->mfx.GopPicSize /
                                                    pv->param.videoParam->mfx.GopRefDist *
                                                    pv->param.videoParam->mfx.GopRefDist);
            if (pv->param.videoParam->mfx.NumRefFrame != 0)
            {
                pv->param.videoParam->mfx.NumRefFrame = FFMAX(pv->param.videoParam->mfx.NumRefFrame,
                                                              pv->param.videoParam->mfx.GopRefDist / 2);
                pv->param.videoParam->mfx.NumRefFrame = FFMAX(pv->param.videoParam->mfx.NumRefFrame, 3);
            }
        }
        else
        {
            /*
             * B-pyramid disabled or not possible (GopPicSize too short).
             * Sanitize gop.b_pyramid to 0 (off/disabled).
             */
            pv->param.gop.b_pyramid = 0;
            /* Then, adjust settings to actually disable it. */
            if (pv->param.videoParam->mfx.GopRefDist == 0)
            {
                /*
                 * GopRefDist == 0 means the value will be set by Media SDK.
                 * Since we can't be sure what the actual value would be, we
                 * have to make sure that GopRefDist is set explicitly.
                 */
                pv->param.videoParam->mfx.GopRefDist = gop_ref_dist - 1;
            }
            else if (pv->param.videoParam->mfx.GopRefDist == gop_ref_dist)
            {
                /* GopRefDist is compatible with Media SDK's B-pyramid. */
                if (pv->param.videoParam->mfx.GopPicSize == 0)
                {
                    /*
                     * GopPicSize is unknown and could be a multiple of
                     * GopRefDist. Decrement the latter to disable B-pyramid.
                     */
                    pv->param.videoParam->mfx.GopRefDist--;
                }
                else if (pv->param.videoParam->mfx.GopPicSize %
                         pv->param.videoParam->mfx.GopRefDist == 0)
                {
                    /*
                     * GopPicSize is a multiple of GopRefDist.
                     * Increment the former to disable B-pyramid.
                     */
                    pv->param.videoParam->mfx.GopPicSize++;
                }
            }
        }
    }
    else
    {
        /* B-pyramid not supported. */
        pv->param.gop.b_pyramid = 0;
    }

    /*
     * Initialize a dummy encode-only session to get the parameter sets and the
     * final output settings sanitized by Media SDK; this is OK since the actual
     * encode will use the same values for all params relevant to the bitstream.
     */
    mfxStatus err;
    mfxVersion version;
    mfxVideoParam videoParam;
    mfxExtBuffer* ExtParamArray[3];
    mfxSession session = (mfxSession)0;
    mfxExtCodingOption  option1_buf, *option1 = &option1_buf;
    mfxExtCodingOption2 option2_buf, *option2 = &option2_buf;
    mfxExtCodingOptionSPSPPS sps_pps_buf, *sps_pps = &sps_pps_buf;
    version.Major = HB_QSV_MINVERSION_MAJOR;
    version.Minor = HB_QSV_MINVERSION_MINOR;
    err = MFXInit(pv->qsv_info->implementation, &version, &session);
    if (err != MFX_ERR_NONE)
    {
        hb_error("encqsvInit: MFXInit failed (%d)", err);
        return -1;
    }

    /* Query the API version for hb_qsv_plugin_load */
    err = MFXQueryVersion(session, &version);
    if (err != MFX_ERR_NONE)
    {
        hb_error("encqsvInit: MFXQueryVersion failed (%d)", err);
        return -1;
    }

    /* Load optional codec plug-ins */
    err = hb_qsv_plugin_load(session, version, pv->qsv_info->codec_id);
    if (err < MFX_ERR_NONE)
    {
        hb_error("encqsvInit: hb_qsv_plugin_load failed (%d)", err);
        return -1;
    }

    err = MFXVideoENCODE_Init(session, pv->param.videoParam);
// workaround for the early 15.33.x driver, should be removed later
#define HB_DRIVER_FIX_33
#ifdef  HB_DRIVER_FIX_33
    int la_workaround = 0;
    if (err < MFX_ERR_NONE &&
        pv->param.videoParam->mfx.RateControlMethod == MFX_RATECONTROL_LA)
    {
        pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        err = MFXVideoENCODE_Init(session, pv->param.videoParam);
        la_workaround = 1;
    }
#endif
    if (err < MFX_ERR_NONE) // ignore warnings
    {
        hb_error("encqsvInit: MFXVideoENCODE_Init failed (%d)", err);
        hb_qsv_plugin_unload(session, version, pv->qsv_info->codec_id);
        MFXClose(session);
        return -1;
    }
    memset(&videoParam, 0, sizeof(mfxVideoParam));
    videoParam.ExtParam = ExtParamArray;
    videoParam.NumExtParam = 0;
    // introduced in API 1.3
    memset(sps_pps, 0, sizeof(mfxExtCodingOptionSPSPPS));
    sps_pps->Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    sps_pps->Header.BufferSz = sizeof(mfxExtCodingOptionSPSPPS);
    sps_pps->SPSId           = 0;
    sps_pps->SPSBuffer       = w->config->h264.sps;
    sps_pps->SPSBufSize      = sizeof(w->config->h264.sps);
    sps_pps->PPSId           = 0;
    sps_pps->PPSBuffer       = w->config->h264.pps;
    sps_pps->PPSBufSize      = sizeof(w->config->h264.pps);
    if (pv->qsv_info->codec_id == MFX_CODEC_AVC)
    {
        videoParam.ExtParam[videoParam.NumExtParam++] = (mfxExtBuffer*)sps_pps;
    }
    // introduced in API 1.0
    memset(option1, 0, sizeof(mfxExtCodingOption));
    option1->Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    option1->Header.BufferSz = sizeof(mfxExtCodingOption);
    if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION1)
    {
        videoParam.ExtParam[videoParam.NumExtParam++] = (mfxExtBuffer*)option1;
    }
    // introduced in API 1.6
    memset(option2, 0, sizeof(mfxExtCodingOption2));
    option2->Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    option2->Header.BufferSz = sizeof(mfxExtCodingOption2);
    if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2)
    {
        videoParam.ExtParam[videoParam.NumExtParam++] = (mfxExtBuffer*)option2;
    }
    err = MFXVideoENCODE_GetVideoParam(session, &videoParam);
    if (err != MFX_ERR_NONE)
    {
        hb_error("encqsvInit: MFXVideoENCODE_GetVideoParam failed (%d)", err);
        hb_qsv_plugin_unload(session, version, pv->qsv_info->codec_id);
        MFXVideoENCODE_Close(session);
        MFXClose(session);
        return -1;
    }

    /* We have the final encoding parameters, now get the headers for muxing */
    if (pv->qsv_info->codec_id == MFX_CODEC_AVC)
    {
        /* remove 4-byte NAL start code (0x00 0x00 0x00 0x01) */
        w->config->h264.sps_length = sps_pps->SPSBufSize - 4;
        memmove(w->config->h264.sps, w->config->h264.sps + 4,
                w->config->h264.sps_length);
        w->config->h264.pps_length = sps_pps->PPSBufSize - 4;
        memmove(w->config->h264.pps, w->config->h264.pps + 4,
                w->config->h264.pps_length);
    }
    else if (pv->qsv_info->codec_id == MFX_CODEC_HEVC &&
             qsv_hevc_make_header(w, session) < 0)
    {
        hb_error("encqsvInit: qsv_hevc_make_header failed");
        hb_qsv_plugin_unload(session, version, pv->qsv_info->codec_id);
        MFXVideoENCODE_Close(session);
        MFXClose(session);
        return -1;
    }

    /* We don't need this encode session once we have the header */
    MFXVideoENCODE_Close(session);

#ifdef HB_DRIVER_FIX_33
    if (la_workaround)
    {
        videoParam.mfx.RateControlMethod =
        pv->param.videoParam->mfx.RateControlMethod = MFX_RATECONTROL_LA;
        option2->LookAheadDepth = pv->param.codingOption2.LookAheadDepth;
        hb_log("encqsvInit: using LookAhead workaround (\"early 33 fix\")");
    }
#endif

    // when using system memory, we re-use this same session
    if (pv->is_sys_mem)
    {
        pv->mfx_session = session;
    }
    else
    {
        hb_qsv_plugin_unload(session, version, pv->qsv_info->codec_id);
        MFXClose(session);
    }

    // check whether B-frames are used
    if (videoParam.mfx.CodecId == MFX_CODEC_AVC)
    {
        switch (videoParam.mfx.CodecProfile)
        {
            case MFX_PROFILE_AVC_BASELINE:
            case MFX_PROFILE_AVC_CONSTRAINED_HIGH:
            case MFX_PROFILE_AVC_CONSTRAINED_BASELINE:
                pv->bfrm_delay = 0;
                break;
            default:
                pv->bfrm_delay = 1;
                break;
        }
    }
    else if (videoParam.mfx.CodecId == MFX_CODEC_HEVC)
    {
        switch (videoParam.mfx.CodecProfile)
        {
            case MFX_PROFILE_HEVC_MAINSP:
                pv->bfrm_delay = 0;
                break;
            default:
                pv->bfrm_delay = 1;
                break;
        }
    }
    else
    {
        pv->bfrm_delay = 1; // assume B-frames are enabled by default
    }
    // sanitize
    pv->bfrm_delay = FFMIN(pv->bfrm_delay, videoParam.mfx.GopRefDist - 1);
    pv->bfrm_delay = FFMIN(pv->bfrm_delay, videoParam.mfx.GopPicSize - 2);
    pv->bfrm_delay = FFMAX(pv->bfrm_delay, 0);
    // let the muxer know whether to expect B-frames or not
    job->areBframes = !!pv->bfrm_delay;
    // check whether we need to generate DTS ourselves (MSDK API < 1.6 or VFR)
    pv->bfrm_workaround = job->cfr != 1 || !(pv->qsv_info->capabilities &
                                             HB_QSV_CAP_MSDK_API_1_6);
    if (pv->bfrm_delay && pv->bfrm_workaround)
    {
        pv->bfrm_workaround = 1;
        pv->list_dts        = hb_list_init();
    }
    else
    {
        pv->bfrm_workaround = 0;
        pv->list_dts        = NULL;
    }

    // log code path and main output settings
    hb_log("encqsvInit: using %s path",
           pv->is_sys_mem ? "encode-only" : "full QSV");
    hb_log("encqsvInit: TargetUsage %"PRIu16" AsyncDepth %"PRIu16"",
           videoParam.mfx.TargetUsage, videoParam.AsyncDepth);
    hb_log("encqsvInit: GopRefDist %"PRIu16" GopPicSize %"PRIu16" NumRefFrame %"PRIu16"",
           videoParam.mfx.GopRefDist, videoParam.mfx.GopPicSize, videoParam.mfx.NumRefFrame);
    if (pv->qsv_info->capabilities & HB_QSV_CAP_B_REF_PYRAMID)
    {
        hb_log("encqsvInit: BFrames %s BPyramid %s",
               pv->bfrm_delay                            ? "on" : "off",
               pv->bfrm_delay && pv->param.gop.b_pyramid ? "on" : "off");
    }
    else
    {
        hb_log("encqsvInit: BFrames %s", pv->bfrm_delay ? "on" : "off");
    }
    if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2_IB_ADAPT)
    {
        if (pv->bfrm_delay > 0)
        {
            hb_log("encqsvInit: AdaptiveI %s AdaptiveB %s",
                   hb_qsv_codingoption_get_name(option2->AdaptiveI),
                   hb_qsv_codingoption_get_name(option2->AdaptiveB));
        }
        else
        {
            hb_log("encqsvInit: AdaptiveI %s",
                   hb_qsv_codingoption_get_name(option2->AdaptiveI));
        }
    }
    if (videoParam.mfx.RateControlMethod == MFX_RATECONTROL_CQP)
    {
        char qpi[7], qpp[9], qpb[9];
        snprintf(qpi, sizeof(qpi),  "QPI %"PRIu16"", videoParam.mfx.QPI);
        snprintf(qpp, sizeof(qpp), " QPP %"PRIu16"", videoParam.mfx.QPP);
        snprintf(qpb, sizeof(qpb), " QPB %"PRIu16"", videoParam.mfx.QPB);
        hb_log("encqsvInit: RateControlMethod CQP with %s%s%s", qpi,
               videoParam.mfx.GopPicSize > 1 ? qpp : "",
               videoParam.mfx.GopRefDist > 1 ? qpb : "");
    }
    else
    {
        switch (videoParam.mfx.RateControlMethod)
        {
            case MFX_RATECONTROL_LA:
                hb_log("encqsvInit: RateControlMethod LA TargetKbps %"PRIu16" LookAheadDepth %"PRIu16"",
                       videoParam.mfx.TargetKbps, option2->LookAheadDepth);
                break;
            case MFX_RATECONTROL_LA_ICQ:
                hb_log("encqsvInit: RateControlMethod LA_ICQ ICQQuality %"PRIu16" LookAheadDepth %"PRIu16"",
                       videoParam.mfx.ICQQuality, option2->LookAheadDepth);
                break;
            case MFX_RATECONTROL_ICQ:
                hb_log("encqsvInit: RateControlMethod ICQ ICQQuality %"PRIu16"",
                       videoParam.mfx.ICQQuality);
                break;
            case MFX_RATECONTROL_CBR:
            case MFX_RATECONTROL_VBR:
                hb_log("encqsvInit: RateControlMethod %s TargetKbps %"PRIu16" MaxKbps %"PRIu16" BufferSizeInKB %"PRIu16" InitialDelayInKB %"PRIu16"",
                       videoParam.mfx.RateControlMethod == MFX_RATECONTROL_CBR ? "CBR" : "VBR",
                       videoParam.mfx.TargetKbps,     videoParam.mfx.MaxKbps,
                       videoParam.mfx.BufferSizeInKB, videoParam.mfx.InitialDelayInKB);
                break;
            default:
                hb_log("encqsvInit: invalid rate control method %"PRIu16"",
                       videoParam.mfx.RateControlMethod);
                return -1;
        }
    }
    if ((pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2_LA_DOWNS) &&
        (videoParam.mfx.RateControlMethod == MFX_RATECONTROL_LA ||
         videoParam.mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ))
    {
        switch (option2->LookAheadDS)
        {
            case MFX_LOOKAHEAD_DS_UNKNOWN:
                hb_log("encqsvInit: LookAheadDS unknown (auto)");
                break;
            case MFX_LOOKAHEAD_DS_OFF:
                hb_log("encqsvInit: LookAheadDS off");
                break;
            case MFX_LOOKAHEAD_DS_2x:
                hb_log("encqsvInit: LookAheadDS 2x");
                break;
            case MFX_LOOKAHEAD_DS_4x:
                hb_log("encqsvInit: LookAheadDS 4x");
                break;
            default:
                hb_log("encqsvInit: invalid LookAheadDS value 0x%"PRIx16"",
                       option2->LookAheadDS);
                break;
        }
    }
    switch (videoParam.mfx.FrameInfo.PicStruct)
    {
        // quiet, most people don't care
        case MFX_PICSTRUCT_PROGRESSIVE:
            break;
        // interlaced encoding is intended for advanced users only, who do care
        case MFX_PICSTRUCT_FIELD_TFF:
            hb_log("encqsvInit: PicStruct top field first");
            break;
        case MFX_PICSTRUCT_FIELD_BFF:
            hb_log("encqsvInit: PicStruct bottom field first");
            break;
        default:
            hb_error("encqsvInit: invalid PicStruct value 0x%"PRIx16"",
                     videoParam.mfx.FrameInfo.PicStruct);
            return -1;
    }
    if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION1)
    {
        hb_log("encqsvInit: CAVLC %s",
               hb_qsv_codingoption_get_name(option1->CAVLC));
    }
    if (pv->param.rc.lookahead           == 0 &&
        videoParam.mfx.RateControlMethod != MFX_RATECONTROL_CQP)
    {
        // LA/CQP and ExtBRC/MBBRC are mutually exclusive
        if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2_EXTBRC)
        {
            hb_log("encqsvInit: ExtBRC %s",
                   hb_qsv_codingoption_get_name(option2->ExtBRC));
        }
        if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2_MBBRC)
        {
            hb_log("encqsvInit: MBBRC %s",
                   hb_qsv_codingoption_get_name(option2->MBBRC));
        }
    }
    if (pv->qsv_info->capabilities & HB_QSV_CAP_OPTION2_TRELLIS)
    {
        switch (option2->Trellis)
        {
            case MFX_TRELLIS_OFF:
                hb_log("encqsvInit: Trellis off");
                break;
            case MFX_TRELLIS_UNKNOWN:
                hb_log("encqsvInit: Trellis unknown (auto)");
                break;
            default:
                hb_log("encqsvInit: Trellis on (%s%s%s)",
                       (option2->Trellis & MFX_TRELLIS_I) ? "I" : "",
                       (option2->Trellis & MFX_TRELLIS_P) &&
                       (videoParam.mfx.GopPicSize > 1)    ? "P" : "",
                       (option2->Trellis & MFX_TRELLIS_B) &&
                       (videoParam.mfx.GopRefDist > 1)    ? "B" : "");
                break;
        }
    }
    hb_log("encqsvInit: %s profile %s @ level %s",
           hb_qsv_codec_name  (videoParam.mfx.CodecId),
           hb_qsv_profile_name(videoParam.mfx.CodecId, videoParam.mfx.CodecProfile),
           hb_qsv_level_name  (videoParam.mfx.CodecId, videoParam.mfx.CodecLevel));

    // AsyncDepth has now been set and/or modified by Media SDK
    pv->max_async_depth = videoParam.AsyncDepth;
    pv->async_depth     = 0;

    return 0;
}

void encqsvClose(hb_work_object_t *w)
{
    int i;
    hb_work_private_t *pv = w->private_data;

    if (pv != NULL && pv->job != NULL && pv->job->qsv.ctx != NULL &&
        pv->job->qsv.ctx->is_context_active)
    {

        av_qsv_context *qsv = pv->job->qsv.ctx;

        if (qsv->enc_space != NULL)
        {
            av_qsv_space *qsv_encode = qsv->enc_space;

            if (qsv_encode->is_init_done)
            {
                if (pv->is_sys_mem)
                {
                    if (qsv_encode->surface_num > 0)
                    {
                        for (i = 0; i < qsv_encode->surface_num; i++)
                        {
                            if (qsv_encode->p_surfaces[i]->Data.Y != NULL)
                            {
                                free(qsv_encode->p_surfaces[i]->Data.Y);
                                qsv_encode->p_surfaces[i]->Data.Y = NULL;
                            }
                            if (qsv_encode->p_surfaces[i]->Data.VU != NULL)
                            {
                                free(qsv_encode->p_surfaces[i]->Data.VU);
                                qsv_encode->p_surfaces[i]->Data.VU = NULL;
                            }
                        }
                    }

                    sws_freeContext(pv->sws_context_to_nv12);
                }

                for (i = av_qsv_list_count(qsv_encode->tasks); i > 1; i--)
                {
                    av_qsv_task *task = av_qsv_list_item(qsv_encode->tasks, i - 1);

                    if (task != NULL && task->bs != NULL)
                    {
                        av_qsv_list_rem(qsv_encode->tasks, task);
                        av_freep(&task->bs->Data);
                        av_freep(&task->bs);
                    }
                }
                av_qsv_list_close(&qsv_encode->tasks);

                for (i = 0; i < qsv_encode->surface_num; i++)
                {
                    av_freep(&qsv_encode->p_surfaces[i]);
                }
                qsv_encode->surface_num = 0;

                for (i = 0; i < qsv_encode->sync_num; i++)
                {
                    av_freep(&qsv_encode->p_syncp[i]->p_sync);
                    av_freep(&qsv_encode->p_syncp[i]);
                }
                qsv_encode->sync_num = 0;

                qsv_encode->is_init_done = 0;
            }
        }

        if (qsv != NULL)
        {
            mfxVersion version;

            /* Unload optional codec plug-ins */
            if (MFXQueryVersion(qsv->mfx_session, &version) == MFX_ERR_NONE)
            {
                hb_qsv_plugin_unload(qsv->mfx_session, version,
                                     pv->qsv_info->codec_id);
            }

            /* QSV context cleanup and MFXClose */
            av_qsv_context_clean(qsv);

            if (pv->is_sys_mem)
            {
                av_freep(&qsv);
            }
        }
    }

    if (pv != NULL)
    {
        if (pv->list_dts != NULL)
        {
            while (hb_list_count(pv->list_dts) > 0)
            {
                int64_t *item = hb_list_item(pv->list_dts, 0);
                hb_list_rem(pv->list_dts, item);
                free(item);
            }
            hb_list_close(&pv->list_dts);
        }

        if (pv->delayed_chapters != NULL)
        {
            struct chapter_s *item;

            while ((item = hb_list_item(pv->delayed_chapters, 0)) != NULL)
            {
                hb_list_rem(pv->delayed_chapters, item);
                free(item);
            }
            hb_list_close(&pv->delayed_chapters);
        }

        if (pv->encoded_frames != NULL)
        {
            hb_buffer_t *item;

            while ((item = hb_list_item(pv->encoded_frames, 0)) != NULL)
            {
                // should never happen
                hb_list_rem(pv->encoded_frames, item);
                hb_buffer_close(&item);
            }
            hb_list_close(&pv->encoded_frames);
        }
    }

    free(pv);
    w->private_data = NULL;
}

static void save_chapter(hb_work_private_t *pv, hb_buffer_t *buf)
{
    /*
     * Since there may be several frames buffered in the encoder, remember the
     * timestamp so when this frame finally pops out of the encoder we'll mark
     * its buffer as the start of a chapter.
     */
    if (pv->next_chapter_pts == AV_NOPTS_VALUE)
    {
        pv->next_chapter_pts = buf->s.start;
    }

    /*
     * Chapter markers are sometimes so close we can get a new
     * one before the previous goes through the encoding queue.
     *
     * Dropping markers can cause weird side-effects downstream,
     * including but not limited to missing chapters in the
     * output, so we need to save it somehow.
     */
    struct chapter_s *item = malloc(sizeof(struct chapter_s));
    if (item != NULL)
    {
        item->start = buf->s.start;
        item->index = buf->s.new_chap;
        hb_list_add(pv->delayed_chapters, item);
    }

    /* don't let 'work_loop' put a chapter mark on the wrong buffer */
    buf->s.new_chap = 0;
}

static void restore_chapter(hb_work_private_t *pv, hb_buffer_t *buf)
{
    /* we're no longer looking for this chapter */
    pv->next_chapter_pts = AV_NOPTS_VALUE;

    /* get the chapter index from the list */
    struct chapter_s *item = hb_list_item(pv->delayed_chapters, 0);
    if (item != NULL)
    {
        /* we're done with this chapter */
        hb_list_rem(pv->delayed_chapters, item);
        buf->s.new_chap = item->index;
        free(item);

        /* we may still have another pending chapter */
        item = hb_list_item(pv->delayed_chapters, 0);
        if (item != NULL)
        {
            /*
             * we're looking for this chapter now
             * we still need it, don't remove it
             */
            pv->next_chapter_pts = item->start;
        }
    }
}

static hb_buffer_t* bitstream2buf(hb_work_private_t *pv, mfxBitstream *bs)
{
    hb_buffer_t *buf;

    if (pv->qsv_info->codec_id == MFX_CODEC_AVC)
    {
        /*
         * we need to convert the encoder's Annex B output
         * to an MP4-compatible format (ISO/IEC 14496-15).
         */
        buf = hb_nal_bitstream_annexb_to_mp4(bs->Data + bs->DataOffset, bs->DataLength);
        if (buf == NULL)
        {
            return NULL;
        }
    }
    else
    {
        // muxers will take care of re-formatting the bitstream
        buf = hb_buffer_init(bs->DataLength);
        if (buf == NULL)
        {
            return NULL;
        }

        memcpy(buf->data, bs->Data + bs->DataOffset, bs->DataLength);
    }

    // map Media SDK's FrameType to our internal representation
    buf->s.frametype = hb_qsv_frametype_xlat(bs->FrameType, &buf->s.flags);

    buf->s.start    = buf->s.renderOffset = bs->TimeStamp;
    buf->s.stop     = buf->s.start + pv->default_duration;
    buf->s.duration = pv->default_duration;

    if (pv->bfrm_delay)
    {
        if (!pv->bfrm_workaround)
        {
            buf->s.renderOffset = bs->DecodeTimeStamp;
        }
        else
        {
            /*
             * MSDK API < 1.6 or VFR
             *
             * Generate VFR-compatible output DTS based on input PTS.
             *
             * Depends on the B-frame delay:
             *
             * 0: ipts0,  ipts1, ipts2...
             * 1: ipts0 - ipts1, ipts1 - ipts1, ipts1,  ipts2...
             * 2: ipts0 - ipts2, ipts1 - ipts2, ipts2 - ipts2, ipts1...
             * ...and so on.
             */
            if (pv->frames_out <= pv->bfrm_delay)
            {
                buf->s.renderOffset = (pv->init_pts[pv->frames_out] -
                                       pv->init_pts[pv->bfrm_delay]);
            }
            else
            {
                buf->s.renderOffset = hb_qsv_pop_next_dts(pv->list_dts);
            }
        }

        // check whether B-pyramid is used even though it's disabled
        if ((pv->param.gop.b_pyramid == 0)    &&
            (bs->FrameType & MFX_FRAMETYPE_B) &&
            (bs->FrameType & MFX_FRAMETYPE_REF))
        {
            hb_log("encqsvWork: BPyramid off not respected (delay: %d)",
                   pv->bfrm_delay);
        }

        // check for PTS < DTS
        if (buf->s.start < buf->s.renderOffset)
        {
            hb_log("encqsvWork: PTS %"PRId64" < DTS %"PRId64" for frame %d with type '%s' (bfrm_workaround: %d)",
                   buf->s.start, buf->s.renderOffset, pv->frames_out + 1,
                   hb_qsv_frametype_name(bs->FrameType),
                   pv->bfrm_workaround);
        }
    }

    /*
     * If we have a chapter marker pending and this frame's presentation
     * time stamp is at or after the marker's time stamp, use this frame
     * as the chapter start.
     */
    if (pv->next_chapter_pts != AV_NOPTS_VALUE &&
        pv->next_chapter_pts <= buf->s.start   && (bs->FrameType &
                                                   MFX_FRAMETYPE_IDR))
    {
        restore_chapter(pv, buf);
    }

    pv->frames_out++;
    return buf;
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

static int encode_loop(hb_work_object_t *w, av_qsv_list *qsv_atom,
                       mfxEncodeCtrl *ctrl, mfxFrameSurface1 *surface)
{
    hb_work_private_t *pv       = w->private_data;
    av_qsv_context *qsv_ctx     = pv->job->qsv.ctx;
    av_qsv_space *qsv_enc_space = pv->job->qsv.ctx->enc_space;
    mfxStatus sts               = MFX_ERR_NONE;
    hb_buffer_t *buf;
    int i;

    do
    {
        int sync_idx = av_qsv_get_free_sync(qsv_enc_space, qsv_ctx);
        if (sync_idx == -1)
        {
            hb_error("encqsv: av_qsv_get_free_sync failed");
            return -1;
        }
        av_qsv_task *task = av_qsv_list_item(qsv_enc_space->tasks, pv->async_depth);

        do
        {
            // encode a frame asychronously (returns immediately)
            sts = MFXVideoENCODE_EncodeFrameAsync(qsv_ctx->mfx_session,
                                                  ctrl, surface, task->bs,
                                                  qsv_enc_space->p_syncp[sync_idx]->p_sync);

            if (sts == MFX_ERR_MORE_DATA || (sts >= MFX_ERR_NONE &&
                                             sts != MFX_WRN_DEVICE_BUSY))
            {
                if (surface != NULL && !pv->is_sys_mem)
                {
                    ff_qsv_atomic_dec(&surface->Data.Locked);
                }
            }

            if (sts == MFX_ERR_MORE_DATA)
            {
                ff_qsv_atomic_dec(&qsv_enc_space->p_syncp[sync_idx]->in_use);

                if (surface != NULL && qsv_atom != NULL)
                {
                    hb_list_add(pv->delayed_processing, qsv_atom);
                }
                break;
            }

            if (sts < MFX_ERR_NONE)
            {
                hb_error("encqsv: MFXVideoENCODE_EncodeFrameAsync failed (%s)", sts);
                return -1;
            }

            if (sts >= MFX_ERR_NONE /*&& !syncpE*/) // repeat the call if warning and no output
            {
                if (sts == MFX_WRN_DEVICE_BUSY)
                {
                    av_qsv_sleep(10); // wait if device is busy
                    continue;
                }

                av_qsv_stage *new_stage = av_qsv_stage_init();
                new_stage->type         = AV_QSV_ENCODE;
                new_stage->in.p_surface = surface;
                new_stage->out.sync     = qsv_enc_space->p_syncp[sync_idx];
                new_stage->out.p_bs     = task->bs;
                task->stage             = new_stage;

                pv->async_depth++;

                if (qsv_atom != NULL)
                {
                    av_qsv_add_stagee(&qsv_atom, new_stage, HAVE_THREADS);
                }
                else
                {
                    // flushing the end
                    int pipe_idx           = av_qsv_list_add (qsv_ctx->pipes, av_qsv_list_init(HAVE_THREADS));
                    av_qsv_list *list_item = av_qsv_list_item(qsv_ctx->pipes, pipe_idx);
                    av_qsv_add_stagee(&list_item, new_stage, HAVE_THREADS);
                }

                for (i = hb_list_count(pv->delayed_processing); i > 0; i--)
                {
                    hb_list_t *item = hb_list_item(pv->delayed_processing, i - 1);

                    if (item != NULL)
                    {
                        hb_list_rem(pv->delayed_processing, item);
                        av_qsv_flush_stages(qsv_ctx->pipes, (av_qsv_list**)&item);
                    }
                }
                break;
            }

            ff_qsv_atomic_dec(&qsv_enc_space->p_syncp[sync_idx]->in_use);

            if (sts == MFX_ERR_NOT_ENOUGH_BUFFER)
            {
                hb_error("encqsv: bitstream buffer too small");
                return -1;
            }
            break;
        }
        while (1);

        buf = NULL;

        do
        {
            if (pv->async_depth == 0) break;

            // working properly with sync depth approach of MediaSDK OR flushing
            if (pv->async_depth >= pv->max_async_depth || surface == NULL)
            {
                pv->async_depth--;

                sts                     = MFX_ERR_NONE;
                av_qsv_task  *task      = av_qsv_list_item(qsv_enc_space->tasks, 0);
                av_qsv_stage *stage     = task->stage;
                av_qsv_list  *this_pipe = av_qsv_pipe_by_stage(qsv_ctx->pipes, stage);

                // only here we need to wait on operation been completed, therefore SyncOperation is used,
                // after this step - we continue to work with bitstream, muxing ...
                av_qsv_wait_on_sync(qsv_ctx, stage);

                if (task->bs->DataLength > 0)
                {
                    av_qsv_flush_stages(qsv_ctx->pipes, &this_pipe);

                    if ((pv->bfrm_delay && pv->frames_out == 0)                &&
                        (pv->qsv_info->capabilities & HB_QSV_CAP_MSDK_API_1_6) &&
                        (pv->qsv_info->capabilities & HB_QSV_CAP_B_REF_PYRAMID))
                    {
                        /*
                         * with B-pyramid, the delay may be more than 1 frame,
                         * so compute the actual delay based on the initial DTS
                         * provided by MSDK; variables are used for readibility.
                         */
                        int64_t init_delay = (task->bs->TimeStamp -
                                              task->bs->DecodeTimeStamp);

                        pv->bfrm_delay = HB_QSV_CLIP3(1, BFRM_DELAY_MAX,
                                                      init_delay /
                                                      pv->default_duration);

                        /*
                         * In the MP4 container, DT(0) = STTS(0) = 0.
                         *
                         * Which gives us:
                         * CT(0) = CTTS(0) + STTS(0) = CTTS(0) = PTS(0) - DTS(0)
                         * When DTS(0) < PTS(0), we then have:
                         * CT(0) > 0 for video, but not audio (breaks A/V sync).
                         *
                         * This is typically solved by writing an edit list shifting
                         * video samples by the initial delay, PTS(0) - DTS(0).
                         *
                         * See:
                         * ISO/IEC 14496-12:2008(E), ISO base media file format
                         *  - 8.6.1.2 Decoding Time to Sample Box
                         */
                        /* TODO: factor out init_delay setup? */
                        if (pv->bfrm_workaround)
                        {
                            w->config->h264.init_delay = (pv->init_pts[pv->bfrm_delay] -
                                                          pv->init_pts[0]);
                        }
                        else
                        {
                            w->config->h264.init_delay = init_delay;
                        }
                    }

                    buf = bitstream2buf(pv, task->bs);
                    hb_list_add(pv->encoded_frames, buf);

                    // shift for fifo
                    if (pv->async_depth)
                    {
                        av_qsv_list_rem(qsv_enc_space->tasks, task);
                        av_qsv_list_add(qsv_enc_space->tasks, task);
                    }

                    task->stage          = 0;
                    task->bs->DataLength = 0;
                    task->bs->DataOffset = 0;
                    task->bs->MaxLength  = qsv_enc_space->p_buf_max_size;
                }
            }
        }
        while (surface == NULL);

        /* TODO: condition below, instead of 1 */
        if (surface != NULL || (buf == NULL && sts == MFX_ERR_MORE_DATA))
        {
            break;
        }
    }
    while (1);

    return 0;
}

int encqsvWork(hb_work_object_t *w, hb_buffer_t **buf_in, hb_buffer_t **buf_out)
{
    hb_work_private_t *pv   = w->private_data;
    hb_job_t *job           = pv->job;
    av_qsv_context *qsv_ctx = pv->job->qsv.ctx;
    hb_buffer_t *in         = *buf_in;

    while (1)
    {
        int ret = qsv_enc_init(qsv_ctx, pv);
        qsv_ctx = job->qsv.ctx;

        if (ret >= 2)
        {
            av_qsv_sleep(1); // encoding not yet done initializing, wait
            continue;
        }
        break;
    }

    if (*job->die)
    {
        *buf_out = NULL;
        return HB_WORK_DONE; // unrecoverable error in qsv_enc_init
    }

    /*
     * EOF on input. Flush the decoder, then send the
     * EOF downstream to let the muxer know we're done.
     */
    if (in->size <= 0)
    {
        encode_loop(w, NULL, NULL, NULL);
        hb_list_add(pv->encoded_frames, in);
        *buf_out = link_buffer_list(pv->encoded_frames);
        *buf_in  = NULL; // don't let 'work_loop' close this buffer
        return HB_WORK_DONE;
    }

    av_qsv_list *qsv_atom       = NULL;
    mfxEncodeCtrl *ctrl         = NULL;
    mfxFrameSurface1 *surface   = NULL;
    av_qsv_space *qsv_enc_space = qsv_ctx->enc_space;

    /*
     * If we have some input, we need to obtain
     * an unlocked frame surface and set it up.
     */
    if (pv->is_sys_mem)
    {
        mfxFrameInfo *fip = &qsv_enc_space->request[0].Info;
        int surface_index = av_qsv_get_free_surface(qsv_enc_space, qsv_ctx, fip,
                                                    QSV_PART_ANY);

        surface = qsv_enc_space->p_surfaces[surface_index];

        qsv_yuv420_to_nv12(pv->sws_context_to_nv12, surface, in);
    }
    else
    {
        qsv_atom            = in->qsv_details.qsv_atom;
        av_qsv_stage *stage = av_qsv_get_last_stage(qsv_atom);
        surface             = stage->out.p_surface;

        // don't let qsv_ctx->dts_seq grow needlessly
        av_qsv_dts_pop(qsv_ctx);
    }

    /*
     * Debugging code to check that the upstream modules have generated
     * a continuous, self-consistent frame stream.
     */
    if (pv->last_start > in->s.start)
    {
        hb_log("encqsvWork: input continuity error, last start %"PRId64" start %"PRId64"",
               pv->last_start, in->s.start);
    }
    pv->last_start = in->s.start;

    // for DTS generation (when MSDK API < 1.6 or VFR)
    if (pv->bfrm_delay && pv->bfrm_workaround)
    {
        if (pv->frames_in <= BFRM_DELAY_MAX)
        {
            pv->init_pts[pv->frames_in] = in->s.start;
        }
        if (pv->frames_in)
        {
            hb_qsv_add_new_dts(pv->list_dts, in->s.start);
        }
    }

    /*
     * Chapters have to start with a keyframe so request that this
     * frame be coded as IDR. Note: this may cause issues with
     * frame reordering, so we have to flush the encoder first.
     */
    if (in->s.new_chap > 0 && job->chapter_markers)
    {
        if (encode_loop(w, NULL, NULL, NULL) < 0)
        {
            goto fail;
        }
        ctrl = &pv->force_keyframe;
        save_chapter(pv, in);
    }

    /*
     * If interlaced encoding is requested during encoder initialization,
     * but the input mfxFrameSurface1 is flagged as progressive here,
     * the output bitstream will be progressive (according to MediaInfo).
     *
     * Assume the user knows what he's doing (say he is e.g. encoding a
     * progressive-flagged source using interlaced compression - he may
     * well have a good reason to do so; mis-flagged sources do exist).
     */
    surface->Info.PicStruct = pv->param.videoParam->mfx.FrameInfo.PicStruct;
    surface->Data.TimeStamp = in->s.start;

    // this is a non-EOF input packet, so an input frame
    pv->frames_in++;

    /*
     * Now that the input surface is setup, we can encode it.
     */
    if (encode_loop(w, qsv_atom, ctrl, surface) < 0)
    {
        goto fail;
    }

    *buf_out = link_buffer_list(pv->encoded_frames);
    return HB_WORK_OK;

fail:
    *buf_out = NULL;
    return HB_WORK_ERROR;
}

#endif // USE_QSV
