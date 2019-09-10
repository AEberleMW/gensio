/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#ifndef GENSIO_FILTER_MUX_H
#define GENSIO_FILTER_MUX_H

#include <gensio/gensio_base.h>

struct gensio_mux_filter_data;

int gensio_mux_filter_config(struct gensio_os_funcs *o,
			     const char * const args[],
			     bool default_is_client,
			     struct gensio_mux_filter_data **rdata);

void gensio_mux_filter_config_free(struct gensio_mux_filter_data *data);

int gensio_mux_filter_alloc(struct gensio_mux_filter_data *data,
			    struct gensio_filter **rfilter);

#endif /* GENSIO_FILTER_MUX_H */
