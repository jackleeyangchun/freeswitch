/*
 * Copyright (c) 2007, Anthony Minessale II
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "openzap.h"
#include "zap_skel.h"

static ZINT_CONFIGURE_FUNCTION(skel_configure)
{
	ZINT_CONFIGURE_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_OPEN_FUNCTION(skel_open) 
{
	ZINT_OPEN_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_CLOSE_FUNCTION(skel_close)
{
	ZINT_CLOSE_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_SET_CODEC_FUNCTION(skel_set_codec)
{
	ZINT_SET_CODEC_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_SET_INTERVAL_FUNCTION(skel_set_interval)
{
	ZINT_SET_INTERVAL_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_WAIT_FUNCTION(skel_wait)
{
	ZINT_WAIT_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_READ_FUNCTION(skel_read)
{
	ZINT_READ_MUZZLE;
	return ZAP_FAIL;
}

static ZINT_WRITE_FUNCTION(skel_write)
{
	ZINT_WRITE_MUZZLE;
	return ZAP_FAIL;
}

static zap_software_interface_t skel_interface;

zap_status_t skel_init(zap_software_interface_t **zint)
{
	assert(zint != NULL);
	memset(&skel_interface, 0, sizeof(skel_interface));

	skel_interface.name = "skel";
	skel_interface.configure =  skel_configure;
	skel_interface.open = skel_open;
	skel_interface.close = skel_close;
	skel_interface.set_codec = skel_set_codec;
	skel_interface.set_interval = skel_set_interval;
	skel_interface.wait = skel_wait;
	skel_interface.read = skel_read;
	skel_interface.write = skel_write;
	*zint = &skel_interface;

	return ZAP_FAIL;
}

zap_status_t skel_destroy(void)
{
	return ZAP_FAIL;
}
