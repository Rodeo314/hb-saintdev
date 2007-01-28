/* $Id: sync.c,v 1.38 2005/04/14 21:57:58 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include "hb.h"

#include "samplerate.h"
#include "ffmpeg/avcodec.h"

#ifdef INT64_MIN
#undef INT64_MIN /* Because it isn't defined correctly in Zeta */
#endif
#define INT64_MIN (-9223372036854775807LL-1)

#define AC3_SAMPLES_PER_FRAME 1536

typedef struct
{
    hb_audio_t * audio;
    int64_t      count_frames;
    
    /* Raw */
    SRC_STATE  * state;
    SRC_DATA     data;

    /* AC-3 */
    int          ac3_size;
    uint8_t    * ac3_buf;

} hb_sync_audio_t;

struct hb_work_private_s
{
    hb_job_t * job;
    int        done;

    /* Video */
    hb_subtitle_t * subtitle;
    int64_t pts_offset;
    int64_t pts_offset_old;
    int64_t count_frames;
    int64_t count_frames_max;
    hb_buffer_t * cur; /* The next picture to process */

    /* Audio */
    hb_sync_audio_t sync_audio[8];

    /* Statistics */
    uint64_t st_counts[4];
    uint64_t st_dates[4];
    uint64_t st_first;
};

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static void InitAudio( hb_work_object_t * w, int i );
static int  SyncVideo( hb_work_object_t * w );
static void SyncAudio( hb_work_object_t * w, int i );
static int  NeedSilence( hb_work_object_t * w, hb_audio_t * );
static void InsertSilence( hb_work_object_t * w, int i );
static void UpdateState( hb_work_object_t * w );

/***********************************************************************
 * hb_work_sync_init
 ***********************************************************************
 * Initialize the work object
 **********************************************************************/
int syncInit( hb_work_object_t * w, hb_job_t * job )
{
    hb_title_t       * title = job->title;
    hb_chapter_t     * chapter;
    int                i;
    uint64_t           duration;
    hb_work_private_t * pv;

    pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job            = job;
    pv->pts_offset     = INT64_MIN;
    pv->pts_offset_old = INT64_MIN;
    pv->count_frames   = 0;

    /* Calculate how many video frames we are expecting */
    duration = 0;
    for( i = job->chapter_start; i <= job->chapter_end; i++ )
    {
        chapter   = hb_list_item( title->list_chapter, i - 1 );
        duration += chapter->duration;
    }                                                                           
    duration += 90000;
        /* 1 second safety so we're sure we won't miss anything */
    pv->count_frames_max = duration * job->vrate / job->vrate_base / 90000;

    hb_log( "sync: expecting %lld video frames", pv->count_frames_max );

    /* Initialize libsamplerate for every audio track we have */
    for( i = 0; i < hb_list_count( title->list_audio ); i++ )
    {
        InitAudio( w, i );
    }

    /* Get subtitle info, if any */
    pv->subtitle = hb_list_item( title->list_subtitle, 0 );

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
void syncClose( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t          * job   = pv->job;
    hb_title_t        * title = job->title;
    
    int i;

    if( pv->cur ) hb_buffer_close( &pv->cur );

    for( i = 0; i < hb_list_count( title->list_audio ); i++ )
    {
        if( job->acodec & HB_ACODEC_AC3 )
        {
            free( pv->sync_audio[i].ac3_buf );
        }
        else
        {
            src_delete( pv->sync_audio[i].state );
        }
    }
}

/***********************************************************************
 * Work
 ***********************************************************************
 * The root routine of this work abject
 **********************************************************************/
int syncWork( hb_work_object_t * w, hb_buffer_t ** unused1,
                 hb_buffer_t ** unused2 )
{
    hb_work_private_t * pv = w->private_data;
    int i;

    /* If we ever got a video frame, handle audio now */
    if( pv->pts_offset != INT64_MIN )
    {
        for( i = 0; i < hb_list_count( pv->job->title->list_audio ); i++ )
        {
            SyncAudio( w, i );
        }
    }

    /* Handle video */
    return SyncVideo( w );
}

hb_work_object_t hb_sync =
{
    WORK_SYNC,
    "Synchronization",
    syncInit,
    syncWork,
    syncClose
};

static void InitAudio( hb_work_object_t * w, int i )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t        * job   = pv->job;
    hb_title_t      * title = job->title;
    hb_sync_audio_t * sync;

    sync        = &pv->sync_audio[i];
    sync->audio = hb_list_item( title->list_audio, i );

    if( job->acodec & HB_ACODEC_AC3 )
    {
        /* Have a silent AC-3 frame ready in case we have to fill a
           gap */
        AVCodec        * codec;
        AVCodecContext * c;
        short          * zeros;

        codec = avcodec_find_encoder( CODEC_ID_AC3 );
        c     = avcodec_alloc_context();

        c->bit_rate    = sync->audio->bitrate;
        c->sample_rate = sync->audio->rate;
        c->channels    = sync->audio->channels;

        if( avcodec_open( c, codec ) < 0 )
        {
            hb_log( "sync: avcodec_open failed" );
            return;
        }

        zeros          = calloc( AC3_SAMPLES_PER_FRAME *
                                 sizeof( short ) * c->channels, 1 );
        sync->ac3_size = sync->audio->bitrate * AC3_SAMPLES_PER_FRAME /
                             sync->audio->rate / 8;
        sync->ac3_buf  = malloc( sync->ac3_size );

        if( avcodec_encode_audio( c, sync->ac3_buf, sync->ac3_size,
                                  zeros ) != sync->ac3_size )
        {
            hb_log( "sync: avcodec_encode_audio failed" );
        }
        
        free( zeros );
        avcodec_close( c );
        av_free( c );
    }
    else
    {
        /* Initialize libsamplerate */
        int error;
        sync->state             = src_new( SRC_LINEAR, 2, &error );
        sync->data.end_of_input = 0;
    }
}



#define PTS_DISCONTINUITY_TOLERANCE 90000

/***********************************************************************
 * SyncVideo
 ***********************************************************************
 * 
 **********************************************************************/
static int SyncVideo( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * cur, * next, * sub = NULL;
    hb_job_t * job = pv->job;
    int64_t pts_expected;

    if( pv->done )
    {
        return HB_WORK_DONE;
    }

    if( hb_thread_has_exited( job->reader ) &&
        !hb_fifo_size( job->fifo_mpeg2 ) &&
        !hb_fifo_size( job->fifo_raw ) )
    {
        /* All video data has been processed already, we won't get
           more */
        hb_log( "sync: got %lld frames, %lld expected",
                pv->count_frames, pv->count_frames_max );
        pv->done = 1;
        return HB_WORK_DONE;
    }

    if( !pv->cur && !( pv->cur = hb_fifo_get( job->fifo_raw ) ) )
    {
        /* We haven't even got a frame yet */
        return HB_WORK_OK;
    }
    cur = pv->cur;

    /* At this point we have a frame to process. Let's check
        1) if we will be able to push into the fifo ahead
        2) if the next frame is there already, since we need it to
           know whether we'll have to repeat the current frame or not */
    while( !hb_fifo_is_full( job->fifo_sync ) &&
           ( next = hb_fifo_see( job->fifo_raw ) ) )
    {
        hb_buffer_t * buf_tmp;

        if( pv->pts_offset == INT64_MIN )
        {
            /* This is our first frame */
            hb_log( "sync: first pts is %lld", cur->start );
            pv->pts_offset = cur->start;
        }

        /* Check for PTS jumps over 0.5 second */
        if( next->start < cur->start - PTS_DISCONTINUITY_TOLERANCE ||
            next->start > cur->start + PTS_DISCONTINUITY_TOLERANCE )
        {
            hb_log( "PTS discontinuity (%lld, %lld)",
                    cur->start, next->start );
            
            /* Trash all subtitles */
            if( pv->subtitle )
            {
                while( ( sub = hb_fifo_get( pv->subtitle->fifo_raw ) ) )
                {
                    hb_buffer_close( &sub );
                }
            }

            /* Trash current picture */
            hb_buffer_close( &cur );
            pv->cur = cur = hb_fifo_get( job->fifo_raw );

            /* Calculate new offset */
            pv->pts_offset_old = pv->pts_offset;
            pv->pts_offset     = cur->start -
                pv->count_frames * pv->job->vrate_base / 300;
            continue;
        }

        /* Look for a subtitle for this frame */
        if( pv->subtitle )
        {
            hb_buffer_t * sub2;
            while( ( sub = hb_fifo_see( pv->subtitle->fifo_raw ) ) )
            {
                /* If two subtitles overlap, make the first one stop
                   when the second one starts */
                sub2 = hb_fifo_see2( pv->subtitle->fifo_raw );
                if( sub2 && sub->stop > sub2->start )
                    sub->stop = sub2->start;

                if( sub->stop > cur->start )
                    break;

                /* The subtitle is older than this picture, trash it */
                sub = hb_fifo_get( pv->subtitle->fifo_raw );
                hb_buffer_close( &sub );
            }

            /* If we have subtitles left in the fifo, check if we should
               apply the first one to the current frame or if we should
               keep it for later */
            if( sub && sub->start > cur->start )
            {
                sub = NULL;
            }
        }

        /* The PTS of the frame we are expecting now */
        pts_expected = pv->pts_offset +
            pv->count_frames * pv->job->vrate_base / 300;

        if( cur->start < pts_expected - pv->job->vrate_base / 300 / 2 &&
            next->start < pts_expected + pv->job->vrate_base / 300 / 2 )
        {
            /* The current frame is too old but the next one matches,
               let's trash */
            hb_buffer_close( &cur );
            pv->cur = cur = hb_fifo_get( job->fifo_raw );
            continue;
        }

        if( next->start > pts_expected + 3 * pv->job->vrate_base / 300 / 2 )
        {
            /* We'll need the current frame more than one time. Make a
               copy of it and keep it */
            buf_tmp = hb_buffer_init( cur->size );
            memcpy( buf_tmp->data, cur->data, cur->size );
        }
        else
        {
            /* The frame has the expected date and won't have to be
               duplicated, just put it through */
            buf_tmp = cur;
            pv->cur = cur = hb_fifo_get( job->fifo_raw );
        }

        /* Replace those MPEG-2 dates with our dates */
        buf_tmp->start = (uint64_t) pv->count_frames *
            pv->job->vrate_base / 300;
        buf_tmp->stop  = (uint64_t) ( pv->count_frames + 1 ) *
            pv->job->vrate_base / 300;

        /* If we have a subtitle for this picture, copy it */
        /* FIXME: we should avoid this memcpy */
        if( sub )
        {
            buf_tmp->sub         = hb_buffer_init( sub->size );
            buf_tmp->sub->x      = sub->x;
            buf_tmp->sub->y      = sub->y;
            buf_tmp->sub->width  = sub->width;
            buf_tmp->sub->height = sub->height;
            memcpy( buf_tmp->sub->data, sub->data, sub->size );
        }

        /* Push the frame to the renderer */
        hb_fifo_push( job->fifo_sync, buf_tmp );

        /* Update UI */
        UpdateState( w );

        /* Make sure we won't get more frames then expected */
        if( pv->count_frames >= pv->count_frames_max )
        {
            hb_log( "sync: got %lld frames", pv->count_frames );
            pv->done = 1;
            break;
        }
    }

    return HB_WORK_OK;
}

/***********************************************************************
 * SyncAudio
 ***********************************************************************
 * 
 **********************************************************************/
static void SyncAudio( hb_work_object_t * w, int i )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t        * job;
    hb_audio_t      * audio;
    hb_buffer_t     * buf;
    hb_sync_audio_t * sync;

    hb_fifo_t       * fifo;
    int               rate;

    int64_t           pts_expected;
    int64_t           start;

    job    = pv->job;
    sync   = &pv->sync_audio[i];
    audio  = sync->audio;

    if( job->acodec & HB_ACODEC_AC3 )
    {
        fifo = audio->fifo_out;
        rate = audio->rate;
    }
    else
    {
        fifo = audio->fifo_sync;
        rate = job->arate;
    }

    while( !hb_fifo_is_full( fifo ) &&
           ( buf = hb_fifo_see( audio->fifo_raw ) ) )
    {
        /* The PTS of the samples we are expecting now */
        pts_expected = pv->pts_offset + sync->count_frames * 90000 / rate;

        if( ( buf->start > pts_expected + PTS_DISCONTINUITY_TOLERANCE ||
              buf->start < pts_expected - PTS_DISCONTINUITY_TOLERANCE ) &&
            pv->pts_offset_old > INT64_MIN )
        {
            /* There has been a PTS discontinuity, and this frame might
               be from before the discontinuity */
            pts_expected = pv->pts_offset_old + sync->count_frames *
                90000 / rate;

            if( buf->start > pts_expected + PTS_DISCONTINUITY_TOLERANCE ||
                buf->start < pts_expected - PTS_DISCONTINUITY_TOLERANCE )
            {
                /* There is really nothing we can do with it */
                buf = hb_fifo_get( audio->fifo_raw );
                hb_buffer_close( &buf );
                continue;
            }

            /* Use the older offset */
            start = pts_expected - pv->pts_offset_old;
        }
        else
        {
            start = pts_expected - pv->pts_offset;
        }

        /* Tolerance: 100 ms */
        if( buf->start < pts_expected - 9000 )
        {
            /* Late audio, trash it */
            hb_log( "sync: trashing late audio" );
            buf = hb_fifo_get( audio->fifo_raw );
            hb_buffer_close( &buf );
            continue;
        }
        else if( buf->start > pts_expected + 9000 )
        {
            /* Missing audio, send a frame of silence */
            InsertSilence( w, i );
            continue;
        }

        if( job->acodec & HB_ACODEC_AC3 )
        {
            buf        = hb_fifo_get( audio->fifo_raw );
            buf->start = start;
            buf->stop  = start + 90000 * AC3_SAMPLES_PER_FRAME / rate;

            sync->count_frames += AC3_SAMPLES_PER_FRAME;
        }
        else
        {
            hb_buffer_t * buf_raw = hb_fifo_get( audio->fifo_raw );

            int count_in, count_out;

            count_in  = buf_raw->size / 2 / sizeof( float );
            count_out = ( buf_raw->stop - buf_raw->start ) * job->arate / 90000;
            if( buf->start < pts_expected - 1500 )
                count_out--;
            else if( buf->start > pts_expected + 1500 )
                count_out++;

            sync->data.data_in      = (float *) buf_raw->data;
            sync->data.input_frames = count_in;
            sync->data.output_frames = count_out;

            sync->data.src_ratio = (double) sync->data.output_frames /
                                   (double) sync->data.input_frames;

            buf = hb_buffer_init( sync->data.output_frames * 2 *
                                  sizeof( float ) );
            sync->data.data_out = (float *) buf->data;
            if( src_process( sync->state, &sync->data ) )
            {
                /* XXX If this happens, we're screwed */
                hb_log( "sync: src_process failed" );
            }
            hb_buffer_close( &buf_raw );

            buf->size = sync->data.output_frames_gen * 2 * sizeof( float );

            /* Set dates for resampled data */
            buf->start = start;
            buf->stop  = start + sync->data.output_frames_gen *
                            90000 / job->arate;

            sync->count_frames += sync->data.output_frames_gen;
        }

        buf->key = 1;
        hb_fifo_push( fifo, buf );
    }

    if( NeedSilence( w, audio ) )
    {
        InsertSilence( w, i );
    }
}

static int NeedSilence( hb_work_object_t * w, hb_audio_t * audio )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;

    if( hb_fifo_size( audio->fifo_in ) ||
        hb_fifo_size( audio->fifo_raw ) ||
        hb_fifo_size( audio->fifo_sync ) ||
        hb_fifo_size( audio->fifo_out ) )
    {
        /* We have some audio, we are fine */
        return 0;
    }

    /* No audio left in fifos */

    if( hb_thread_has_exited( job->reader ) )
    {
        /* We might miss some audio to complete encoding and muxing
           the video track */
        return 1;
    }

    if( hb_fifo_is_full( job->fifo_mpeg2 ) &&
        hb_fifo_is_full( job->fifo_raw ) &&
        hb_fifo_is_full( job->fifo_sync ) &&
        hb_fifo_is_full( job->fifo_render ) &&
        hb_fifo_is_full( job->fifo_mpeg4 ) )
    {
        /* Too much video and no audio, oh-oh */
        return 1;
    }

    return 0;
}

static void InsertSilence( hb_work_object_t * w, int i )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t        * job;
    hb_sync_audio_t * sync;
    hb_buffer_t     * buf;

    job    = pv->job;
    sync   = &pv->sync_audio[i];

    if( job->acodec & HB_ACODEC_AC3 )
    {
        buf        = hb_buffer_init( sync->ac3_size );
        buf->start = sync->count_frames * 90000 / sync->audio->rate;
        buf->stop  = buf->start + 90000 * AC3_SAMPLES_PER_FRAME /
                     sync->audio->rate;
        memcpy( buf->data, sync->ac3_buf, buf->size );

        hb_log( "sync: adding a silent AC-3 frame for track %x",
                sync->audio->id );
        hb_fifo_push( sync->audio->fifo_out, buf );

        sync->count_frames += AC3_SAMPLES_PER_FRAME;

    }
    else
    {
        buf        = hb_buffer_init( 2 * job->arate / 20 *
                                     sizeof( float ) );
        buf->start = sync->count_frames * 90000 / job->arate;
        buf->stop  = buf->start + 90000 / 20;
        memset( buf->data, 0, buf->size );

        hb_log( "sync: adding 50 ms of silence for track %x",
                sync->audio->id );
        hb_fifo_push( sync->audio->fifo_sync, buf );

        sync->count_frames += job->arate / 20;
    }
}

static void UpdateState( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_state_t state;

    if( !pv->count_frames )
    {
        pv->st_first = hb_get_date();
    }
    pv->count_frames++;

    if( hb_get_date() > pv->st_dates[3] + 1000 )
    {
        memmove( &pv->st_dates[0], &pv->st_dates[1],
                 3 * sizeof( uint64_t ) );
        memmove( &pv->st_counts[0], &pv->st_counts[1],
                 3 * sizeof( uint64_t ) );
        pv->st_dates[3]  = hb_get_date();
        pv->st_counts[3] = pv->count_frames;
    } 

#define p state.param.working
    state.state = HB_STATE_WORKING;
    p.progress  = (float) pv->count_frames / (float) pv->count_frames_max;
    if( p.progress > 1.0 )
    {
        p.progress = 1.0; 
    }
    p.rate_cur   = 1000.0 *
        (float) ( pv->st_counts[3] - pv->st_counts[0] ) /
        (float) ( pv->st_dates[3] - pv->st_dates[0] );
    if( hb_get_date() > pv->st_first + 4000 )
    {
        int eta;
        p.rate_avg = 1000.0 * (float) pv->st_counts[3] /
            (float) ( pv->st_dates[3] - pv->st_first );
        eta = (float) ( pv->count_frames_max - pv->st_counts[3] ) /
            p.rate_avg;
        p.hours   = eta / 3600;
        p.minutes = ( eta % 3600 ) / 60;
        p.seconds = eta % 60;
    }
    else
    {
        p.rate_avg = 0.0;
        p.hours    = -1;
        p.minutes  = -1;
        p.seconds  = -1;
    }
#undef p

    hb_set_state( pv->job->h, &state );
}
