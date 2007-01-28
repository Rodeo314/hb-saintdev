/* $Id: enclame.c,v 1.9 2005/03/05 14:27:05 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include "hb.h"

#include "lame/lame.h"

int  enclameInit( hb_work_object_t *, hb_job_t * );
int  enclameWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void enclameClose( hb_work_object_t * );

hb_work_object_t hb_enclame =
{
    WORK_ENCLAME,
    "MP3 encoder (libmp3lame)",
    enclameInit,
    enclameWork,
    enclameClose
};

struct hb_work_private_s
{
    hb_job_t   * job;

    /* LAME handle */
    lame_global_flags * lame;

    unsigned long   input_samples;
    unsigned long   output_bytes;
    uint8_t       * buf;

    hb_list_t     * list;
    int64_t         pts;
};

int enclameInit( hb_work_object_t * w, hb_job_t * job )
{
    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job   = job;

    hb_log( "enclame: opening libmp3lame" );

    pv->lame = lame_init();
    lame_set_brate( pv->lame, job->abitrate );
    lame_set_in_samplerate( pv->lame, job->arate );
    lame_set_out_samplerate( pv->lame, job->arate );
    lame_init_params( pv->lame );
    
    pv->input_samples = 1152 * 2;
    pv->output_bytes = LAME_MAXMP3BUFFER;
    pv->buf  = malloc( pv->input_samples * sizeof( float ) );

    pv->list = hb_list_init();
    pv->pts  = -1;

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
void enclameClose( hb_work_object_t * w )
{
}

/***********************************************************************
 * Encode
 ***********************************************************************
 *
 **********************************************************************/
static hb_buffer_t * Encode( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * buf;
    int16_t samples_s16[1152 * 2];
    uint64_t pts;
    int      pos, i;

    if( hb_list_bytes( pv->list ) < pv->input_samples * sizeof( float ) )
    {
        return NULL;
    }

    hb_list_getbytes( pv->list, pv->buf, pv->input_samples * sizeof( float ),
                      &pts, &pos);

    for( i = 0; i < 1152 * 2; i++ )
    {
        samples_s16[i] = ((float*) pv->buf)[i];
    }

    buf        = hb_buffer_init( pv->output_bytes );
    buf->start = pts + 90000 * pos / 2 / sizeof( float ) / pv->job->arate;
    buf->stop  = buf->start + 90000 * 1152 / pv->job->arate;
    buf->size  = lame_encode_buffer_interleaved( pv->lame, samples_s16,
            1152, buf->data, LAME_MAXMP3BUFFER );
    buf->key   = 1;

    if( !buf->size )
    {
        /* Encoding was successful but we got no data. Try to encode
           more */
        hb_buffer_close( &buf );
        return Encode( w );
    }
    else if( buf->size < 0 )
    {
        hb_log( "enclame: lame_encode_buffer failed" );
        hb_buffer_close( &buf );
        return NULL;
    }

    return buf;
}

/***********************************************************************
 * Work
 ***********************************************************************
 *
 **********************************************************************/
int enclameWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                 hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * buf;

    hb_list_add( pv->list, *buf_in );
    *buf_in = NULL;

    *buf_out = buf = Encode( w );

    while( buf )
    {
        buf->next = Encode( w );
        buf       = buf->next;
    }

    return HB_WORK_OK;
}

