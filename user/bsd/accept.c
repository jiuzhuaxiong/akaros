/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */
/* posix */
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/* bsd extensions */
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>

#include "priv.h"

/* they've made the delcarations hurl-inducing. */
int
accept(int fd, __SOCKADDR_ARG __addr,
       socklen_t *__restrict alen)
{
	void *a = (void*)__addr;
	int n, nfd, lcfd;
	Rock *r, *nr;
	struct sockaddr_in *ip;
	char name[Ctlsize];
	char file[8+Ctlsize+1];
	char *p, *net;
	char listen[Ctlsize];

	r = _sock_findrock(fd, 0);
	if(r == 0){
		errno = ENOTSOCK;
		return -1;
	}

	switch(r->domain){
	case PF_INET:
		switch(r->stype){
		case SOCK_DGRAM:
			net = "udp";
			break;
		case SOCK_STREAM:
			net = "tcp";
			break;
		}
		/* at this point, our FD is for the data file.  we need to open the
		 * listen file.  The line is stored in r->ctl (e.g. /net/tcp/666/ctl) */
		strcpy(listen, r->ctl);
		p = strrchr(listen, '/');
		if (p == 0)
			return -1;
		strcpy(p + 1, "listen");

		lcfd = open(listen, O_RDWR);
		if (lcfd < 0)
			return -1;
		/* at this point, we have a new conversation, and lcfd is its ctl fd.
		 * nfd will be the FD for that conv's data file. */
		nfd = _sock_data(lcfd, net, r->domain, r->stype, r->protocol, &nr);
		if (nfd < 0)
			return -1;
		/* we have the data file, and can reopen the ctl based on the info in
		 * the rock (nr) */
		close(lcfd);

		/* get remote address */
		ip = (struct sockaddr_in*)&nr->raddr;
		_sock_ingetaddr(nr, ip, &n, "remote");
		if(a){
			memmove(a, ip, sizeof(struct sockaddr_in));
			*alen = sizeof(struct sockaddr_in);
		}

		return nfd;
	case PF_UNIX:
		if(r->other >= 0){
			errno = EINVAL; // was EGREG
			return -1;
		}

		for(;;){
			/* read path to new connection */
			n = read(fd, name, sizeof(name) - 1);
			if(n < 0)
				return -1;
			if(n == 0)
				continue;
			name[n] = 0;

			/* open new connection */
			_sock_srvname(file, name);
			nfd = open(file, O_RDWR);
			if(nfd < 0)
				continue;

			/* confirm opening on new connection */
			if(write(nfd, name, strlen(name)) > 0)
				break;

			close(nfd);
		}

		nr = _sock_newrock(nfd);
		if(nr == 0){
			close(nfd);
			return -1;
		}
		nr->domain = r->domain;
		nr->stype = r->stype;
		nr->protocol = r->protocol;

		return nfd;
	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}
