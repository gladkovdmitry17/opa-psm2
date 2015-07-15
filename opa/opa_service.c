/*

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2015 Intel Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  Contact Information:
  Intel Corporation, www.intel.com

  BSD LICENSE

  Copyright(c) 2015 Intel Corporation.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/* Copyright (c) 2003-2014 Intel Corporation. All rights reserved. */

/* This file contains hfi service routine interface used by the low
   level hfi protocol code. */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <poll.h>

#include "opa_service.h"

/*
 * This function is necessary in a udev-based world.  There can be an
 * arbitrarily long (but typically less than one second) delay between
 * a driver getting loaded and any dynamic special files turning up.
 *
 * The timeout is in milliseconds.  A value of zero means "callee
 * decides timeout".  Negative is infinite.
 *
 * Returns 0 on success, -1 on error or timeout.  Check errno to see
 * whether there was a timeout (ETIMEDOUT) or an error (any other
 * non-zero value).
 */
int hfi_wait_for_device(const char *path, long timeout)
{
	int saved_errno;
	struct stat st;
	long elapsed;
	int ret;

	if (timeout == 0)
		timeout = 15000;

	elapsed = 0;

	while (1) {
		static const long default_ms = 250;
		struct timespec req = { 0 };
		long ms;

		ret = stat(path, &st);
		saved_errno = errno;

		if (ret == 0 || (ret == -1 && errno != ENOENT))
			break;

		if (timeout - elapsed == 0) {
			saved_errno = ETIMEDOUT;
			break;
		}

		if (elapsed == 0) {
			if (timeout == -1)
				_HFI_DBG
				    ("Device file %s not present on first check; "
				     "waiting indefinitely...\n", path);
			else
				_HFI_DBG
				    ("Device file %s not present on first check; "
				     "waiting up to %.1f seconds...\n", path,
				     timeout / 1e3);
		}

		if (timeout < 0 || timeout - elapsed >= default_ms)
			ms = default_ms;
		else
			ms = timeout;

		elapsed += ms;
		req.tv_nsec = ms * 1000000;

		ret = nanosleep(&req, NULL);
		saved_errno = errno;

		if (ret == -1)
			break;
	}

	if (ret == 0)
		_HFI_DBG("Found %s after %.1f seconds\n", path, elapsed / 1e3);
	else
		_HFI_INFO
		    ("The %s device failed to appear after %.1f seconds: %s\n",
		     path, elapsed / 1e3, strerror(saved_errno));

	errno = saved_errno;
	return ret;
}

int hfi_context_open(int unit, int port, uint64_t open_timeout)
{
	int fd;
	char dev_name[MAXPATHLEN];

	if (unit != HFI_UNIT_ID_ANY && unit >= 0)
		snprintf(dev_name, sizeof(dev_name), "%s_%u", HFI_DEVICE_PATH,
			 unit);
	else
		snprintf(dev_name, sizeof(dev_name), "%s", HFI_DEVICE_PATH);

	if (hfi_wait_for_device(dev_name, (long)open_timeout) == -1) {
		_HFI_DBG("Could not find an HFI Unit on device "
			 "%s (%lds elapsed)", dev_name,
			 (long)open_timeout / 1000);
		return -1;
	}

	if ((fd = open(dev_name, O_RDWR)) == -1) {
		_HFI_DBG("(host:Can't open %s for reading and writing",
			 dev_name);
		return -1;
	}

	if (fcntl(fd, F_SETFD, FD_CLOEXEC))
		_HFI_INFO("Failed to set close on exec for device: %s\n",
			  strerror(errno));

	return fd;
}

void hfi_context_close(int fd)
{
	(void)close(fd);
}

int hfi_cmd_writev(int fd, const struct iovec *iov, int iovcnt)
{
	return writev(fd, iov, iovcnt);
}

int hfi_cmd_write(int fd, struct hfi1_cmd *cmd, size_t count)
{
	return write(fd, cmd, count);
}

/* we use mmap64() because we compile in both 32 and 64 bit mode,
   and we have to map physical addresses that are > 32 bits long.
   While linux implements mmap64, it doesn't have a man page,
   and isn't declared in any header file, so we declare it here ourselves.

   We'd like to just use -D_LARGEFILE64_SOURCE, to make off_t 64 bits and
   redirects mmap to mmap64 for us, but at least through suse10 and fc4,
   it doesn't work when the address being mapped is > 32 bits.  It chips
   off bits 32 and above.   So we stay with mmap64. */
void *hfi_mmap64(void *addr, size_t length, int prot, int flags, int fd,
		 __off64_t offset)
{
	return mmap64(addr, length, prot, flags, fd, offset);
}

/* get the number of units supported by the driver.  Does not guarantee */
/* that a working chip has been found for each possible unit #. */
/* number of units >=0 (0 means none found). */
/* formerly used sysfs file "num_units" */
int hfi_get_num_units(void)
{
	int ret;
	char pathname[128];
	struct stat st;

	for (ret = 0;; ret++) {
		snprintf(pathname, sizeof(pathname), HFI_CLASS_PATH "_%d", ret);
		if (stat(pathname, &st) || !S_ISDIR(st.st_mode))
			break;
	}

	return ret;
}

/* get the number of contexts from the unit id. */
/* Returns 0 if no unit or no match. */
int hfi_get_num_contexts(int unit_id)
{
	int n = 0;
	int units;

	units = hfi_get_num_units();
	if (units > 0) {
		int64_t val;
		if (unit_id == HFI_UNIT_ID_ANY) {
			uint32_t u, p;
			for (u = 0; u < units; u++) {
				for (p = 1; p <= HFI_MAX_PORT; p++)
					if (hfi_get_port_lid(u, p) != -1)
						break;
				if (p <= HFI_MAX_PORT &&
				    !hfi_sysfs_unit_read_s64(u, "nctxts", &val,
							     0))
					n += (uint32_t) val;
			}
		} else {
			uint32_t p;
			for (p = 1; p <= HFI_MAX_PORT; p++)
				if (hfi_get_port_lid(unit_id, p) != -1)
					break;
			if (p <= HFI_MAX_PORT &&
			    !hfi_sysfs_unit_read_s64(unit_id, "nctxts", &val,
						     0))
				n += (uint32_t) val;
		}
	}

	return n;
}

/* Given the unit number, return an error, or the corresponding LID
   For now, it's used only so the MPI code can determine it's own
   LID, and which other LIDs (if any) are also assigned to this node
   Returns an int, so -1 indicates an error.  0 may indicate that
   the unit is valid, but no LID has been assigned.
   No error print because we call this for both potential
   ports without knowing if both ports exist (or are connected) */
int hfi_get_port_lid(int unit, int port)
{
	int ret;
	char *state;
	int64_t val;

	ret = hfi_sysfs_port_read(unit, port, "phys_state", &state);
	if (ret == -1) {
		if (errno == ENODEV)
			/* this is "normal" for port != 1, on single port chips */
			_HFI_VDBG
			    ("Failed to get phys_state for unit %u:%u: %s\n",
			     unit, port, strerror(errno));
		else
			_HFI_DBG
			    ("Failed to get phys_state for unit %u:%u: %s\n",
			     unit, port, strerror(errno));
	} else {
		if (strncmp(state, "5: LinkUp", 9)) {
			_HFI_DBG("Link is not Up for unit %u:%u\n", unit, port);
			ret = -1;
		}
		free(state);
	}
	/* If link is not up, we think lid not valid */
	if (ret == -1)
		return ret;

	ret = hfi_sysfs_port_read_s64(unit, port, "lid", &val, 0);
	_HFI_VDBG("hfi_get_port_lid: ret %d, unit %d port %d\n", ret, unit,
		  port);

	if (ret == -1) {
		if (errno == ENODEV)
			/* this is "normal" for port != 1, on single port chips */
			_HFI_VDBG("Failed to get LID for unit %u:%u: %s\n",
				  unit, port, strerror(errno));
		else
			_HFI_DBG("Failed to get LID for unit %u:%u: %s\n",
				 unit, port, strerror(errno));
	} else {
		ret = val;

/* disable this feature since we don't have a way to provide
   file descriptor in multiple context case. */
#if 0
		if (getenv("HFI_DIAG_LID_LOOP")) {
			/* provides diagnostic ability to run MPI, etc. even */
			/* on loopback, by claiming a different LID for each context */
			struct hfi1_ctxt_info info;
			struct hfi1_cmd cmd;
			cmd.type = HFI1_CMD_CTXT_INFO;
			cmd.cmd.ctxt_info = (uintptr_t) &info;
			if (__hfi_lastfd == -1)
				_HFI_INFO
				    ("Can't run CONTEXT_INFO for lid_loop, fd not set\n");
			else if (write(__hfi_lastfd, &cmd, sizeof(cmd)) == -1)
				_HFI_INFO("CONTEXT_INFO command failed: %s\n",
					  strerror(errno));
			else if (!info.context)
				_HFI_INFO("CONTEXT_INFO returned context 0!\n");
			else {
				_HFI_PRDBG
				    ("Using lid 0x%x, base %x, context %x\n",
				     ret + info.context, ret, info.context);
				ret += info.context;
			}
		}
#endif
	}

	return ret;
}

/* Given the unit number, return an error, or the corresponding GID
   For now, it's used only so the MPI code can determine its fabric ID.
   Returns an int, so -1 indicates an error.
   No error print because we call this for both potential
   ports without knowing if both ports exist (or are connected) */
int hfi_get_port_gid(int unit, int port, uint64_t *hi, uint64_t *lo)
{
	int ret;
	char *gid_str = NULL;

	ret = hfi_sysfs_port_read(unit, port, "gids/0", &gid_str);

	if (ret == -1) {
		if (errno == ENODEV)
			/* this is "normal" for port != 1, on single
			 * port chips */
			_HFI_VDBG("Failed to get GID for unit %u:%u: %s\n",
				  unit, port, strerror(errno));
		else
			_HFI_DBG("Failed to get GID for unit %u:%u: %s\n",
				 unit, port, strerror(errno));
	} else {
		int gid[8];
		if (sscanf(gid_str, "%4x:%4x:%4x:%4x:%4x:%4x:%4x:%4x",
			   &gid[0], &gid[1], &gid[2], &gid[3],
			   &gid[4], &gid[5], &gid[6], &gid[7]) != 8) {
			_HFI_DBG("Failed to parse GID for unit %u:%u: %s\n",
				 unit, port, gid_str);
			ret = -1;
		} else {
			*hi = (((uint64_t) gid[0]) << 48) | (((uint64_t) gid[1])
							     << 32) |
			    (((uint64_t)
			      gid[2]) << 16) | (((uint64_t) gid[3]) << 0);
			*lo = (((uint64_t) gid[4]) << 48) | (((uint64_t) gid[5])
							     << 32) |
			    (((uint64_t)
			      gid[6]) << 16) | (((uint64_t) gid[7]) << 0);
		}
		free(gid_str);
	}

	return ret;
}

/* Given the unit number, return an error, or the corresponding LMC value
   for the port */
/* Returns an int, so -1 indicates an error.  0 */
int hfi_get_port_lmc(int unit, int port)
{
	int ret;
	int64_t val;

	ret = hfi_sysfs_port_read_s64(unit, port, "lid_mask_count", &val, 0);

	if (ret == -1) {
		_HFI_INFO("Failed to get LMC for unit %u:%u: %s\n",
			  unit, port, strerror(errno));
	} else
		ret = val;

	return ret;
}

/* Given the unit number, return an error, or the corresponding link rate
   for the port */
/* Returns an int, so -1 indicates an error. */
int hfi_get_port_rate(int unit, int port)
{
	int ret;
	double rate;
	char *data_rate = NULL, *newptr;

	ret = hfi_sysfs_port_read(unit, port, "rate", &data_rate);
	if (ret == -1)
		goto get_port_rate_error;
	else {
		rate = strtod(data_rate, &newptr);
		if ((rate == 0) && (data_rate == newptr))
			goto get_port_rate_error;
	}

	free(data_rate);
	return ((int)(rate * 2) >> 1);

get_port_rate_error:
	_HFI_INFO("Failed to get link rate for unit %u:%u: %s\n",
		  unit, port, strerror(errno));

	return ret;
}

/* Given a unit, port and SL, return an error, or the corresponding SC for the
   SL as programmed by the SM */
/* Returns an int, so -1 indicates an error. */
int hfi_get_port_sl2sc(int unit, int port, int sl)
{
	int ret;
	int64_t val;
	char sl2scpath[16];

	snprintf(sl2scpath, sizeof(sl2scpath), "sl2sc/%d", sl);
	ret = hfi_sysfs_port_read_s64(unit, port, sl2scpath, &val, 0);

	if (ret == -1) {
		_HFI_DBG
		    ("Failed to get SL2SC mapping for SL %d unit %u:%u: %s\n",
		     sl, unit, port, strerror(errno));
	} else
		ret = val;

	return ret;
}

/* Given a unit, port and SC, return an error, or the corresponding VL for the
   SC as programmed by the SM */
/* Returns an int, so -1 indicates an error. */
int hfi_get_port_sc2vl(int unit, int port, int sc)
{
	int ret;
	int64_t val;
	char sc2vlpath[16];

	snprintf(sc2vlpath, sizeof(sc2vlpath), "sc2vl/%d", sc);
	ret = hfi_sysfs_port_read_s64(unit, port, sc2vlpath, &val, 0);

	if (ret == -1) {
		_HFI_DBG
		    ("Failed to get SC2VL mapping for SC %d unit %u:%u: %s\n",
		     sc, unit, port, strerror(errno));
	} else
		ret = val;

	return ret;
}

/* Given a unit, port and VL, return an error, or the corresponding MTU for the
   VL as programmed by the SM */
/* Returns an int, so -1 indicates an error. */
int hfi_get_port_vl2mtu(int unit, int port, int vl)
{
	int ret;
	int64_t val;
	char vl2mtupath[16];

	snprintf(vl2mtupath, sizeof(vl2mtupath), "vl2mtu/%d", vl);
	ret = hfi_sysfs_port_read_s64(unit, port, vl2mtupath, &val, 0);

	if (ret == -1) {
		_HFI_DBG
		    ("Failed to get VL2MTU mapping for VL %d unit %u:%u: %s\n",
		     vl, unit, port, strerror(errno));
	} else
		ret = val;

	return ret;
}

/* Given a unit, port and index, return an error, or the corresponding pkey
   value for the index as programmed by the SM */
/* Returns an int, so -1 indicates an error. */
int hfi_get_port_index2pkey(int unit, int port, int index)
{
	int ret;
	int64_t val;
	char index2pkeypath[16];

	snprintf(index2pkeypath, sizeof(index2pkeypath), "pkeys/%d", index);
	ret = hfi_sysfs_port_read_s64(unit, port, index2pkeypath, &val, 0);

	if (ret == -1) {
		_HFI_DBG
		    ("Failed to get index2pkey mapping for index %d unit %u:%u: %s\n",
		     index, unit, port, strerror(errno));
	} else
		ret = val;

	return ret;
}

/* These have been fixed to read the values, but they are not
 * compatible with the hfi driver, they return new info with
 * the qib driver
 */
static int hfi_count_names(const char *namep)
{
	int n = 0;
	while (*namep != '\0') {
		if (*namep == '\n')
			n++;
		namep++;
	}
	return n;
}

int hfi_get_stats_names(char **namep)
{
	int i;
	i = hfi_hfifs_read("driver_stats_names", namep);
	if (i < 0)
		return -1;
	else
		return hfi_count_names(*namep);
}

int hfi_get_stats(uint64_t *s, int nelem)
{
	int i;
	i = hfi_hfifs_rd("driver_stats", s, nelem * sizeof(*s));
	if (i < 0)
		return -1;
	else
		return i / sizeof(*s);
}

int hfi_get_ctrs_unit_names(int unitno, char **namep)
{
	int i;
	i = hfi_hfifs_unit_read(unitno, "counter_names", namep);
	if (i < 0)
		return -1;
	else
		return hfi_count_names(*namep);
}

int hfi_get_ctrs_unit(int unitno, uint64_t *c, int nelem)
{
	int i;
	i = hfi_hfifs_unit_rd(unitno, "counters", c, nelem * sizeof(*c));
	if (i < 0)
		return -1;
	else
		return i / sizeof(*c);
}

int hfi_get_ctrs_port_names(int unitno, char **namep)
{
	int i;
	i = hfi_hfifs_unit_read(unitno, "portcounter_names", namep);
	if (i < 0)
		return -1;
	else
		return hfi_count_names(*namep);
}

int hfi_get_ctrs_port(int unitno, int port, uint64_t *c, int nelem)
{
	int i;
	char buf[32];
	snprintf(buf, sizeof(buf), "port%dcounters", port);
	i = hfi_hfifs_unit_rd(unitno, buf, c, nelem * sizeof(*c));
	if (i < 0)
		return -1;
	else
		return i / sizeof(*c);
}

int hfi_get_cc_settings_bin(int unit, int port, char *ccabuf)
{
	int fd;
	size_t count;
/*
 * Check qib driver CCA setting, and try to use it if available.
 * Fall to self CCA setting if errors.
 */
	sprintf(ccabuf, HFI_CLASS_PATH "_%d/ports/%d/CCMgtA/cc_settings_bin",
		unit, port);
	fd = open(ccabuf, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	/*
	 * 4 bytes for 'control map'
	 * 2 bytes 'port control'
	 * 32 (#SLs) * 6 bytes 'congestion setting' (per-SL)
	 */
	count = 4 + 2 + (32 * 6);
	if (read(fd, ccabuf, count) != count) {
		_HFI_CCADBG("Read cc_settings_bin failed. using static CCA\n");
		close(fd);
		return 0;
	}

	close(fd);

	return 1;
}

int hfi_get_cc_table_bin(int unit, int port, uint16_t **cctp)
{
	int i, ccti_limit;
	uint16_t *cct;
	int fd;
	char pathname[256];

	*cctp = NULL;
	sprintf(pathname, HFI_CLASS_PATH "_%d/ports/%d/CCMgtA/cc_table_bin",
		unit, port);
	fd = open(pathname, O_RDONLY);
	if (fd < 0) {
		_HFI_CCADBG("Open cc_table_bin failed. using static CCA\n");
		return 0;
	}
	if (read(fd, &ccti_limit, 2) != 2) {
		_HFI_CCADBG("Read ccti_limit failed. using static CCA\n");
		close(fd);
		return 0;
	}
	if (ccti_limit < 63 || ccti_limit > 65535) {
		_HFI_CCADBG("Read ccti_limit %d not in range [63, 65535], "
			    "using static CCA.\n", ccti_limit);
		close(fd);
		return 0;
	}

	i = (ccti_limit + 1) * sizeof(uint16_t);
	cct = malloc(i);
	if (!cct) {
		close(fd);
		return -1;
	}
	if (read(fd, cct, i) != i) {
		_HFI_CCADBG("Read ccti_entry_list, using static CCA\n");
		free(cct);
		close(fd);
		return 0;
	}

	close(fd);

	*cctp = cct;
	return ccti_limit;
}

/*
 * This is for diag function hfi_wait_for_packet() only
 */
int hfi_cmd_wait_for_packet(int fd)
{
	int ret;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;

	ret = poll(&pfd, 1, 500 /* ms */);

	return ret;
}