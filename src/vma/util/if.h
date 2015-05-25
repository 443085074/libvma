/*
 * Copyright (C) Mellanox Technologies Ltd. 2001-2013.  ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Mellanox Technologies Ltd.
 * (the "Company") and all right, title, and interest in and to the software product,
 * including all associated intellectual property rights, are and shall
 * remain exclusively with the Company.
 *
 * This software is made available under either the GPL v2 license or a commercial license.
 * If you wish to obtain a commercial license, please contact Mellanox at support@mellanox.com.
 */


#ifndef VMA_IF_H_
#define VMA_IF_H_

#include <sys/socket.h>
#include <linux/if.h>

/* defined in net/if.h but that conflicts with linux/if.h... */
extern "C" unsigned int if_nametoindex (__const char *__ifname) __THROW;
extern "C" char *if_indextoname (unsigned int __ifindex, char *__ifname) __THROW;

#endif
