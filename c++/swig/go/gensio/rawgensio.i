//
//  gensio - A library for abstracting stream I/O
//  Copyright (C) 2021  Corey Minyard <minyard@acm.org>
//
//  SPDX-License-Identifier: LGPL-2.1-only

// This is the go-specific raw gensio wrapper

%module(directors="1") gensio

%go_import("reflect")

// Renaming gensio_time doesn't, as the base code renames it.  Just
// live with it.
%rename(rawOs_Funcs) gensio::Os_Funcs;
%rename("raw%s") gensio::Os_Funcs::log;
%rename(rawGensio) gensio::Gensio;
%rename(rawSerial_Gensio) gensio::Serial_Gensio;
%rename(rawAccepter) gensio::Accepter;
%rename(rawEvent) gensio::Event;
%rename(rawSerial_Event) gensio::Serial_Event;
%rename(rawGensio_Open_Done) gensio::Gensio_Open_Done;
%rename(rawGensio_Close_Done) gensio::Gensio_Close_Done;
%rename(rawSerial_Op_Done) gensio::Serial_Op_Done;
%rename(rawSerial_Op_Sig_Done) gensio::Serial_Op_Sig_Done;
%rename(rawAccepter_Event) gensio::Accepter_Event;
%rename(rawAccepter_Shutdown_Done) gensio::Accepter_Shutdown_Done;
%rename(rawAccepter_Enable_Done) gensio::Accepter_Enable_Done;
%rename(rawWaiter) gensio::Waiter;
%rename(rawWait) gensio::Waiter::wait;
%rename(rawgensio_alloc) gensio::gensio_alloc;
%rename(rawgensio_acc_alloc) gensio::gensio_acc_alloc;
%rename(RawMDNS_Free_Done) gensio::MDNS_Free_Done;
%rename(RawMDNS) gensio::MDNS;
%rename(RawMDNS_Service) gensio::MDNS_Service;
%rename(RawMDNS_Watch_Free_Done) gensio::MDNS_Watch_Free_Done;
%rename(RawMDNS_Watch_Event) gensio::MDNS_Watch_Event;
%rename(RawMDNS_Watch) gensio::MDNS_Watch;

%ignore gensio::MDNS::add_service;
%ignore gensio::MDNS::add_watch;

%include <std_string.i>
%include <gensio_base.i>

// We use the simple uchar vector for go
%ignore gensio::Gensio::write(const std::vector<unsigned char> data,
			      const char *const *auxdata);
%ignore gensio::Gensio::read_s(std::vector<unsigned char> &rvec,
			       gensio_time *timeout = NULL, bool intr = false);

// We do our own version of read_s that returns the new length.
%ignore gensio::Gensio::read_s(SimpleUCharVector &data,
			       gensio_time *timeout = NULL, bool intr = false);
%ignore gensio::Gensio::control(int depth, bool get, unsigned int option,
				char *data, gensiods *datalen);
%ignore gensio::Accepter::control(int depth, bool get, unsigned int option,
				  char *data, gensiods *datalen);

// Convert betwen byte arrays and unsigned char vectors.
%typemap(gotype) (std::vector<unsigned char>) "[]byte";
%typemap(in) (std::vector<unsigned char>) {
    $1.assign((unsigned char *) $input.array,
	      ((unsigned char *) $input.array) + $input.len);
}
%typemap(directorin) (std::vector<unsigned char>) {
    $input.array = (void *) $1.data();
    $input.len = $1.size();
    $input.cap = $input.len;
}
%typemap(gotype) (gensio::SimpleUCharVector) "[]byte";
%typemap(in) (gensio::SimpleUCharVector) {
    $1.setbuf((unsigned char *) $input.array, $input.len);
}
%typemap(directorin) (gensio::SimpleUCharVector) {
    $input.array = (void *) $1.data();
    $input.len = $1.size();
    $input.cap = $input.len;
}

%typemap(gotype) (std::vector<unsigned char> &) "*[]byte";
%typemap(directorin) (std::vector<unsigned char> &) (_goslice_ temp) {
    $input = (std::vector<unsigned char> *) &temp;
    temp.array = (void *) $1.data();
    temp.len = $1.size();
    temp.cap = temp.len;
}
%typemap(directorargout) (std::vector<unsigned char> &) {
    _goslice_ *tmp = (_goslice_ *) $input;
    $1.assign((unsigned char *) tmp->array,
	      ((unsigned char *) tmp->array) + tmp->len);
}

%typemap(directorin) std::string &retval (_gostring_ temp) {
    $input = (std::string *) &temp;
    temp.p = (char *) $1.data();
    temp.n = $1.size();
}
%typemap(directorargout) std::string &retval {
    _gostring_ *tmp = (_gostring_ *) $input;
    $1.assign(tmp->p, tmp->p + tmp->n);
}

%typemap(directorin) gensio::Gensio *tmpg {
    $input = $1;
    gensio_ref($1->get_gensio());
}

// Return data from read_s.  You can't update an existing vector's
// (well, you can, but it's ugly), but you can update the data.  So
// instead, create our own read_s function that takes the data and
// returns a length, and on the go wrapper side you can slice and
// return the data based on the return length.
%typemap(gotype) (gensio::SimpleUCharVector &data) "[]byte";
%typemap(in) (gensio::SimpleUCharVector &data) (gensio::SimpleUCharVector temp) {
    $1 = &temp;
    $1->setbuf((unsigned char *) $input.array, $input.cap);
}
%extend gensio::Gensio {
    int read_s(SimpleUCharVector &data, gensiods *rlen,
	       gensio_time *timeout = NULL, bool intr = false) {
	int rv = self->read_s(data, timeout, intr);
	*rlen = data.size();
	return rv;
    }
}

// Data handling for control
%typemap(gotype) (char *data, gensiods len) "[]byte";
%typemap(in) (char *data, gensiods len) {
    $1 = (char *) $input.array;
    $2 = $input.cap;
}
%extend gensio::Gensio {
    int control(int depth, bool get, unsigned int option,
		char *data, gensiods len, gensiods *rlen) {
	int rv = self->control(depth, get, option, data, &len);
	*rlen = len;
	return rv;
    }
    void ref() {
	gensio_ref(self->get_gensio());
    }
}

%extend gensio::Accepter {
    int control(int depth, bool get, unsigned int option,
		char *data, gensiods len, gensiods *rlen) {
	int rv = self->control(depth, get, option, data, &len);
	*rlen = len;
	return rv;
    }
}

// Handle auxdata
%typemap(gotype) (const char * const *) "[]string";
%typemap(in) (const char * const *) {
    unsigned int i;
    _gostring_ *strs;

    if (!$input.array || $input.len == 0) {
	$1 = NULL;
    } else {
	strs = (_gostring_ *) $input.array;
	$1 = (char **) malloc(sizeof(char *) * ($input.len + 1));
	for (i = 0; i < $input.len; i++) {
	    $1[i] = (char *) malloc(strs[i].n + 1);
	    memcpy($1[i], strs[i].p, strs[i].n);
	    $1[i][strs[i].n] = '\0';
	}
	$1[i] = NULL;
    }
}
%typemap(freearg) (const char * const *) {
    unsigned int i;
    if ($1) {
	for (i = 0; $1[i]; i++)
	    free($1[i]);
	free($1);
    }
}
%typemap(directorin) (const char * const *) {
    unsigned int i;
    _gostring_ *strs;

    if (!$1 || $1[0] == NULL) {
	$input.array = NULL;
	$input.len = 0;
    } else {
	for (i = 0; $1[i]; i++)
	    ;
	$input.len = i;
	strs = (_gostring_ *) malloc(sizeof(_gostring_) * i);
	for (i = 0; $1[i]; i++) {
	    strs[i].p = (char *) $1[i];
	    strs[i].n = strlen($1[i]);
	}
	$input.array = (void *) strs;
    }
    $input.cap = $input.len;
}
%typemap(directorargout) (const char * const *) {
    // Not actually doing argout, use this for cleanup for the directorin.
    if ($input.array)
	free($input.array);
}

// Make sure a nil gensio time is handled correctly
%typemap(imtype) (gensio_time *) "uintptr"
%typemap(goin) (gensio_time *) {
    if ! reflect.ValueOf($input).IsValid() {
	$result = 0
    } else if reflect.ValueOf($input).IsNil() {
	$result = 0
    } else {
	$result = $input.Swigcptr()
    }
}

%typemap(imtype) (gensio::Os_Funcs_Log_Handler *) "uintptr"
%typemap(goin) (gensio::Os_Funcs_Log_Handler *) {
    if ! reflect.ValueOf($input).IsValid() {
	$result = 0
    } else if reflect.ValueOf($input).IsNil() {
	$result = 0
    } else {
	$result = $input.Swigcptr()
    }
}

%extend gensio::Gensio {
    Serial_Gensio *to_serial_gensio() {
	gensio_ref(self->get_gensio());
	return dynamic_cast<Serial_Gensio *>(self);
    }
}

%extend gensio::Waiter {
    int service(gensio_time *timeout) {
	int err;
	Os_Funcs o = self->get_os_funcs();

	do {
	    err = gensio_os_funcs_service(o, timeout);
	    if (err == GE_INTERRUPTED)
		continue;
	    break;
	} while(true);
	return err;
    }
}

%extend gensio::Os_Funcs {
    void cleanup_mem() {
	gensio_cleanup_mem(*self);
    }
}

%rename("%s") gensio::MDNS::add_watch;
%rename("%s") gensio::MDNS::add_service;

// Take string * for the strings and do the proper translation on the
// go side.
%extend gensio::MDNS {
    MDNS_Service *add_service(int interfacenum, int ipdomain,
			      const std::string *name,
			      const std::string *mtype,
			      const std::string *domain,
			      const std::string *host,
			      int port, const char * const *txt) {
	// string types from go are not nil terminated.
	std::string nname, nmtype, ndomain, nhost;

	if (name) {
	    nname = *name;
	    name = &nname;
	}
	if (mtype) {
	    nmtype = *mtype;
	    mtype = &nmtype;
	}
	if (domain) {
	    ndomain = *domain;
	    domain = &ndomain;
	}
	if (host) {
	    nhost = *host;
	    host = &nhost;
	}
	return self->add_service(interfacenum, ipdomain,
				 name ? name->c_str() : NULL,
				 mtype ? mtype->c_str() : NULL,
				 domain ? domain->c_str() : NULL,
				 host ? host->c_str() : NULL,
				 port, txt);
    }

    MDNS_Watch *add_watch(int interfacenum, int ipdomain,
			  const std::string *name,
			  const std::string *mtype,
			  const std::string *domain,
			  const std::string *host,
			  MDNS_Watch_Event *event)
    {
	// string types from go are not nil terminated.
	std::string nname, nmtype, ndomain, nhost;

	if (name) {
	    nname = *name;
	    name = &nname;
	}
	if (mtype) {
	    nmtype = *mtype;
	    mtype = &nmtype;
	}
	if (domain) {
	    ndomain = *domain;
	    domain = &ndomain;
	}
	if (host) {
	    nhost = *host;
	    host = &nhost;
	}
	return self->add_watch(interfacenum, ipdomain,
			       name ? name->c_str() : NULL,
			       mtype ? mtype->c_str() : NULL,
			       domain ? domain->c_str() : NULL,
			       host ? host->c_str() : NULL,
			       event);
    }
}

%include <gensio/gensio_err.h>
%include <gensio/gensio_control.h>
%include <gensio/gensio>

int gensio_num_alloced();
