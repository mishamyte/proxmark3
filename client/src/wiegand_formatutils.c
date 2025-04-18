//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Wiegand card format packing/unpacking support functions
//-----------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wiegand_formatutils.h"
#include "ui.h"

uint8_t get_bit_by_position(wiegand_message_t *data, uint8_t pos) {
    if (pos >= data->Length) return false;
    pos = (data->Length - pos) - 1; // invert ordering; Indexing goes from 0 to 1. Subtract 1 for weight of bit.
    uint8_t result = 0;
    if (pos > 95)
        result = 0;
    else if (pos > 63)
        result = (data->Top >> (pos - 64)) & 1;
    else if (pos > 31)
        result = (data->Mid >> (pos - 32)) & 1;
    else
        result = (data->Bot >> pos) & 1;
    return result;
}
bool set_bit_by_position(wiegand_message_t *data, bool value, uint8_t pos) {
    if (pos >= data->Length) return false;
    pos = (data->Length - pos) - 1; // invert ordering; Indexing goes from 0 to 1. Subtract 1 for weight of bit.
    if (pos > 95) {
        return false;
    } else if (pos > 63) {
        if (value)
            data->Top |= (1UL << (pos - 64));
        else
            data->Top &= ~(1UL << (pos - 64));
        return true;
    } else if (pos > 31) {
        if (value)
            data->Mid |= (1UL << (pos - 32));
        else
            data->Mid &= ~(1UL << (pos - 32));
        return true;
    } else {
        if (value)
            data->Bot |= (1UL << pos);
        else
            data->Bot &= ~(1UL << pos);
        return true;
    }
}
/**
 * Safeguard the data by doing a manual deep copy
 *
 * At the time of the initial writing, the struct does not contain pointers. That doesn't
 * mean it won't eventually contain one, however. To prevent memory leaks and erroneous
 * aliasing, perform the copy function manually instead. Hence, this function.
 *
 * If the definition of the wiegand_message struct changes, this function must also
 * be updated to match.
 */
static void message_datacopy(wiegand_message_t *src, wiegand_message_t *dest) {
    dest->Bot = src->Bot;
    dest->Mid = src->Mid;
    dest->Top = src->Top;
    dest->Length = src->Length;
}
/**
 *
 * Yes, this is horribly inefficient for linear data.
 * The current code is a temporary measure to have a working function in place
 * until all the bugs shaken from the block/chunk version of the code.
 *
 */
uint64_t get_linear_field(wiegand_message_t *data, uint8_t firstBit, uint8_t length) {
    uint64_t result = 0;
    for (uint8_t i = 0; i < length; i++) {
        result = (result << 1) | get_bit_by_position(data, firstBit + i);
    }
    return result;
}
bool set_linear_field(wiegand_message_t *data, uint64_t value, uint8_t firstBit, uint8_t length) {
    wiegand_message_t tmpdata;
    message_datacopy(data, &tmpdata);
    bool result = true;
    for (int i = 0; i < length; i++) {
        result &= set_bit_by_position(&tmpdata, (value >> ((length - i) - 1)) & 1, firstBit + i);
    }
    if (result)
        message_datacopy(&tmpdata, data);

    return result;
}

uint64_t get_nonlinear_field(wiegand_message_t *data, uint8_t numBits, uint8_t *bits) {
    uint64_t result = 0;
    for (int i = 0; i < numBits; i++) {
        result = (result << 1) | get_bit_by_position(data, *(bits + i));
    }
    return result;
}
bool set_nonlinear_field(wiegand_message_t *data, uint64_t value, uint8_t numBits, uint8_t *bits) {

    wiegand_message_t tmpdata;
    message_datacopy(data, &tmpdata);

    bool result = true;
    for (int i = 0; i < numBits; i++) {
        result &= set_bit_by_position(&tmpdata, (value >> ((numBits - i) - 1)) & 1, *(bits + i));
    }

    if (result)
        message_datacopy(&tmpdata, data);

    return result;
}

uint8_t get_length_from_header(wiegand_message_t *data) {
    /**
     * detect if message has "preamble" / "sentinel bit"
     * Right now we just calculate the highest bit set
     *
     * (from http://www.proxmark.org/forum/viewtopic.php?pid=5368#p5368)
     * 0000 0010 0000 0000 01xx xxxx xxxx xxxx xxxx xxxx xxxx  26-bit
     * 0000 0010 0000 0000 1xxx xxxx xxxx xxxx xxxx xxxx xxxx  27-bit
     * 0000 0010 0000 0001 xxxx xxxx xxxx xxxx xxxx xxxx xxxx  28-bit
     * 0000 0010 0000 001x xxxx xxxx xxxx xxxx xxxx xxxx xxxx  29-bit
     * 0000 0010 0000 01xx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  30-bit
     * 0000 0010 0000 1xxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  31-bit
     * 0000 0010 0001 xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  32-bit
     * 0000 0010 001x xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  33-bit
     * 0000 0010 01xx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  34-bit
     * 0000 0010 1xxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  35-bit
     * 0000 0011 xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  36-bit
     * 0000 000x xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  37-bit
     * 0000 00xx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx  38-bit
     */
    uint8_t len = 0;
    uint32_t hfmt = 0; // for calculating card length

    if ((data->Top & 0x000FFFFF) > 0) { // > 64 bits
        hfmt = data->Top & 0x000FFFFF;
        len = 64;
    } else if (data->Mid & 0xFFFFFFC0) { // handle 38bit and above format
        hfmt = data->Mid;
        len = 31; // remove leading 1 (preamble) in 38-64 bits format
    } else if (((data->Mid >> 5) & 1) == 1) { // bit 38 is set => 26-36bit format
        hfmt = (((data->Mid & 31) << 6) | (data->Bot >> 26)); // get bits 27-37 to check for format len bit
        len = 25;
    } else { // if bit 38 is not set => 37bit format
        hfmt = 0;
        len = 37;
    }

    while (hfmt > 0) {
        hfmt >>= 1;
        len++;
    }

    return len;
}

wiegand_message_t initialize_message_object(uint32_t top, uint32_t mid, uint32_t bot, int n) {
    wiegand_message_t result;
    memset(&result, 0, sizeof(wiegand_message_t));

    result.Top = top;
    result.Mid = mid;
    result.Bot = bot;
    if (n > 0)
        result.Length = n;
    else
        result.Length = get_length_from_header(&result);
    return result;
}

bool add_HID_header(wiegand_message_t *data) {
    // Invalid value
    if (data->Length > 84 || data->Length == 0) {
        return false;
    }

    if (data->Length == 48) {
        data->Mid |= 1U << (data->Length - 32); // Example leading 1: start bit
        return true;
    }

    if (data->Length >= 64) {
        data->Top |= 0x09e00000; // Extended-length header
        data->Top |= 1U << (data->Length - 64); // leading 1: start bit
    } else if (data->Length > 37) {
        data->Top |= 0x09e00000; // Extended-length header
        data->Mid |= 1U << (data->Length - 32); // leading 1: start bit
    } else if (data->Length == 37) {
        // No header bits added to 37-bit cards
    } else if (data->Length >= 32) {
        data->Mid |= 0x20; // Bit 37; standard header
        data->Mid |= 1U << (data->Length - 32); // leading 1: start bit
    } else {
        data->Mid |= 0x20; // Bit 37; standard header
        data->Bot |= 1U << data->Length; // leading 1: start bit
    }
    return true;
}
