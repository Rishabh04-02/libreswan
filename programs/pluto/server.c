/* get-next-event loop
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2002  D. Hugh Redelmeier.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * RCSID $Id: server.c,v 1.113 2005/08/27 05:51:00 paul Exp $
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef SOLARIS
# include <sys/sockio.h>	/* for Solaris 2.6: defines SIOCGIFCONF */
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/nameser.h>	/* missing from <resolv.h> on old systems */
#include <sys/resource.h>
#include <sys/wait.h>

#include <openswan.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "state.h"
#include "id.h"
#include "x509.h"
#include "pgp.h"
#include "certs.h"
#include "smartcard.h"
#ifdef XAUTH_USEPAM
#include <security/pam_appl.h>
#endif
#include "connections.h"	/* needs id.h */
#include "kernel.h"  /* for no_klips; needs connections.h */
#include "log.h"
#include "server.h"
#include "timer.h"
#include "packet.h"
#include "demux.h"  /* needs packet.h */
#include "rcv_whack.h"
#include "rcv_info.h"
#include "keys.h"
#include "adns.h"	/* needs <resolv.h> */
#include "dnskey.h"	/* needs keys.h and adns.h */
#include "whack.h"	/* for RC_LOG_SERIOUS */
#include "pluto_crypt.h" /* cryptographic helper functions */
#include "udpfromto.h"

#include <pfkeyv2.h>
#include <pfkey.h>
#include "kameipsec.h"

#ifdef NAT_TRAVERSAL
#include "nat_traversal.h"
#endif

/*
 *  Server main loop and socket initialization routines.
 */

static const int on = TRUE;	/* by-reference parameter; constant, we hope */

bool no_retransmits = FALSE;

/* list of interface devices */
LIST_HEAD(,iface_dev) interface_dev;

/* control (whack) socket */
int ctl_fd = NULL_FD;	/* file descriptor of control (whack) socket */
#if !(defined(macintosh) || (defined(__MACH__) && defined(__APPLE__)))
struct sockaddr_un ctl_addr = { AF_UNIX, DEFAULT_CTLBASE CTL_SUFFIX };
#else
/* This will require fixes elsewhere too! */
struct sockaddr_un ctl_addr = { sizeof(struct sockaddr_un), AF_UNIX, DEFAULT_CTLBASE CTL_SUFFIX };
#endif

/* info (showpolicy) socket */
int policy_fd = NULL_FD;
#if !(defined(macintosh) || (defined(__MACH__) && defined(__APPLE__)))
struct sockaddr_un info_addr= { AF_UNIX, DEFAULT_CTLBASE INFO_SUFFIX };
#else
/* This will require fixes elsewhere too! */
struct sockaddr_un info_addr= { sizeof(struct sockaddr_un), AF_UNIX, DEFAULT_CTLBASE INFO_SUFFIX };
#endif

/* Initialize the control socket.
 * Note: this is called very early, so little infrastructure is available.
 * It is important that the socket is created before the original
 * Pluto process returns.
 */
err_t
init_ctl_socket(void)
{
    err_t failed = NULL;

    LIST_INIT(&interface_dev);

    delete_ctl_socket();	/* preventative medicine */
    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctl_fd == -1)
	failed = "create";
    else if (fcntl(ctl_fd, F_SETFD, FD_CLOEXEC) == -1)
	failed = "fcntl FD+CLOEXEC";
    else if (setsockopt(ctl_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof(on)) < 0)
	failed = "setsockopt";
    else
    {
	/* to keep control socket secure, use umask */
#ifdef PLUTO_GROUP_CTL
	mode_t ou = umask(~(S_IRWXU|S_IRWXG));
#else
	mode_t ou = umask(~S_IRWXU);
#endif

	if (bind(ctl_fd, (struct sockaddr *)&ctl_addr
	, offsetof(struct sockaddr_un, sun_path) + strlen(ctl_addr.sun_path)) < 0)
	    failed = "bind";
	umask(ou);
    }

#ifdef PLUTO_GROUP_CTL
    {
	struct group *g;

	g = getgrnam("pluto");
	if(g != NULL) {
	    if(fchown(ctl_fd, -1, g->gr_gid) != 0) {
		loglog(RC_LOG_SERIOUS, "Can not chgrp ctl fd(%d) to gid=%d: %s\n"
		       , ctl_fd, g->gr_gid, strerror(errno));
	    }
	}
    }
#endif

    /* 5 is a haphazardly chosen limit for the backlog.
     * Rumour has it that this is the max on BSD systems.
     */
    if (failed == NULL && listen(ctl_fd, 5) < 0)
	failed = "listen() on";

    return failed == NULL? NULL : builddiag("could not %s control socket: %d %s"
	    , failed, errno, strerror(errno));
}

void
delete_ctl_socket(void)
{
    /* Is noting failure useful?  Not when used as preventative medicine. */
    unlink(ctl_addr.sun_path);
}

#ifdef IPSECPOLICY
/* Initialize the info socket.
 */
err_t
init_info_socket(void)
{
    err_t failed = NULL;

    delete_info_socket();	/* preventative medicine */
    info_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (info_fd == -1)
	failed = "create";
    else if (fcntl(info_fd, F_SETFD, FD_CLOEXEC) == -1)
	failed = "fcntl FD+CLOEXEC";
    else if (setsockopt(info_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof(on)) < 0)
	failed = "setsockopt";
    else
    {
	/* this socket should be openable by all proceses */
	mode_t ou = umask(0);

	if (bind(info_fd, (struct sockaddr *)&info_addr
	, offsetof(struct sockaddr_un, sun_path) + strlen(info_addr.sun_path)) < 0)
	    failed = "bind";
	umask(ou);
    }

    /* 64 might be big enough, and the system may limit us anyway.
     */
    if (failed == NULL && listen(info_fd, 64) < 0)
	failed = "listen() on";

    return failed == NULL? NULL : builddiag("could not %s info socket: %d %s"
	    , failed, errno, strerror(errno));
}

void
delete_info_socket(void)
{
    unlink(info_addr.sun_path);
}
#endif /* IPSECPOLICY */


bool listening = FALSE;	/* should we pay attention to IKE messages? */

struct iface_port  *interfaces = NULL;	/* public interfaces */

/* Initialize the interface sockets. */

static void
mark_ifaces_dead(void)
{
    struct iface_port *p;

    for (p = interfaces; p != NULL; p = p->next)
	p->change = IFN_DELETE;
}

static void
free_dead_iface_dev(struct iface_dev *id)
{
    if(--id->id_count == 0) {
	pfree(id->id_vname);
	pfree(id->id_rname);

	LIST_REMOVE(id, id_entry);

	pfree(id);
    }
}

static void
free_dead_ifaces(void)
{
    struct iface_port *p;
    bool some_dead = FALSE
	, some_new = FALSE;

    for (p = interfaces; p != NULL; p = p->next)
    {
	if (p->change == IFN_DELETE)
	{
	    openswan_log("shutting down interface %s/%s %s:%d"
			 , p->ip_dev->id_vname
			 , p->ip_dev->id_rname
			 , ip_str(&p->ip_addr), p->port);
	    some_dead = TRUE;
	}
	else if (p->change == IFN_ADD)
	{
	    some_new = TRUE;
	}
    }

    if (some_dead)
    {
	struct iface_port **pp;

	release_dead_interfaces();
	for (pp = &interfaces; (p = *pp) != NULL; )
	{
	    if (p->change == IFN_DELETE)
	    {
		struct iface_dev *id;

		*pp = p->next;	/* advance *pp */
		close(p->fd);

		id = p->ip_dev;
		pfree(p);

		free_dead_iface_dev(id);
	    }
	    else
	    {
		pp = &p->next;	/* advance pp */
	    }
	}
    }

    /* this must be done after the release_dead_interfaces
     * in case some to the newly unoriented connections can
     * become oriented here.
     */
    if (some_dead || some_new)
	check_orientations();
}

void
free_ifaces(void)
{
    mark_ifaces_dead();
    free_dead_ifaces();
}

struct raw_iface {
    ip_address addr;
    char name[IFNAMSIZ + 20];	/* what would be a safe size? */
    struct raw_iface *next;
};

struct raw_iface *static_ifn=NULL;

/* Called to handle --interface <ifname>
 * Semantics: if specified, only these (real) interfaces are considered.
 */
#if !defined(__CYGWIN32__)
static const char *pluto_ifn[10];
static int pluto_ifn_roof = 0;

bool
use_interface(const char *rifn)
{
    if(pluto_ifn_inst[0]=='\0') {
	pluto_ifn_inst = clone_str(rifn, "genifn");
    }

    if (pluto_ifn_roof >= (int)elemsof(pluto_ifn))
    {
	return FALSE;
    }
    else
    {
	pluto_ifn[pluto_ifn_roof++] = rifn;
	return TRUE;
    }
}
#else
bool
use_interface(const char *rifn)
{
    struct raw_iface *ri;
    static int ifnum=0;
    err_t e;

    if(pluto_ifn_inst[0]=='\0') {
	pluto_ifn_inst = clone_str(rifn, "genifn");
    }

    ri = alloc_thing(*ri, "static interface");

    e = ttoaddr(rifn, strlen(rifn), 0, &ri->addr);
    if(e) {
	fprintf(stderr, "--interface failed: %s\n", e);
	exit(10);
    }
    snprintf(ri->name, sizeof(ri->name), "ifn%d", ifnum++);

    ri->next = static_ifn;
    static_ifn = ri;

    return TRUE;
}
#endif

#ifndef IPSECDEVPREFIX
# define IPSECDEVPREFIX "ipsec"
#endif

#if !defined(__CYGWIN32__)
static struct raw_iface *
find_raw_ifaces4(void)
{
    int j;	/* index into buf */
    struct ifconf ifconf;
    struct ifreq buf[300];	/* for list of interfaces -- arbitrary limit */
    struct raw_iface *rifaces = NULL;
    int master_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);    /* Get a UDP socket */

    /* get list of interfaces with assigned IPv4 addresses from system */

    if (master_sock == -1)
	exit_log_errno((e, "socket() failed in find_raw_ifaces4()"));

    if (setsockopt(master_sock, SOL_SOCKET, SO_REUSEADDR
    , (const void *)&on, sizeof(on)) < 0)
	exit_log_errno((e, "setsockopt() in find_raw_ifaces4()"));

    /* bind the socket */
    {
	ip_address any;

	happy(anyaddr(AF_INET, &any));
	setportof(htons(pluto_port), &any);
	if (bind(master_sock, sockaddrof(&any), sockaddrlenof(&any)) < 0)
	    exit_log_errno((e, "bind() failed in find_raw_ifaces4()"));
    }

    /* Get local interfaces.  See netdevice(7). */
    ifconf.ifc_len = sizeof(buf);
    ifconf.ifc_buf = (void *) buf;
    zero(buf);

    if (ioctl(master_sock, SIOCGIFCONF, &ifconf) == -1)
	exit_log_errno((e, "ioctl(SIOCGIFCONF) in find_raw_ifaces4()"));

    /* Add an entry to rifaces for each interesting interface. */
    for (j = 0; (j+1) * sizeof(*buf) <= (size_t)ifconf.ifc_len; j++)
    {
	struct raw_iface ri;
	const struct sockaddr_in *rs = (struct sockaddr_in *) &buf[j].ifr_addr;
	struct ifreq auxinfo;

	/* ignore all but AF_INET interfaces */
	if (rs->sin_family != AF_INET)
	    continue;	/* not interesting */

	/* build a NUL-terminated copy of the rname field */
	memcpy(ri.name, buf[j].ifr_name, IFNAMSIZ);
	ri.name[IFNAMSIZ] = '\0';

	/* ignore if our interface names were specified, and this isn't one */
	if (pluto_ifn_roof != 0)
	{
	    int i;

	    for (i = 0; i != pluto_ifn_roof; i++)
		if (streq(ri.name, pluto_ifn[i]))
		    break;
	    if (i == pluto_ifn_roof)
		continue;	/* not found -- skip */
	}

	/* Find out stuff about this interface.  See netdevice(7). */
	zero(&auxinfo);	/* paranoia */
	memcpy(auxinfo.ifr_name, buf[j].ifr_name, IFNAMSIZ);
	if (ioctl(master_sock, SIOCGIFFLAGS, &auxinfo) == -1)
	    exit_log_errno((e
		, "ioctl(SIOCGIFFLAGS) for %s in find_raw_ifaces4()"
		, ri.name));
	if (!(auxinfo.ifr_flags & IFF_UP))
	    continue;	/* ignore an interface that isn't UP */
#if defined(linux)
        if (auxinfo.ifr_flags & IFF_SLAVE)
            continue;   /* ignore slave interfaces; they share IPs with their master */
#endif

	/* ignore unconfigured interfaces */
	if (rs->sin_addr.s_addr == 0)
	    continue;

	happy(initaddr((const void *)&rs->sin_addr, sizeof(struct in_addr)
	    , AF_INET, &ri.addr));

	DBG(DBG_CONTROL, DBG_log("found %s with address %s"
	    , ri.name, ip_str(&ri.addr)));
	ri.next = rifaces;
	rifaces = clone_thing(ri, "struct raw_iface");
    }

    close(master_sock);

    return rifaces;
}

static struct raw_iface *
find_raw_ifaces6(void)
{

    /* Get list of interfaces with IPv6 addresses from system from /proc/net/if_inet6).
     *
     * Documentation of format?
     * RTFS: linux-2.2.16/net/ipv6/addrconf.c:iface_proc_info()
     *       linux-2.4.9-13/net/ipv6/addrconf.c:iface_proc_info()
     *
     * Sample from Gerhard's laptop:
     *	00000000000000000000000000000001 01 80 10 80       lo
     *	30490009000000000000000000010002 02 40 00 80   ipsec0
     *	30490009000000000000000000010002 07 40 00 80     eth0
     *	fe80000000000000025004fffefd5484 02 0a 20 80   ipsec0
     *	fe80000000000000025004fffefd5484 07 0a 20 80     eth0
     *
     * Each line contains:
     * - IPv6 address: 16 bytes, in hex, no punctuation
     * - ifindex: 1 byte, in hex
     * - prefix_len: 1 byte, in hex
     * - scope (e.g. global, link local): 1 byte, in hex
     * - flags: 1 byte, in hex
     * - device name: string, followed by '\n'
     */
    struct raw_iface *rifaces = NULL;
    static const char proc_name[] = "/proc/net/if_inet6";
    FILE *proc_sock = fopen(proc_name, "r");

    if (proc_sock == NULL)
    {
	DBG(DBG_CONTROL, DBG_log("could not open %s", proc_name));
    }
    else
    {
	for (;;)
	{
	    struct raw_iface ri;
	    unsigned short xb[8];	/* IPv6 address as 8 16-bit chunks */
	    char sb[8*5];	/* IPv6 address as string-with-colons */
	    unsigned int if_idx;	/* proc field, not used */
	    unsigned int plen;	/* proc field, not used */
	    unsigned int scope;	/* proc field, used to exclude link-local */
	    unsigned int dad_status;	/* proc field, not used */
	    /* ??? I hate and distrust scanf -- DHR */
	    int r = fscanf(proc_sock
		, "%4hx%4hx%4hx%4hx%4hx%4hx%4hx%4hx"
		  " %02x %02x %02x %02x %20s\n"
		, xb+0, xb+1, xb+2, xb+3, xb+4, xb+5, xb+6, xb+7
		, &if_idx, &plen, &scope, &dad_status, ri.name);

	    /* ??? we should diagnose any problems */
	    if (r != 13)
		break;

	    /* ignore addresses with link local scope.
	     * From linux-2.4.9-13/include/net/ipv6.h:
	     * IPV6_ADDR_LINKLOCAL	0x0020U
	     * IPV6_ADDR_SCOPE_MASK	0x00f0U
	     */
	    if ((scope & 0x00f0U) == 0x0020U)
		continue;

	    snprintf(sb, sizeof(sb)
		, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x"
		, xb[0], xb[1], xb[2], xb[3], xb[4], xb[5], xb[6], xb[7]);

	    happy(ttoaddr(sb, 0, AF_INET6, &ri.addr));

	    if (!isunspecaddr(&ri.addr))
	    {
		DBG(DBG_CONTROL
		    , DBG_log("found %s with address %s"
			, ri.name, sb));
		ri.next = rifaces;
		rifaces = clone_thing(ri, "struct raw_iface");
	    }
	}
	fclose(proc_sock);
    }

    return rifaces;
}
#endif

static int
create_socket(struct raw_iface *ifp, const char *v_name, int port)
{
    int fd = socket(addrtypeof(&ifp->addr), SOCK_DGRAM, IPPROTO_UDP);
    int fcntl_flags;

    if (fd < 0)
    {
	log_errno((e, "socket() in process_raw_ifaces()"));
	return -1;
    }

    /* Set socket Nonblocking */
    if ((fcntl_flags=fcntl(fd, F_GETFL)) >= 0) {
	if (!(fcntl_flags & O_NONBLOCK)) {
	    fcntl_flags |= O_NONBLOCK;
	    fcntl(fd, F_SETFL, fcntl_flags);
	}
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
	log_errno((e, "fcntl(,, FD_CLOEXEC) in process_raw_ifaces()"));
	close(fd);
	return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR
    , (const void *)&on, sizeof(on)) < 0)
    {
	log_errno((e, "setsockopt SO_REUSEADDR in process_raw_ifaces()"));
	close(fd);
	return -1;
    }

    /* To improve error reporting.  See ip(7). */
#if defined(IP_RECVERR) && defined(MSG_ERRQUEUE)
    if (setsockopt(fd, SOL_IP, IP_RECVERR
    , (const void *)&on, sizeof(on)) < 0)
    {
	log_errno((e, "setsockopt IP_RECVERR in process_raw_ifaces()"));
	close(fd);
	return -1;
    }
#endif

    /* With IPv6, there is no fragmentation after
     * it leaves our interface.  PMTU discovery
     * is mandatory but doesn't work well with IKE (why?).
     * So we must set the IPV6_USE_MIN_MTU option.
     * See draft-ietf-ipngwg-rfc2292bis-01.txt 11.1
     */
#ifdef IPV6_USE_MIN_MTU	/* YUCK: not always defined */
    if (addrtypeof(&ifp->addr) == AF_INET6
    && setsockopt(fd, SOL_SOCKET, IPV6_USE_MIN_MTU
      , (const void *)&on, sizeof(on)) < 0)
    {
	log_errno((e, "setsockopt IPV6_USE_MIN_MTU in process_raw_ifaces()"));
	close(fd);
	return -1;
    }
#endif

#if defined(linux) && defined(NETKEY_SUPPORT)
    if (kern_interface == USE_NETKEY)
    {
	struct sadb_x_policy policy;
	int level, opt;

	policy.sadb_x_policy_len = sizeof(policy) / IPSEC_PFKEYv2_ALIGN;
	policy.sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	policy.sadb_x_policy_type = IPSEC_POLICY_BYPASS;
	policy.sadb_x_policy_dir = IPSEC_DIR_INBOUND;
	policy.sadb_x_policy_reserved = 0;
	policy.sadb_x_policy_id = 0;
	policy.sadb_x_policy_reserved2 = 0;

	if (addrtypeof(&ifp->addr) == AF_INET6)
	{
	    level = IPPROTO_IPV6;
	    opt = IPV6_IPSEC_POLICY;
	}
	else
	{
	    level = IPPROTO_IP;
	    opt = IP_IPSEC_POLICY;
	}

	if (setsockopt(fd, level, opt
	  , &policy, sizeof(policy)) < 0)
	{
	    log_errno((e, "setsockopt IPSEC_POLICY in process_raw_ifaces()"));
	    close(fd);
	    return -1;
	}

	policy.sadb_x_policy_dir = IPSEC_DIR_OUTBOUND;

	if (setsockopt(fd, level, opt
	  , &policy, sizeof(policy)) < 0)
	{
	    log_errno((e, "setsockopt IPSEC_POLICY in process_raw_ifaces()"));
	    close(fd);
	    return -1;
	}
    }
#endif

    setportof(htons(port), &ifp->addr);
    if (bind(fd, sockaddrof(&ifp->addr), sockaddrlenof(&ifp->addr)) < 0)
    {
	log_errno((e, "bind() for %s/%s %s:%u in process_raw_ifaces()"
	    , ifp->name, v_name
	    , ip_str(&ifp->addr), (unsigned) port));
	close(fd);
	return -1;
    }
    setportof(htons(pluto_port), &ifp->addr);

#if defined(HAVE_UDPFROMTO)
    /* we are going to use udpfromto.c, so initialize it */
    udpfromto_init(fd);
#endif

    return fd;
}

static void
process_raw_ifaces(struct raw_iface *rifaces)
{
    struct raw_iface *ifp;

    /* Find all virtual/real interface pairs.
     * For each real interface...
     */
    for (ifp = rifaces; ifp != NULL; ifp = ifp->next)
    {
	struct raw_iface *v = NULL;	/* matching ipsecX interface */
	struct raw_iface fake_v;
	bool after = FALSE; /* has vfp passed ifp on the list? */
	bool bad = FALSE;
	struct raw_iface *vfp;

	/* ignore if virtual (ipsec*) interface */
	if (strncmp(ifp->name, IPSECDEVPREFIX, sizeof(IPSECDEVPREFIX)-1) == 0)
	    continue;

	for (vfp = rifaces; vfp != NULL; vfp = vfp->next)
	{
	    if (vfp == ifp)
	    {
		after = TRUE;
	    }
	    else if (sameaddr(&ifp->addr, &vfp->addr))
	    {
		/* Different entries with matching IP addresses.
		 * Many interesting cases.
		 */
		if (strncmp(vfp->name, IPSECDEVPREFIX, sizeof(IPSECDEVPREFIX)-1) == 0)
		{
		    if (v != NULL)
		    {
			loglog(RC_LOG_SERIOUS
			    , "ipsec interfaces %s and %s share same address %s"
			    , v->name, vfp->name, ip_str(&ifp->addr));
			bad = TRUE;
		    }
		    else
		    {
			v = vfp;	/* current winner */
		    }
		}
		else
		{
		    /* ugh: a second real interface with the same IP address
		     * "after" allows us to avoid double reporting.
		     */
#if defined(linux) && defined(NETKEY_SUPPORT)
		    if (kern_interface == USE_NETKEY)
		    {
			if (after)
			{
			    bad = TRUE;
			    break;
			}
			continue;
		    }
#endif
		    if (after)
		    {
			loglog(RC_LOG_SERIOUS
			    , "IP interfaces %s and %s share address %s!"
			    , ifp->name, vfp->name, ip_str(&ifp->addr));
		    }
		    bad = TRUE;
		}
	    }
	}

	if (bad)
	    continue;

#if defined(linux) && defined(NETKEY_SUPPORT)
	if (kern_interface == USE_NETKEY)
	{
	    v = ifp;
	    goto add_entry;
	}
#endif

	/* what if we didn't find a virtual interface? */
	if (v == NULL)
	{
	    if (kern_interface == NO_KERNEL)
	    {
		/* kludge for testing: invent a virtual device */
		static const char fvp[] = "virtual";
		fake_v = *ifp;
		passert(sizeof(fake_v.name) > sizeof(fvp));
		strcpy(fake_v.name, fvp);
		addrtot(&ifp->addr, 0, fake_v.name + sizeof(fvp) - 1
		    , sizeof(fake_v.name) - (sizeof(fvp) - 1));
		v = &fake_v;
	    }
	    else
	    {
		DBG(DBG_CONTROL,
			DBG_log("IP interface %s %s has no matching ipsec* interface -- ignored"
			    , ifp->name, ip_str(&ifp->addr)));
		continue;
	    }
	}

	/* We've got all we need; see if this is a new thing:
	 * search old interfaces list.
	 */
#if defined(linux) && defined(NETKEY_SUPPORT)
add_entry:
#endif
	{
	    struct iface_port **p = &interfaces;

	    for (;;)
	    {
		struct iface_port *q = *p;
		struct iface_dev *id = NULL;

		/* search is over if at end of list */
		if (q == NULL)
		{
		    /* matches nothing -- create a new entry */
		    int fd = create_socket(ifp, v->name, pluto_port);

		    if (fd < 0)
			break;

#ifdef NAT_TRAVERSAL
		    if (nat_traversal_support_non_ike && addrtypeof(&ifp->addr) == AF_INET)
		    {
			nat_traversal_espinudp_socket(fd, "IPv4", ESPINUDP_WITH_NON_IKE);
		    }
#endif

		    q = alloc_thing(struct iface_port, "struct iface_port");
		    id = alloc_thing(struct iface_dev, "struct iface_dev");

		    LIST_INSERT_HEAD(&interface_dev, id, id_entry);

		    q->ip_dev = id;
		    id->id_rname = clone_str(ifp->name, "real device name");
		    id->id_vname = clone_str(v->name, "virtual device name");
		    id->id_count++;

		    q->ip_addr = ifp->addr;
		    q->fd = fd;
		    q->next = interfaces;
		    q->change = IFN_ADD;
		    q->port = pluto_port;
		    q->ike_float = FALSE;

		    interfaces = q;

		    openswan_log("adding interface %s/%s %s:%d"
				 , q->ip_dev->id_vname
				 , q->ip_dev->id_rname
				 , ip_str(&q->ip_addr)
				 , q->port);

#ifdef NAT_TRAVERSAL
		    /*
		     * right now, we do not support NAT-T on IPv6, because
		     * the kernel did not support it, and gave an error
		     * it one tried to turn it on.
		     */
		    if (nat_traversal_support_port_floating
			&& addrtypeof(&ifp->addr) == AF_INET)
		    {
			fd = create_socket(ifp, v->name, NAT_T_IKE_FLOAT_PORT);
			if (fd < 0) 
			    break;
			nat_traversal_espinudp_socket(fd, "IPv4"
						      , ESPINUDP_WITH_NON_ESP);
			q = alloc_thing(struct iface_port, "struct iface_port");
			q->ip_dev = id;
			id->id_count++;
			
			q->ip_addr = ifp->addr;
			setportof(htons(NAT_T_IKE_FLOAT_PORT), &q->ip_addr);
			q->port = NAT_T_IKE_FLOAT_PORT;
			q->fd = fd;
			q->next = interfaces;
			q->change = IFN_ADD;
			q->ike_float = TRUE;
			interfaces = q;
			openswan_log("adding interface %s/%s %s:%d"
				     , q->ip_dev->id_vname, q->ip_dev->id_rname
				     , ip_str(&q->ip_addr)
				     , q->port);
		    }
#endif
		    break;
		}

		/* search over if matching old entry found */
		if (streq(q->ip_dev->id_rname, ifp->name)
		    && streq(q->ip_dev->id_vname, v->name)
		    && sameaddr(&q->ip_addr, &ifp->addr))
		{
		    /* matches -- rejuvinate old entry */
		    q->change = IFN_KEEP;
#ifdef NAT_TRAVERSAL
		    /* look for other interfaces to keep (due to NAT-T) */
		    for (q = q->next ; q ; q = q->next) {
			if (streq(q->ip_dev->id_rname, ifp->name)
			    && streq(q->ip_dev->id_vname, v->name)
			    && sameaddr(&q->ip_addr, &ifp->addr)) {
				q->change = IFN_KEEP;
			}
		    }
#endif
		    break;
		}

		/* try again */
		p = &q->next;
	    } /* for (;;) */
	}
    }

    /* delete the raw interfaces list */
    while (rifaces != NULL)
    {
	struct raw_iface *t = rifaces;

	rifaces = t->next;
	pfree(t);
    }
}

void
find_ifaces(void)
{
    mark_ifaces_dead();

#if !defined(__CYGWIN32__)
    process_raw_ifaces(find_raw_ifaces4());
    process_raw_ifaces(find_raw_ifaces6());
#endif
    process_raw_ifaces(static_ifn);

    free_dead_ifaces();	    /* ditch remaining old entries */

    if (interfaces == NULL)
	loglog(RC_LOG_SERIOUS, "no public interfaces found");
}

void
show_ifaces_status(void)
{
    struct iface_port *p;

    for (p = interfaces; p != NULL; p = p->next)
	whack_log(RC_COMMENT, "interface %s/%s %s"
	    , p->ip_dev->id_vname, p->ip_dev->id_rname, ip_str(&p->ip_addr));
}

void
show_debug_status(void)
{
#ifdef DEBUG
    whack_log(RC_COMMENT, "debug %s"
	, bitnamesof(debug_bit_names, cur_debugging));
#endif
}

static volatile sig_atomic_t sighupflag = FALSE;

static void
huphandler(int sig UNUSED)
{
    sighupflag = TRUE;
}

static volatile sig_atomic_t sigtermflag = FALSE;

static void
termhandler(int sig UNUSED)
{
    sigtermflag = TRUE;
}

static volatile sig_atomic_t sigchildflag = FALSE;

static void
childhandler(int sig UNUSED)
{
    sigchildflag = TRUE;
}

/* perform wait4() on all children */
static void
reapchildren(void)
{
    pid_t child;
    int status;
    struct rusage r;

    sigchildflag = FALSE;
    errno=0;

    while((child = wait3(&status, WNOHANG, &r)) > 0) {
	/* got a child to reap */
	if(adns_reapchild(child, status)) continue;
	if(pluto_crypt_handle_dead_child(child, status)) continue;
	
	openswan_log("child pid=%d (status=%d) is not my child!", child, status);
    }
    
    if(child == -1) {
	openswan_log("reapchild failed with errno=%d %s",
		     errno, strerror(errno));
    }
}

/* call_server listens for incoming ISAKMP packets and Whack messages,
 * and handles timer events.
 */
void
call_server(void)
{
    struct iface_port *ifp;

    /* catch SIGHUP and SIGTERM */
    {
	int r;
	struct sigaction act;

	act.sa_handler = &huphandler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;	/* no SA_ONESHOT, no SA_RESTART, no nothing */
	r = sigaction(SIGHUP, &act, NULL);
	passert(r == 0);

	act.sa_handler = &termhandler;
	r = sigaction(SIGTERM, &act, NULL);
	passert(r == 0);

	act.sa_handler = &childhandler;
	act.sa_flags   = SA_RESTART;
	r = sigaction(SIGCHLD, &act, NULL);
	passert(r == 0);
    }

    for (;;)
    {
	fd_set readfds;
	fd_set writefds;
	int ndes;

	/* wait for next interesting thing */

	for (;;)
	{
	    long next_time = next_event();   /* time to any pending timer event */
	    int maxfd = ctl_fd;

	    if (sigtermflag)
		exit_pluto(0);

	    if (sighupflag)
	    {
		/* Ignorant folks think poking any daemon with SIGHUP
		 * is polite.  We catch it and tell them otherwise.
		 * There is one use: unsticking a hung recvfrom.
		 * This sticking happens sometimes -- kernel bug?
		 */
		sighupflag = FALSE;
		openswan_log("Pluto ignores SIGHUP -- perhaps you want \"whack --listen\"");
	    }

	    if(sigchildflag) {
		reapchildren();
	    }

	    FD_ZERO(&readfds);
	    FD_ZERO(&writefds);
	    FD_SET(ctl_fd, &readfds);
#ifdef IPSECPOLICY
	    FD_SET(info_fd, &readfds);
	    if (maxfd < info_fd)
		maxfd = info_fd;
#endif

	    /* the only write file-descriptor of interest */
	    if (adns_qfd != NULL_FD && unsent_ADNS_queries)
	    {
		if (maxfd < adns_qfd)
		    maxfd = adns_qfd;
		FD_SET(adns_qfd, &writefds);
	    }

	    if (adns_afd != NULL_FD)
	    {
		if (maxfd < adns_afd)
		    maxfd = adns_afd;
		FD_SET(adns_afd, &readfds);
	    }

#ifdef KLIPS
	    if (kern_interface != NO_KERNEL)
	    {
		int fd = *kernel_ops->async_fdp;

		if (kernel_ops->process_queue)
		    kernel_ops->process_queue();
		if (maxfd < fd)
		    maxfd = fd;
		passert(!FD_ISSET(fd, &readfds));
		FD_SET(fd, &readfds);
	    }
#endif

	    if (listening)
	    {
		for (ifp = interfaces; ifp != NULL; ifp = ifp->next)
		{
		    if (maxfd < ifp->fd)
			maxfd = ifp->fd;
		    passert(!FD_ISSET(ifp->fd, &readfds));
		    FD_SET(ifp->fd, &readfds);
		}
	    }

	    /* see if helpers need attention */
	    pluto_crypto_helper_sockets(&readfds);

	    if (no_retransmits || next_time < 0)
	    {
		/* select without timer */

		ndes = select(maxfd + 1, &readfds, &writefds, NULL, NULL);
	    }
	    else if (next_time == 0)
	    {
		/* timer without select: there is a timer event pending,
		 * and it should fire now so don't bother to do the select.
		 */
		ndes = 0;	/* signify timer expiration */
	    }
	    else
	    {
		/* select with timer */

		struct timeval tm;

		tm.tv_sec = next_time;
		tm.tv_usec = 0;
		ndes = select(maxfd + 1, &readfds, &writefds, NULL, &tm);
	    }

	    if (ndes != -1)
		break;	/* success */

	    if (errno != EINTR)
		exit_log_errno((e, "select() failed in call_server()"));

	    /* retry if terminated by signal */
	}

	if(log_to_stderr_desired) {
	    time_t n;

	    time(&n);
	    DBG_log("time is %s (%lu)", ctime(&n), n);
	}
		    
	/* figure out what is interesting */

	if (ndes == 0)
	{
	    /* timer event */

	    if(!no_retransmits)
	    {
		DBG(DBG_CONTROL,
		    DBG_log(BLANK_FORMAT);
		    DBG_log("*time to handle event"));
		
		handle_timer_event();
		passert(GLOBALS_ARE_RESET());
	    }
	}
	else
	{
	    /* at least one file descriptor is ready */

	    if (adns_qfd != NULL_FD && FD_ISSET(adns_qfd, &writefds))
	    {
		passert(ndes > 0);
		send_unsent_ADNS_queries();
		passert(GLOBALS_ARE_RESET());
		ndes--;
	    }

	    if (adns_afd != NULL_FD && FD_ISSET(adns_afd, &readfds))
	    {
		passert(ndes > 0);
		DBG(DBG_CONTROL,
		    DBG_log(BLANK_FORMAT);
		    DBG_log("*received adns message"));
		handle_adns_answer();
		passert(GLOBALS_ARE_RESET());
		ndes--;
	    }

#ifdef KLIPS
	    if (kern_interface != NO_KERNEL
		&& FD_ISSET(*kernel_ops->async_fdp, &readfds))
	    {
		passert(ndes > 0);
		DBG(DBG_CONTROL,
		    DBG_log(BLANK_FORMAT);
		    DBG_log("*received kernel message"));
		kernel_ops->process_msg();
		passert(GLOBALS_ARE_RESET());
		ndes--;
	    }
#endif

	    for (ifp = interfaces; ifp != NULL; ifp = ifp->next)
	    {
		if (FD_ISSET(ifp->fd, &readfds))
		{
		    /* comm_handle will print DBG_CONTROL intro,
		     * with more info than we have here.
		     */

		    passert(ndes > 0);
		    comm_handle(ifp);
		    passert(GLOBALS_ARE_RESET());
		    ndes--;
		}
	    }

	    if (FD_ISSET(ctl_fd, &readfds))
	    {
		passert(ndes > 0);
		DBG(DBG_CONTROL,
		    DBG_log(BLANK_FORMAT);
		    DBG_log("*received whack message"));
		whack_handle(ctl_fd);
		passert(GLOBALS_ARE_RESET());
		ndes--;
	    }

#ifdef IPSECPOLICY
	    if (FD_ISSET(info_fd, &readfds))
	    {
		passert(ndes > 0);
		DBG(DBG_CONTROL,
		    DBG_log(BLANK_FORMAT);
		    DBG_log("*received info message"));
		info_handle(info_fd);
		passert(GLOBALS_ARE_RESET());
		ndes--;
	    }
#endif

	    /* note we process helper things last on purpose */
	    ndes -= pluto_crypto_helper_ready(&readfds);

	    passert(ndes == 0);
	}
    }
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * End Variables:
 */
