/* $Id: encfaac.c,v 1.13 2005/03/03 17:21:57 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include "hb.h"

#include "faac.h"

struct hb_work_private_s
{
    hb_job_t   * job;

    faacEncHandle * faac;
    unsigned long   input_samples;
    unsigned long   output_bytes;
    uint8_t       * buf;

    hb_list_t     * list;
    int64_t         pts;
};

int  encfaacInit( hb_work_object_t *, hb_job_t * );
int  encfaacWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encfaacClose( hb_work_object_t * );

hb_work_object_t hb_encfaac =
{
    WORK_ENCFAAC,
    "AAC encoder (libfaac)",
    encfaacInit,
    encfaacWork,
    encfaacClose
};

/***********************************************************************
 * hb_work_encfaac_init
 ***********************************************************************
 *
 **********************************************************************/
int encfaacInit( hb_work_object_t * w, hb_job_t * job )
{
    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    faacEncConfigurationPtr cfg;

    pv->job   = job;

    pv->faac = faacEncOpen( job->arate, 2, &pv->input_samples,
                           &pv->output_bytes );
    pv->buf  = malloc( pv->input_samples * sizeof( float ) );
    
    cfg                = faacEncGetCurrentConfiguration( pv->faac );
    cfg->mpegVersion   = MPEG4;
    cfg->aacObjectType = LOW;
    cfg->allowMidside  = 1;
    cfg->useLfe        = 0;
    cfg->useTns        = 0;
    cfg->bitRate       = job->abitrate * 500; /* Per channel */
    cfg->bandWidth     = 0;
    cfg->outputFormat  = 0;
    cfg->inputFormat   =  FAAC_INPUT_FLOAT;
    if( !faacEncSetConfiguration( pv->faac, cfg ) )
    {
        hb_log( "faacEncSetConfiguration failed" );
    }

    uint8_t * bytes;
    unsigned long length;
    if( faacEncGetDecoderSpecificInfo( pv->faac, &bytes, &length ) < 0 )
    {
        hb_log( "faacEncGetDecoderSpecificInfo failed" );
    }
    memcpy( w->config->aac.bytes, bytes, length );
    w->config->aac.length = length;
    free( bytes );

    pv->list = hb_list_init();
    pv->pts  = -1;

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
void encfaacClose( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    faacEncClose( pv->faac );
    free( pv->buf );
    hb_list_empty( &pv->list );
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
    uint64_t      pts;
    int           pos;

    if( hb_list_bytes( pv->list ) < pv->input_samples * sizeof( float ) )
    {
        /* Need more data */
        return NULL;
    }

    hb_list_getbytes( pv->list, pv->buf, pv->input_samples * sizeof( float ),
                      &pts, &pos );

    buf        = hb_buffer_init( pv->output_bytes );
    buf->start = pts + 90000 * pos / 2 / sizeof( float ) / pv->job->arate;
    buf->stop  = buf->start + 90000 * pv->input_samples / pv->job->arate / 2;
    buf->size  = faacEncEncode( pv->faac, (int32_t *) pv->buf,
            pv->input_samples, buf->data, pv->output_bytes );
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
        hb_log( "faacEncEncode failed" );
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
int encfaacWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
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

