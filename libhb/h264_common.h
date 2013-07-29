/* h264_common.h

   Copyright (c) 2003-2012 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HB_H264_COMMON_H
#define HB_H264_COMMON_H

#include "x264.h"
#ifdef USE_QSV
#include "msdk/mfxvideo.h"
#endif

static const char * const hb_h264_profile_names[] = { "auto", "high", "main", "baseline", NULL, };
static const char * const   hb_h264_level_names[] = { "auto", "1.0", "1b", "1.1", "1.2", "1.3", "2.0", "2.1", "2.2", "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2",  NULL, };
static const int    const  hb_h264_level_values[] = {     -1,    10,    9,    11,    12,    13,    20,    21,    22,    30,    31,    32,    40,    41,    42,    50,    51,    52,     0, };
#ifdef USE_QSV
static const int const hb_qsv_h264_level_values[] =
{
    MFX_LEVEL_UNKNOWN, // "auto"
    MFX_LEVEL_AVC_1,
    MFX_LEVEL_AVC_1b,
    MFX_LEVEL_AVC_11,
    MFX_LEVEL_AVC_12,
    MFX_LEVEL_AVC_13,
    MFX_LEVEL_AVC_2,
    MFX_LEVEL_AVC_21,
    MFX_LEVEL_AVC_22,
    MFX_LEVEL_AVC_3,
    MFX_LEVEL_AVC_31,
    MFX_LEVEL_AVC_32,
    MFX_LEVEL_AVC_4,
    MFX_LEVEL_AVC_41,
    MFX_LEVEL_AVC_42,
    MFX_LEVEL_AVC_5,
    MFX_LEVEL_AVC_51,
    MFX_LEVEL_AVC_52,
    MFX_LEVEL_UNKNOWN, // out of bounds
};

// as per
// #include "set.h"
enum profile_e
{
    PROFILE_BASELINE = 66,
    PROFILE_MAIN     = 77,
    PROFILE_HIGH    = 100,
    PROFILE_HIGH10  = 110,
    PROFILE_HIGH422 = 122,
    PROFILE_HIGH444_PREDICTIVE = 244,
};
#endif // USE_QSV

#endif // HB_H264_COMMON_H
