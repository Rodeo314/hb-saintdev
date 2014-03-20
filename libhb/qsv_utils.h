/* qsv_utils.h
 *
 * Copyright (c) 2003-2014 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HB_QSV_UTILS_H
#define HB_QSV_UTILS_H

static const uint8_t hb_annexb_startcode[] = { 0x00, 0x00, 0x00, 0x01, };

/*
 * Write a NAL unit of the specified size to the provided output buffer.
 * Returns the amount (in bytes) of data written to the buffer.
 *
 * Note: the buffer is assumed to be large enough to hold the NAL unit
 * as well as any additonal data the function may prepend/append to it.
 *
 * The caller may check the minimum required buffer size by passing a
 * NULL buffer to the fucntion and checking the returned size value.
 *
 * hb_nal_unit_write_annexb adds an Annex B start code before the data.
 * hb_nal_unit_write_isomp4 adds a 4-byte length field before the data.
 */
size_t hb_nal_unit_write_annexb(uint8_t *buf, const uint8_t *nal_unit, size_t nal_unit_size);
size_t hb_nal_unit_write_isomp4(uint8_t *buf, const uint8_t *nal_unit, size_t nal_unit_size);

/*
 * Search the provided data buffer for NAL units in Annex B format.
 *
 * Returns a pointer to the start (start code prefix excluded) of the
 * first NAL unit found, or NULL if no NAL units were found in the buffer.
 *
 * On input,  size holds the length of the provided data buffer.
 * On output, size holds the length of the returned NAL unit.
 */
uint8_t* hb_annexb_find_next_nalu(uint8_t *start, size_t *size);

#endif // HB_QSV_UTILS_H
