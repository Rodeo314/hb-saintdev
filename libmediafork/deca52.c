/* $Id: deca52.c,v 1.14 2005/03/03 17:21:57 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include "mediafork.h"

#include "a52dec/a52.h"

struct hb_work_private_s
{
    hb_job_t    * job;

    /* liba52 handle */
    a52_state_t * state;

    int           flags_in;
    int           flags_out;
    int           rate;
    int           bitrate;
    float         level;
    
    int           error;
    int           sync;
    int           size;

    uint8_t       frame[3840];

    hb_list_t   * list;
};

int  deca52Init( hb_work_object_t *, hb_job_t * );
int  deca52Work( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void deca52Close( hb_work_object_t * );

hb_work_object_t hb_deca52 =
{
    WORK_DECA52,
    "AC3 decoder",
    deca52Init,
    deca52Work,
    deca52Close
};

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static hb_buffer_t * Decode( hb_work_object_t * w );

/***********************************************************************
 * hb_work_deca52_init
 ***********************************************************************
 * Allocate the work object, initialize liba52
 **********************************************************************/
int deca52Init( hb_work_object_t * w, hb_job_t * job )
{
    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job   = job;

    pv->list      = hb_list_init();
    pv->state     = a52_init( 0 );
    pv->flags_out = A52_STEREO;
    pv->level     = 32768.0;

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 * Free memory
 **********************************************************************/
void deca52Close( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    a52_free( pv->state );
}

/***********************************************************************
 * Work
 ***********************************************************************
 * Add the given buffer to the data we already have, and decode as much
 * as we can
 **********************************************************************/
int deca52Work( hb_work_object_t * w, hb_buffer_t ** buf_in,
                hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * buf;

    hb_list_add( pv->list, *buf_in );
    *buf_in = NULL;

    /* If we got more than a frame, chain raw buffers */
    *buf_out = buf = Decode( w );
    while( buf )
    {
        buf->next = Decode( w );
        buf       = buf->next;
    }

    return HB_WORK_OK;
}

/***********************************************************************
 * Decode
 ***********************************************************************
 * 
 **********************************************************************/
static hb_buffer_t * Decode( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t * buf;
    int           i, j;
    uint64_t      pts;
    int           pos;

    /* Get a frame header if don't have one yet */
    if( !pv->sync )
    {
        while( hb_list_bytes( pv->list ) >= 7 )
        {
            /* We have 7 bytes, check if this is a correct header */
            hb_list_seebytes( pv->list, pv->frame, 7 );
            pv->size = a52_syncinfo( pv->frame, &pv->flags_in, &pv->rate,
                                    &pv->bitrate );
            if( pv->size )
            {
                /* It is. W00t. */
                if( pv->error )
                {
                    hb_log( "a52_syncinfo ok" );
                }
                pv->error = 0;
                pv->sync  = 1;
                break;
            }

            /* It is not */
            if( !pv->error )
            {
                hb_log( "a52_syncinfo failed" );
                pv->error = 1;
            }

            /* Try one byte later */
            hb_list_getbytes( pv->list, pv->frame, 1, NULL, NULL );
        }
    }

    if( !pv->sync ||
        hb_list_bytes( pv->list ) < pv->size )
    {
        /* Need more data */
        return NULL;
    }

    /* Get the whole frame */
    hb_list_getbytes( pv->list, pv->frame, pv->size, &pts, &pos );

    /* AC3 passthrough: don't decode the AC3 frame */
    if( pv->job->acodec & HB_ACODEC_AC3 )
    {
        buf = hb_buffer_init( pv->size );
        memcpy( buf->data, pv->frame, pv->size );
        buf->start = pts + ( pos / pv->size ) * 6 * 256 * 90000 / pv->rate;
        buf->stop  = buf->start + 6 * 256 * 90000 / pv->rate;
        pv->sync = 0;
        return buf;
    }

    /* Feed liba52 */
    a52_frame( pv->state, pv->frame, &pv->flags_out, &pv->level, 0 );

    /* 6 blocks per frame, 256 samples per block, 2 channels */
    buf        = hb_buffer_init( 3072 * sizeof( float ) );
    buf->start = pts + ( pos / pv->size ) * 6 * 256 * 90000 / pv->rate;
    buf->stop  = buf->start + 6 * 256 * 90000 / pv->rate;

    for( i = 0; i < 6; i++ )
    {
        sample_t * samples_in;
        float    * samples_out;

        a52_block( pv->state );
        samples_in  = a52_samples( pv->state );
        samples_out = ((float *) buf->data) + 512 * i;

        /* Interleave */
        for( j = 0; j < 256; j++ )
        {
            samples_out[2*j]   = samples_in[j];
            samples_out[2*j+1] = samples_in[256+j];
        }
    }

    pv->sync = 0;
    return buf;
}

