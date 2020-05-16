/*
 *  ser_ioinfo - A program for connecting gensios.
 *  Copyright (C) 2019  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * This provides special handling for serial gensios.
 */

#ifndef SER_IOINFO_H
#define SER_IOINFO_H

#include "ioinfo.h"

/*
 * Allocate a serial gensio sub-handler.  The signature is the
 * signature value to provide to the other end if this is a
 * server-side serial gensio (only used for RFC2217).
 *
 * This return the subhandlers to use in sh, and returns the
 * subhandler data.  If it return NULL, memory could not be allocated.
 *
 * The escape handling is as follows:
 *
 * d - Dump serial data for the other gensio.  Ignored if the other
 * gensio is not a serial gensio.
 *
 * s - Set the serial port (baud) rate for the other gensio.  Ignored
 * if the other gensio is not a serial gensio.  After this, the serial
 * port speed must be typed, terminated by a new line.  Invalid speeds
 * are ignored, use escchar-d to know if you set it right.
 *
 * n, o, e - Set the parity on the other gensio to none, odd, or even.
 * Ignored if the other gensio is not a serial gensio.
 *
 * 7, 8 - Set the data size on the other gensio to 7 or 8 bits.
 * Ignored if the other gensio is not a serial gensio.
 *
 * 1, 2 - Set the number of stop bits to 1 or 2 on the other gensio
 * bits.  Ignored if the other gensio is not a serial gensio.
 */
void *alloc_ser_ioinfo(struct gensio_os_funcs *o,
		       const char *signature,
		       struct ioinfo_sub_handlers **sh);

/* Free the serial gensio sub-handler. */
void free_ser_ioinfo(void *subdata);

#endif /* SER_IOINFO_H */
