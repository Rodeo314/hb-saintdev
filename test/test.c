/* $Id: test.c,v 1.82 2005/11/19 08:25:54 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hb.h"

/* Options */
static int    debug       = HB_DEBUG_NONE;
static int    update      = 0;
static char * input       = NULL;
static char * output      = NULL;
static char * format      = NULL;
static int    titleindex  = 1;
static int    twoPass     = 0;
static int    deinterlace = 0;
static int    grayscale   = 0;
static int    vcodec      = HB_VCODEC_FFMPEG;
static int    h264_13     = 0;
static int    h264_30     = 0;
static char * audios      = NULL;
static int    sub         = 0;
static int    width       = 0;
static int    height      = 0;
static int    crop[4]     = { -1,-1,-1,-1 };
static int    cpu         = 0;
static int    vrate       = 0;
static int    arate       = 0;
static float  vquality    = -1.0;
static int    vbitrate    = 0;
static int    size        = 0;
static int    abitrate    = 0;
static int    mux         = 0;
static int    acodec      = 0;
static int    chapter_start = 0;
static int    chapter_end   = 0;
static int	  crf			= 0;

/* Exit cleanly on Ctrl-C */
static volatile int die = 0;
static void SigHandler( int );

/* Utils */
static void ShowCommands();
static void ShowHelp();
static int  ParseOptions( int argc, char ** argv );
static int  CheckOptions( int argc, char ** argv );
static int  HandleEvents( hb_handle_t * h );

int main( int argc, char ** argv )
{
    hb_handle_t * h;
    int           build;
    char        * version;

    /* Parse command line */
    if( ParseOptions( argc, argv ) ||
        CheckOptions( argc, argv ) )
    {
        return 1;
    }

    /* Init libhb */
    h = hb_init( debug, update );

    /* Show version */
    fprintf( stderr, "HandBrake %s (%d) - http://handbrake.m0k.org/\n",
             hb_get_version( h ), hb_get_build( h ) );

    /* Check for update */
    if( update )
    {
        if( ( build = hb_check_update( h, &version ) ) > -1 )
        {
            fprintf( stderr, "You are using an old version of "
                     "HandBrake.\nLatest is %s (build %d).\n", version,
                     build );
        }
        else
        {
            fprintf( stderr, "Your version of HandBrake is up to "
                     "date.\n" );
        }
        hb_close( &h );
        return 0;
    }

    /* Geeky */
    fprintf( stderr, "%d CPU%s detected\n", hb_get_cpu_count(),
             hb_get_cpu_count( h ) > 1 ? "s" : "" );
    if( cpu )
    {
        fprintf( stderr, "Forcing %d CPU%s\n", cpu,
                 cpu > 1 ? "s" : "" );
        hb_set_cpu_count( h, cpu );
    }

    /* Exit ASAP on Ctrl-C */
    signal( SIGINT, SigHandler );

    /* Feed libhb with a DVD to scan */
    fprintf( stderr, "Opening %s...\n", input );
    hb_scan( h, input, titleindex );

    /* Wait... */
    while( !die )
    {
#if !defined(SYS_BEOS)
        fd_set         fds;
        struct timeval tv;
        int            ret;
        char           buf[257];

        tv.tv_sec  = 0;
        tv.tv_usec = 100000;

        FD_ZERO( &fds );
        FD_SET( STDIN_FILENO, &fds );
        ret = select( STDIN_FILENO + 1, &fds, NULL, NULL, &tv );

        if( ret > 0 )
        {
            int size = 0;

            while( size < 256 &&
                   read( STDIN_FILENO, &buf[size], 1 ) > 0 )
            {
                if( buf[size] == '\n' )
                {
                    break;
                }
                size++;
            }

            if( size >= 256 || buf[size] == '\n' )
            {
                switch( buf[0] )
                {
                    case 'q':
                        die = 1;
                        break;
                    case 'p':
                        hb_pause( h );
                        break;
                    case 'r':
                        hb_resume( h );
                        break;
                    case 'h':
                        ShowCommands();
                        break;
                }
            }
        }
        hb_snooze( 200 );
#else
        hb_snooze( 200 );
#endif

        HandleEvents( h );
    }

    /* Clean up */
    hb_close( &h );
    if( input )  free( input );
    if( output ) free( output );
    if( format ) free( format );
    if( audios ) free( audios );

    fprintf( stderr, "HandBrake has exited.\n" );

    return 0;
}

static void ShowCommands()
{
    fprintf( stderr, "Commands:\n" );
    fprintf( stderr, " [h]elp    Show this message\n" );
    fprintf( stderr, " [q]uit    Exit HBTest\n" );
    fprintf( stderr, " [p]ause   Pause encoding\n" );
    fprintf( stderr, " [r]esume  Resume encoding\n" );
}

static void PrintTitleInfo( hb_title_t * title )
{
    hb_chapter_t  * chapter;
    hb_audio_t    * audio;
    hb_subtitle_t * subtitle;
    int i;

    fprintf( stderr, "+ title %d:\n", title->index );
    fprintf( stderr, "  + vts %d, ttn %d, cells %d->%d (%d blocks)\n",
             title->vts, title->ttn, title->cell_start, title->cell_end,
             title->block_count );
    fprintf( stderr, "  + duration: %02d:%02d:%02d\n",
             title->hours, title->minutes, title->seconds );
    fprintf( stderr, "  + size: %dx%d, aspect: %.2f, %.3f fps\n",
             title->width, title->height,
             (float) title->aspect / HB_ASPECT_BASE,
             (float) title->rate / title->rate_base );
    fprintf( stderr, "  + autocrop: %d/%d/%d/%d\n", title->crop[0],
             title->crop[1], title->crop[2], title->crop[3] );
    fprintf( stderr, "  + chapters:\n" );
    for( i = 0; i < hb_list_count( title->list_chapter ); i++ )
    {
        chapter = hb_list_item( title->list_chapter, i );
        fprintf( stderr, "    + %d: cells %d->%d, %d blocks, duration "
                 "%02d:%02d:%02d\n", chapter->index,
                 chapter->cell_start, chapter->cell_end,
                 chapter->block_count, chapter->hours, chapter->minutes,
                 chapter->seconds );
    }
    fprintf( stderr, "  + audio tracks:\n" );
    for( i = 0; i < hb_list_count( title->list_audio ); i++ )
    {
        audio = hb_list_item( title->list_audio, i );
        if( audio->codec & HB_ACODEC_AC3 )
        {
            fprintf( stderr, "    + %x, %s, %dHz, %dbps\n", audio->id,
                     audio->lang, audio->rate, audio->bitrate );
        }
        else
        {
            fprintf( stderr, "    + %x, %s\n", audio->id, audio->lang );
        }
    }
    fprintf( stderr, "  + subtitle tracks:\n" );
    for( i = 0; i < hb_list_count( title->list_subtitle ); i++ )
    {
        subtitle = hb_list_item( title->list_subtitle, i );
        fprintf( stderr, "    + %x, %s\n", subtitle->id, subtitle->lang );
    }
}

static int HandleEvents( hb_handle_t * h )
{
    hb_state_t s;
    hb_get_state( h, &s );
    switch( s.state )
    {
        case HB_STATE_IDLE:
            /* Nothing to do */
            break;

#define p s.param.scanning
        case HB_STATE_SCANNING:
            /* Show what title is currently being scanned */
            fprintf( stderr, "Scanning title %d", p.title_cur );
            if( !titleindex )
                fprintf( stderr, " of %d", p.title_count );
            fprintf( stderr, "...\n" );
            break;
#undef p

        case HB_STATE_SCANDONE:
        {
            hb_list_t  * list;
            hb_title_t * title;
            hb_job_t   * job;

            list = hb_get_titles( h );

            if( !hb_list_count( list ) )
            {
                /* No valid title, stop right there */
                fprintf( stderr, "No title found.\n" );
                die = 1;
                break;
            }
            if( !titleindex )
            {
                /* Scan-only mode, print infos and exit */
                int i;
                for( i = 0; i < hb_list_count( list ); i++ )
                {
                    title = hb_list_item( list, i );
                    PrintTitleInfo( title );
                }
                die = 1;
                break;
            }

            /* Set job settings */
            title = hb_list_item( list, 0 );
            job   = title->job;

            PrintTitleInfo( title );

            if( chapter_start && chapter_end )
            {
                job->chapter_start = MAX( job->chapter_start,
                                          chapter_start );
                job->chapter_end   = MIN( job->chapter_end,
                                          chapter_end );
                job->chapter_end   = MAX( job->chapter_start,
                                          job->chapter_end );
            }

            if( crop[0] >= 0 && crop[1] >= 0 &&
                crop[2] >= 0 && crop[3] >= 0 )
            {
                memcpy( job->crop, crop, 4 * sizeof( int ) );
            }

            job->deinterlace = deinterlace;
            job->grayscale   = grayscale;

            if( width && height )
            {
                job->width  = width;
                job->height = height;
            }
            else if( width )
            {
                job->width = width;
                hb_fix_aspect( job, HB_KEEP_WIDTH );
            }
            else if( height )
            {
                job->height = height;
                hb_fix_aspect( job, HB_KEEP_HEIGHT );
            }
            else
            {
                hb_fix_aspect( job, HB_KEEP_WIDTH );
            }

            if( vquality >= 0.0 && vquality <= 1.0 )
            {
                job->vquality = vquality;
                job->vbitrate = 0;
            }
            else if( vbitrate )
            {
                job->vquality = -1.0;
                job->vbitrate = vbitrate;
            }
            if( vcodec )
            {
                job->vcodec = vcodec;
            }
            if( h264_13 ) 
            { 
                job->h264_level = 13; 
            }
	    if( h264_30 )
	    {
	        job->h264_level = 30;
            }
            if( vrate )
            {
                job->vrate = 27000000;
                job->vrate_base = vrate;
            }
            if( arate )
            {
                job->arate = arate;
            }

            if( audios )
            {
                if( strcasecmp( audios, "none" ) )
                {
                    int    audio_count = 0;
                    char * tmp         = audios;
                    while( *tmp )
                    {
                        if( *tmp < '0' || *tmp > '9' )
                        {
                            /* Skip non numeric char */
                            tmp++;
                            continue;
                        }
                        job->audios[audio_count++] =
                            strtol( tmp, &tmp, 0 ) - 1;
                    }
                    job->audios[audio_count] = -1;
                }
                else
                {
                    job->audios[0] = -1;
                }
            }
            if( abitrate )
            {
                job->abitrate = abitrate;
            }
            if( acodec )
            {
                job->acodec = acodec;
            }

            if( size )
            {
                job->vbitrate = hb_calc_bitrate( job, size );
                fprintf( stderr, "Calculated bitrate: %d kbps\n",
                         job->vbitrate );
            }
            
            if( sub )
            {
                job->subtitle = sub - 1;
            }

            if( job->mux )
            {
                job->mux = mux;
            }
            job->file = strdup( output );

			if( crf )
			{
				job->crf = 1;
			}

            if( twoPass )
            {
                job->pass = 1;
                hb_add( h, job );
                job->pass = 2;
                hb_add( h, job );
            }
            else
            {
                job->pass = 0;
                hb_add( h, job );
            }
            hb_start( h );
            break;
        }

#define p s.param.working
        case HB_STATE_WORKING:
            fprintf( stderr, "\rEncoding: task %d of %d, %.2f %%",
                     p.job_cur, p.job_count, 100.0 * p.progress );
            if( p.seconds > -1 )
            {
                fprintf( stderr, " (%.2f fps, avg %.2f fps, ETA "
                         "%02dh%02dm%02ds)", p.rate_cur, p.rate_avg,
                         p.hours, p.minutes, p.seconds );
            }
            break;
#undef p

#define p s.param.muxing
        case HB_STATE_MUXING:
        {
            fprintf( stderr, "\rMuxing: %.2f %%", 100.0 * p.progress );
            break;
        }
#undef p

#define p s.param.workdone
        case HB_STATE_WORKDONE:
            /* Print error if any, then exit */
            switch( p.error )
            {
                case HB_ERROR_NONE:
                    fprintf( stderr, "\nRip done!\n" );
                    break;
                case HB_ERROR_CANCELED:
                    fprintf( stderr, "\nRip canceled.\n" );
                    break;
                default:
                    fprintf( stderr, "\nRip failed (error %x).\n",
                             p.error );
            }
            die = 1;
            break;
#undef p
    }
    return 0;
}

/****************************************************************************
 * SigHandler:
 ****************************************************************************/
static volatile int64_t i_die_date = 0;
void SigHandler( int i_signal )
{
    if( die == 0 )
    {
        die = 1;
        i_die_date = hb_get_date();
        fprintf( stderr, "Signal %d received, terminating - do it "
                 "again in case it gets stuck\n", i_signal );
    }
    else if( i_die_date + 500 < hb_get_date() )
    {
        fprintf( stderr, "Dying badly, files might remain in your /tmp\n" );
        exit( 1 );
    }
}

/****************************************************************************
 * ShowHelp:
 ****************************************************************************/
static void ShowHelp()
{
    int i;
    
    fprintf( stderr,
    "Syntax: HBTest [options] -i <device> -o <file>\n"
    "\n"
    "    -h, --help              Print help\n"
    "    -u, --update            Check for updates and exit\n"
    "    -v, --verbose           Be verbose\n"
    "    -C, --cpu               Set CPU count (default: autodetected)\n"
    "\n"
    "    -f, --format <string>   Set output format (avi/mp4/ogm, default:\n"
    "                            autodetected from file name)\n"
    "    -i, --input <string>    Set input device\n"
    "    -o, --output <string>   Set output file name\n"
    "\n"
    "    -t, --title <number>    Select a title to encode (0 to scan only,\n"
    "                            default: 1)\n"
    "    -c, --chapters <string> Select chapters (e.g. \"1-3\" for chapters\n"
    "                            1 to 3, or \"3\" for chapter 3 only,\n"
    "                            default: all chapters)\n"
    "    -a, --audio <string>    Select audio channel(s) (\"none\" for no \n"
    "                            audio, default: first one)\n"
    "\n"
    "    -s, --subtitle <number> Select subtitle (default: none)\n"
    "    -e, --encoder <string>  Set video library encoder (ffmpeg,xvid,\n"
    "                            x264,x264b13,x264b30 default: ffmpeg)\n"
    "    -E, --aencoder <string> Set audio encoder (faac/lame/vorbis/ac3, ac3\n"
    "                            meaning passthrough, default: guessed)\n"
    "    -2, --two-pass          Use two-pass mode\n"
    "    -d, --deinterlace       Deinterlace video\n"
    "    -g, --grayscale         Grayscale encoding\n"
    "\n"
    "    -r, --rate              Set video framerate (" );
    for( i = 0; i < hb_video_rates_count; i++ )
    {
        fprintf( stderr, hb_video_rates[i].string );
        if( i != hb_video_rates_count - 1 )
            fprintf( stderr, "/" );
    }
    fprintf( stderr, ")\n"
    "    -R, --arate             Set audio samplerate (" );
    for( i = 0; i < hb_audio_rates_count; i++ )
    {
        fprintf( stderr, hb_audio_rates[i].string );
        if( i != hb_audio_rates_count - 1 )
            fprintf( stderr, "/" );
    }
    fprintf( stderr, " kHz)\n"
    "    -b, --vb <kb/s>         Set video bitrate (default: 1000)\n"
    "    -q, --quality <float>   Set video quality (0.0..1.0)\n"
	"    -Q, --crf               Use with -q for CRF instead of CQP\n"
    "    -S, --size <MB>         Set target size\n"
    "    -B, --ab <kb/s>         Set audio bitrate (default: 128)\n"
    "    -w, --width <number>    Set picture width\n"
    "    -l, --height <number>   Set picture height\n"
    "        --crop <T:B:L:R>    Set cropping values (default: autocrop)\n" );
}

/****************************************************************************
 * ParseOptions:
 ****************************************************************************/
static int ParseOptions( int argc, char ** argv )
{
    for( ;; )
    {
        static struct option long_options[] =
          {
            { "help",        no_argument,       NULL,    'h' },
            { "update",      no_argument,       NULL,    'u' },
            { "verbose",     no_argument,       NULL,    'v' },
            { "cpu",         required_argument, NULL,    'C' },

            { "format",      required_argument, NULL,    'f' },
            { "input",       required_argument, NULL,    'i' },
            { "output",      required_argument, NULL,    'o' },

            { "title",       required_argument, NULL,    't' },
            { "chapters",    required_argument, NULL,    'c' },
            { "audio",       required_argument, NULL,    'a' },
            { "subtitle",    required_argument, NULL,    's' },

            { "encoder",     required_argument, NULL,    'e' },
            { "aencoder",    required_argument, NULL,    'E' },
            { "two-pass",    no_argument,       NULL,    '2' },
            { "deinterlace", no_argument,       NULL,    'd' },
            { "grayscale",   no_argument,       NULL,    'g' },
            { "width",       required_argument, NULL,    'w' },
            { "height",      required_argument, NULL,    'l' },
            { "crop",        required_argument, NULL,    'n' },

            { "vb",          required_argument, NULL,    'b' },
            { "quality",     required_argument, NULL,    'q' },
            { "size",        required_argument, NULL,    'S' },
            { "ab",          required_argument, NULL,    'B' },
            { "rate",        required_argument, NULL,    'r' },
            { "arate",       required_argument, NULL,    'R' },
			{ "crf",		 no_argument,		NULL,	 'Q' },
			
            { 0, 0, 0, 0 }
          };

        int option_index = 0;
        int c;

        c = getopt_long( argc, argv,
                         "hvuC:f:i:o:t:c:a:s:e:E:2dgw:l:n:b:q:S:B:r:R:Q",
                         long_options, &option_index );
        if( c < 0 )
        {
            break;
        }

        switch( c )
        {
            case 'h':
                ShowHelp();
                exit( 0 );
            case 'u':
                update = 1;
                break;
            case 'v':
                debug = HB_DEBUG_ALL;
                break;
            case 'C':
                cpu = atoi( optarg );
                break;

            case 'f':
                format = strdup( optarg );
                break;
            case 'i':
                input = strdup( optarg );
                break;
            case 'o':
                output = strdup( optarg );
                break;

            case 't':
                titleindex = atoi( optarg );
                break;
            case 'c':
            {
                int start, end;
                if( sscanf( optarg, "%d-%d", &start, &end ) == 2 )
                {
                    chapter_start = start;
                    chapter_end   = end;
                }
                else if( sscanf( optarg, "%d", &start ) == 1 )
                {
                    chapter_start = start;
                    chapter_end   = chapter_start;
                }
                else
                {
                    fprintf( stderr, "chapters: invalid syntax (%s)\n",
                             optarg );
                    return -1;
                }
                break;
            }
            case 'a':
                audios = strdup( optarg );
                break;
            case 's':
                sub = atoi( optarg );
                break;

            case '2':
                twoPass = 1;
                break;
            case 'd':
                deinterlace = 1;
                break;
            case 'g':
                grayscale = 1;
                break;
            case 'e':
                if( !strcasecmp( optarg, "ffmpeg" ) )
                {
                    vcodec = HB_VCODEC_FFMPEG;
                }
                else if( !strcasecmp( optarg, "xvid" ) )
                {
                    vcodec = HB_VCODEC_XVID;
                }
                else if( !strcasecmp( optarg, "x264" ) )
                {
                    vcodec = HB_VCODEC_X264;
                }
                else if( !strcasecmp( optarg, "x264b13" ) )
                {
                    vcodec = HB_VCODEC_X264;
                    h264_13 = 1;
                }
		else if( !strcasecmp( optarg, "x264b30" ) )
		{
		    vcodec = HB_VCODEC_X264;
		    h264_30 = 1;
		}
                else
                {
                    fprintf( stderr, "invalid codec (%s)\n", optarg );
                    return -1;
                }
                break;
            case 'E':
                if( !strcasecmp( optarg, "ac3" ) )
                {
                    acodec = HB_ACODEC_AC3;
                }
                else if( !strcasecmp( optarg, "lame" ) )
                {
                    acodec = HB_ACODEC_LAME;
                }
                break;
            case 'w':
                width = atoi( optarg );
                break;
            case 'l':
                height = atoi( optarg );
                break;
            case 'n':
            {
                int    i;
                char * tmp = optarg;
                for( i = 0; i < 4; i++ )
                {
                    if( !*tmp )
                        break;
                    crop[i] = strtol( tmp, &tmp, 0 );
                    tmp++;
                }
                break;
            }
            case 'r':
            {
                int i;
                vrate = 0;
                for( i = 0; i < hb_video_rates_count; i++ )
                {
                    if( !strcmp( optarg, hb_video_rates[i].string ) )
                    {
                        vrate = hb_video_rates[i].rate;
                        break;
                    }
                }
                if( !vrate )
                {
                    fprintf( stderr, "invalid framerate %s\n", optarg );
                }
                break;
            }
            case 'R':
            {
                int i;
                arate = 0;
                for( i = 0; i < hb_audio_rates_count; i++ )
                {
                    if( !strcmp( optarg, hb_audio_rates[i].string ) )
                    {
                        arate = hb_audio_rates[i].rate;
                        break;
                    }
                }
                if( !arate )
                {
                    fprintf( stderr, "invalid framerate %s\n", optarg );
                }
                break;
            }
            case 'b':
                vbitrate = atoi( optarg );
                break;
            case 'q':
                vquality = atof( optarg );
                break;
            case 'S':
                size = atoi( optarg );
                break;
            case 'B':
                abitrate = atoi( optarg );
                break;
			case 'Q':
				crf = 1;
				break;

            default:
                fprintf( stderr, "unknown option (%s)\n", argv[optind] );
                return -1;
        }
    }

    return 0;
}

static int CheckOptions( int argc, char ** argv )
{
    if( update )
    {
        return 0;
    }

    if( input == NULL || *input == '\0' )
    {
        fprintf( stderr, "Missing input device. Run %s --help for "
                 "syntax.\n", argv[0] );
        return 1;
    }

    /* Parse format */
    if( titleindex > 0 )
    {
        if( output == NULL || *output == '\0' )
        {
            fprintf( stderr, "Missing output file name. Run %s --help "
                     "for syntax.\n", argv[0] );
            return 1;
        }

        if( !format )
        {
            char * p = strrchr( output, '.' );

            /* autodetect */
            if( p && !strcasecmp( p, ".avi" ) )
            {
                mux = HB_MUX_AVI;
            }
            else if( p && !strcasecmp( p, ".mp4" ) )
            {
	    	if ( h264_30 == 1 )
                    mux = HB_MUX_IPOD;
		else
		    mux = HB_MUX_MP4;
            }
            else if( p && ( !strcasecmp( p, ".ogm" ) ||
                            !strcasecmp( p, ".ogg" ) ) )
            {
                mux = HB_MUX_OGM;
            }
            else
            {
                fprintf( stderr, "Output format couldn't be guessed "
                         "from file name, using default.\n" );
                return 0;
            }
        }
        else if( !strcasecmp( format, "avi" ) )
        {
            mux = HB_MUX_AVI;
        }
        else if( !strcasecmp( format, "mp4" ) )
        {
	    if ( h264_30 == 1)
	        mux = HB_MUX_IPOD;
            else
	        mux = HB_MUX_MP4;
        }
        else if( !strcasecmp( format, "ogm" ) ||
                 !strcasecmp( format, "ogg" ) )
        {
            mux = HB_MUX_OGM;
        }
        else
        {
            fprintf( stderr, "Invalid output format (%s). Possible "
                     "choices are avi, mp4 and ogm\n.", format );
            return 1;
        }

        if( !acodec )
        {
            if( mux == HB_MUX_MP4 || mux == HB_MUX_IPOD )
            {
                acodec = HB_ACODEC_FAAC;
            }
            else if( mux == HB_MUX_AVI )
            {
                acodec = HB_ACODEC_LAME;
            }
            else if( mux == HB_MUX_OGM )
            {
                acodec = HB_ACODEC_VORBIS;
            }
        }
    }

    return 0;
}

