/*
draughts-tb: A tablebase generator for Brazilian and international draughts.
    Copyright (C) 2026  Matteo Raso

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once

#include <stdint.h>

#ifdef _WIN32
#  ifdef dwtb_EXPORTS
#    define DWTB_API __declspec(dllexport)
#  else
#    define DWTB_API __declspec(dllimport)
#  endif
#else
#  define DWTB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* dwtb_handle;

/*
 * Bitboards use contiguous playable-square bits:
 *
 *   8x8:  bits 0..31 correspond to playable squares 1..32.
 *   10x10: bits 0..49 correspond to playable squares 1..50.
 *
 * turn: 0 = white, 1 = black.
 *
 * Return values:
 *   0 = loss
 *   1 = draw
 *   2 = win
 *   3 = unknown / unavailable
 *  -1 = API error
 */
DWTB_API dwtb_handle dwtb_open(
    const char* directory,
    int board_size,
    unsigned cache_mib);

DWTB_API int dwtb_probe(
    dwtb_handle handle,
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    int turn);

DWTB_API const char* dwtb_last_error(dwtb_handle handle);

DWTB_API void dwtb_close(dwtb_handle handle);

#ifdef __cplusplus
}
#endif
