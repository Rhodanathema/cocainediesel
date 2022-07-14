/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Management Component Transport Protocol (MCTP)
 *
 * Copyright (c) 2021 Code Construct
 * Copyright (c) 2021 Google
 */

#ifndef __UAPI_MCTP_H
#define __UAPI_MCTP_H

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/netdevice.h>

typedef __u8			mctp_eid_t;

struct mctp_addr {
	mctp_eid_t		s_addr;
};

struct sockaddr_mctp {
	__kernel_sa_family_t	smctp_family;
	__u16			__smctp_pad0;
	unsigned int		smctp_network;
	struct mctp_addr	smctp_addr;
	__u8			smctp_type;
	__u8			smctp_tag;
	__u8			__smctp_pad1;
};

struct sockaddr_mctp_ext {
	struct sockaddr_mctp	smctp_base;
	int			smctp_ifindex;
	__u8			smctp_halen;
	__u8			__smctp_pad0[3];
	__u8			smctp_haddr[MAX_ADDR_LEN];
};

#define MCTP_NET_ANY		0x0

#define MCTP_ADDR_NULL		0x00
#define MCTP_ADDR_ANY		0xff

#define MCTP_TAG_MASK		0x07
#define MCTP_TAG_OWNER		0x08

#define MCTP_OPT_ADDR_EXT	1

#endif /* __UAPI_MCTP_H */