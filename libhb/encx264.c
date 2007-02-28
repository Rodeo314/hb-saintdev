/* $Id: encx264.c,v 1.21 2005/11/04 13:09:41 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include <stdarg.h>

#include "hb.h"

#include "x264.h"

int  encx264Init( hb_work_object_t *, hb_job_t * );
int  encx264Work( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encx264Close( hb_work_object_t * );

hb_work_object_t hb_encx264 =
{
    WORK_ENCX264,
    "H.264/AVC encoder (libx264)",
    encx264Init,
    encx264Work,
    encx264Close
};

struct hb_work_private_s
{
    hb_job_t       * job;
    x264_t         * x264;
    x264_picture_t   pic_in;
    x264_picture_t   pic_out;

    char             filename[1024];
};

/***********************************************************************
 * hb_work_encx264_init
 ***********************************************************************
 *
 **********************************************************************/
int encx264Init( hb_work_object_t * w, hb_job_t * job )
{
    x264_param_t       param;
    x264_nal_t       * nal;
    int                nal_count;

    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job = job;

    memset( pv->filename, 0, 1024 );
    hb_get_tempory_filename( job->h, pv->filename, "x264.log" );

    x264_param_default( &param );

    param.i_threads    = hb_get_cpu_count();
    param.i_width      = job->width;
    param.i_height     = job->height;
    param.i_fps_num    = job->vrate;
    param.i_fps_den    = job->vrate_base;
    param.i_keyint_max = 20 * job->vrate / job->vrate_base;
    param.i_log_level  = X264_LOG_INFO;
	if( job->h264_level )
	{
	param.i_threads   = 1;
	param.b_cabac     = 0;
	param.i_level_idc = job->h264_level;
	hb_log( "encx264: encoding at level %i",
		param.i_level_idc );
	}

	/* Slightly faster with minimal quality lost */
	param.analyse.i_subpel_refine = 4;

	/* 	This section passes the string x264opts to libx264 for parsing into parameter names and values.

	The string is set up like this:
	option1=value1:option2=value 2

	So, you have to iterate through based on the colons, and then put the left side of the equals sign in "name"
	and the right side into "value." Then you hand those strings off to x264 for interpretation.

	This is all based on the universal x264 option handling Loren Merritt implemented in the Mplayer/Mencoder project.
	*/
 
	char *x264opts = job->x264opts;
	if(x264opts != NULL && *x264opts != '\0')
	{
		while(*x264opts)
		{
	    	char *name = x264opts;
	    	char *value;
	    	int ret;

	    	x264opts += strcspn(x264opts, ":");
	    	if(*x264opts)
	   		{
	       		*x264opts = 0;
	       		x264opts++;
	   		}

	    	value = strchr( name, '=' );
	    	if(value)
	   		{
	        	*value = 0;
	        	value++;
	   		}
		
			/*
				When B-frames are enabled, the max frame count increments by 1 (regardless of the number of B-frames).
				If you don't change the duration of the video track when you mux, libmp4 barfs.
				So, check if the x264opts are using B-frames, and when they are, set the boolean job->areBframes as true.
			*/

			if (!(strcmp(name, "bframes")))
			{
				if (atoi(value) > 0)
				{
					job->areBframes = 1;
				}
			}

			/* 	Here's where the strings are passed to libx264 for parsing. */
	    	ret = x264_param_parse(&param, name, value);
	
			/* 	Let x264 sanity check the options for us*/
	    	if(ret == X264_PARAM_BAD_NAME)
	         	printf("x264 options: Unknown suboption %s\n", name);
	    	if(ret == X264_PARAM_BAD_VALUE)
	       		printf("x264 options: Bad argument %s=%s\n", name, value ? value : "(null)");
		}
	}


	if( job->pixel_ratio )
	{
	    param.vui.i_sar_width = job->pixel_aspect_width;
	    param.vui.i_sar_height = job->pixel_aspect_height;

	    hb_log( "encx264: encoding with stored aspect %d/%d",
	            param.vui.i_sar_width, param.vui.i_sar_height );
	}


	if( job->vquality >= 0.0 && job->vquality <= 1.0 )
	{
	    switch(job->crf)
		{
			case 1:
			/*Constant RF*/
			param.rc.i_rc_method = X264_RC_CRF;
			param.rc.f_rf_constant = 51 - job->vquality * 51;
			hb_log( "encx264: Encoding at constant RF %f",
				param.rc.f_rf_constant );
			break;

			case 0:
			/*Constant QP*/
			param.rc.i_rc_method = X264_RC_CQP;
	    	param.rc.i_qp_constant = 51 - job->vquality * 51;
	    	hb_log( "encx264: encoding at constant QP %d",
	            param.rc.i_qp_constant );
			break;
		}
	}
	else
	{
	/* Rate control */
	    param.rc.i_rc_method = X264_RC_ABR;
	    param.rc.i_bitrate = job->vbitrate;
	    switch( job->pass )
	    {
	        case 1:
	            param.rc.b_stat_write  = 1;
	            param.rc.psz_stat_out = pv->filename;
	            break;
	        case 2:
	            param.rc.b_stat_read = 1;
	            param.rc.psz_stat_in = pv->filename;
	            break;
	    }
	}

    hb_log( "encx264: opening libx264 (pass %d)", job->pass );
    pv->x264 = x264_encoder_open( &param );

    x264_encoder_headers( pv->x264, &nal, &nal_count );

    /* Sequence Parameter Set */
    w->config->h264.sps_length = 1 + nal[1].i_payload;
    w->config->h264.sps[0] = 0x67;
    memcpy( &w->config->h264.sps[1], nal[1].p_payload, nal[1].i_payload );

    /* Picture Parameter Set */
    w->config->h264.pps_length = 1 + nal[2].i_payload;
    w->config->h264.pps[0] = 0x68;
    memcpy( &w->config->h264.pps[1], nal[2].p_payload, nal[2].i_payload );

    x264_picture_alloc( &pv->pic_in, X264_CSP_I420,
                        job->width, job->height );

    return 0;
}

void encx264Close( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    x264_encoder_close( pv->x264 );

    /* TODO */
}

int encx264Work( hb_work_object_t * w, hb_buffer_t ** buf_in,
                  hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t    * job = pv->job;
    hb_buffer_t * in = *buf_in, * buf;
    int           i_nal;
    x264_nal_t  * nal;
    int i;

    /* XXX avoid this memcpy ? */
    memcpy( pv->pic_in.img.plane[0], in->data, job->width * job->height );
    if( job->grayscale )
    {
        /* XXX x264 has currently no option for grayscale encoding */
        memset( pv->pic_in.img.plane[1], 0x80, job->width * job->height / 4 );
        memset( pv->pic_in.img.plane[2], 0x80, job->width * job->height / 4 );
    }
    else
    {
        memcpy( pv->pic_in.img.plane[1], in->data + job->width * job->height,
                job->width * job->height / 4 );
        memcpy( pv->pic_in.img.plane[2], in->data + 5 * job->width *
                job->height / 4, job->width * job->height / 4 );
    }

    pv->pic_in.i_type    = X264_TYPE_AUTO;
    pv->pic_in.i_qpplus1 = 0;

    x264_encoder_encode( pv->x264, &nal, &i_nal,
                         &pv->pic_in, &pv->pic_out );

    /* Should be way too large */
    buf        = hb_buffer_init( 3 * job->width * job->height / 2 );
    buf->size  = 0;
    buf->start = in->start;
    buf->stop  = in->stop;
    buf->key   = 0;

    for( i = 0; i < i_nal; i++ )
    {
        int size, data;

        data = buf->alloc - buf->size;
        if( ( size = x264_nal_encode( buf->data + buf->size, &data,
                                      1, &nal[i] ) ) < 1 )
        {
            continue;
        }

        if( job->mux & HB_MUX_AVI )
        {
            if( nal[i].i_ref_idc == NAL_PRIORITY_HIGHEST )
            {
                buf->key = 1;
            }
            buf->size += size;
            continue;
        }

        /* H.264 in .mp4 */
        switch( buf->data[buf->size+4] & 0x1f )
        {
            case 0x7:
            case 0x8:
                /* SPS, PPS */
                break;

            default:
                /* H.264 in mp4 (stolen from mp4creator) */
                buf->data[buf->size+0] = ( ( size - 4 ) >> 24 ) & 0xFF;
                buf->data[buf->size+1] = ( ( size - 4 ) >> 16 ) & 0xFF;
                buf->data[buf->size+2] = ( ( size - 4 ) >>  8 ) & 0xFF;
                buf->data[buf->size+3] = ( ( size - 4 ) >>  0 ) & 0xFF;
                if( nal[i].i_ref_idc == NAL_PRIORITY_HIGHEST )
                {
                    buf->key = 1;
                }
                buf->size += size;
        }
    }

    *buf_out = buf;

    return HB_WORK_OK;
}


