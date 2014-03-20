/* qsv_utils.c
 *
 * Copyright (c) 2003-2014 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifdef USE_QSV

#include <stdint.h>
#include <string.h>

#include "qsv_utils.h"

size_t hb_nal_unit_write_annexb(uint8_t *buf,
                                const uint8_t *nal_unit, size_t nal_unit_size)
{
    if (buf != NULL)
    {
        memcpy(buf, hb_annexb_startcode, sizeof(hb_annexb_startcode));
        memcpy(buf + sizeof(hb_annexb_startcode), nal_unit, nal_unit_size);
    }

    return sizeof(hb_annexb_startcode) + nal_unit_size;
}

size_t hb_nal_unit_write_isomp4(uint8_t *buf,
                                const uint8_t *nal_unit, size_t nal_unit_size)
{
    uint32_t nalu_length = nal_unit_size; // 4-byte length replaces startcode

    if (buf != NULL)
    {
        memcpy(buf, &nalu_length, sizeof(nalu_length));
        memcpy(buf + sizeof(nalu_length), nal_unit, nal_unit_size);
    }

    return sizeof(nalu_length) + nal_unit_size;
}

uint8_t* hb_annexb_find_next_nalu(uint8_t *start, size_t *size)
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
    return  nal;
}

#endif // USE_QSV
