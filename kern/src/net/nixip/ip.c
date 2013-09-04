#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>

#define BLKIPVER(xp)	(((struct Ip4hdr*)((xp)->rp))->vihl&0xF0)

static char *statnames[] = {
	[Forwarding] "Forwarding",
	[DefaultTTL] "DefaultTTL",
	[InReceives] "InReceives",
	[InHdrErrors] "InHdrErrors",
	[InAddrErrors] "InAddrErrors",
	[ForwDatagrams] "ForwDatagrams",
	[InUnknownProtos] "InUnknownProtos",
	[InDiscards] "InDiscards",
	[InDelivers] "InDelivers",
	[OutRequests] "OutRequests",
	[OutDiscards] "OutDiscards",
	[OutNoRoutes] "OutNoRoutes",
	[ReasmTimeout] "ReasmTimeout",
	[ReasmReqds] "ReasmReqds",
	[ReasmOKs] "ReasmOKs",
	[ReasmFails] "ReasmFails",
	[FragOKs] "FragOKs",
	[FragFails] "FragFails",
	[FragCreates] "FragCreates",
};

#define BLKIP(xp)	((struct Ip4hdr*)((xp)->rp))
/*
 * This sleazy macro relies on the media header size being
 * larger than sizeof(Ipfrag). ipreassemble checks this is true
 */
#define BKFG(xp)	((struct Ipfrag*)((xp)->base))

uint16_t ipcsum(uint8_t * unused_uint8_p_t);
struct block *ip4reassemble(struct IP *, int unused_int,
							struct block *, struct Ip4hdr *);
void ipfragfree4(struct IP *, struct fragment4 *);
struct fragment4 *ipfragallo4(struct IP *);

void ip_init_6(struct fs *f)
{
	struct v6params *v6p;

	v6p = kzmalloc(sizeof(struct v6params), 0);

	v6p->rp.mflag = 0;	/* default not managed */
	v6p->rp.oflag = 0;
	v6p->rp.maxraint = 600000;	/* millisecs */
	v6p->rp.minraint = 200000;
	v6p->rp.linkmtu = 0;	/* no mtu sent */
	v6p->rp.reachtime = 0;
	v6p->rp.rxmitra = 0;
	v6p->rp.ttl = MAXTTL;
	v6p->rp.routerlt = 3 * v6p->rp.maxraint;

	v6p->hp.rxmithost = 1000;	/* v6 RETRANS_TIMER */

	v6p->cdrouter = -1;

	f->v6p = v6p;
}

void initfrag(struct IP *ip, int size)
{
	struct fragment4 *fq4, *eq4;
	struct fragment6 *fq6, *eq6;

	ip->fragfree4 =
		(struct fragment4 *)kzmalloc(sizeof(struct fragment4) * size, 0);
	if (ip->fragfree4 == NULL)
		panic("initfrag");

	eq4 = &ip->fragfree4[size];
	for (fq4 = ip->fragfree4; fq4 < eq4; fq4++)
		fq4->next = fq4 + 1;

	ip->fragfree4[size - 1].next = NULL;

	ip->fragfree6 =
		(struct fragment6 *)kzmalloc(sizeof(struct fragment6) * size, 0);
	if (ip->fragfree6 == NULL)
		panic("initfrag");

	eq6 = &ip->fragfree6[size];
	for (fq6 = ip->fragfree6; fq6 < eq6; fq6++)
		fq6->next = fq6 + 1;

	ip->fragfree6[size - 1].next = NULL;
}

void ip_init(struct fs *f)
{
	struct IP *ip;

	ip = kzmalloc(sizeof(struct IP), 0);
	initfrag(ip, 100);
	f->ip = ip;

	ip_init_6(f);
}

void iprouting(struct fs *f, int on)
{
	f->ip->iprouting = on;
	if (f->ip->iprouting == 0)
		f->ip->stats[Forwarding] = 2;
	else
		f->ip->stats[Forwarding] = 1;
}

int
ipoput4(struct fs *f,
		struct block *bp, int gating, int ttl, int tos, struct conv *c)
{
	ERRSTACK(2);
	struct ipifc *ifc;
	uint8_t *gate;
	uint32_t fragoff;
	struct block *xp, *nb;
	struct Ip4hdr *eh, *feh;
	int lid, len, seglen, chunk, dlen, blklen, offset, medialen;
	struct route *r, *sr;
	struct IP *ip;
	int rv = 0;

	ip = f->ip;

	/* Fill out the ip header */
	eh = (struct Ip4hdr *)(bp->rp);

	ip->stats[OutRequests]++;

	/* Number of uint8_ts in data and ip header to write */
	len = blocklen(bp);

	if (gating) {
		chunk = nhgets(eh->length);
		if (chunk > len) {
			ip->stats[OutDiscards]++;
			netlog(f, Logip, "short gated packet\n");
			goto free;
		}
		if (chunk < len)
			len = chunk;
	}
	if (len >= IP_MAX) {
		ip->stats[OutDiscards]++;
		netlog(f, Logip, "exceeded ip max size %V\n", eh->dst);
		goto free;
	}

	r = v4lookup(f, eh->dst, c);
	if (r == NULL) {
		ip->stats[OutNoRoutes]++;
		netlog(f, Logip, "no interface %V\n", eh->dst);
		rv = -1;
		goto free;
	}

	ifc = r->routeTree.ifc;
	if (r->routeTree.type & (Rifc | Runi))
		gate = eh->dst;
	else if (r->routeTree.type & (Rbcast | Rmulti)) {
		gate = eh->dst;
		sr = v4lookup(f, eh->src, NULL);
		if (sr != NULL && (sr->routeTree.type & Runi))
			ifc = sr->routeTree.ifc;
	} else
		gate = r->v4.gate;

	if (!gating)
		eh->vihl = IP_VER4 | IP_HLEN4;
	eh->ttl = ttl;
	if (!gating)
		eh->tos = tos;

	if (!canrlock(&ifc->rwlock))
		goto free;
	if (waserror()) {
		runlock(&ifc->rwlock);
		nexterror();
	}
	if (ifc->medium == NULL)
		goto raise;

	/* If we dont need to fragment just send it */
	if (c && c->maxfragsize && c->maxfragsize < ifc->maxtu)
		medialen = c->maxfragsize - ifc->medium->hsize;
	else
		medialen = ifc->maxtu - ifc->medium->hsize;
	if (len <= medialen) {
		if (!gating) {
			/* TODO: not sure what you want here.  The only thing we can
			 * guarantee about the ref you read is that it is > 0. */
			kref_get(&ip->id4, 1);
			hnputs(eh->id, kref_refcnt(&ip->id4));
		}
		hnputs(eh->length, len);
		if (!gating) {
			eh->frag[0] = 0;
			eh->frag[1] = 0;
		}
		eh->cksum[0] = 0;
		eh->cksum[1] = 0;
		hnputs(eh->cksum, ipcsum(&eh->vihl));
		assert(bp->next == NULL);
		ifc->medium->bwrite(ifc, bp, V4, gate);
		runlock(&ifc->rwlock);
		poperror();
		return 0;
	}

	if ((eh->frag[0] & (IP_DF >> 8)) && !gating)
		printd("%V: DF set\n", eh->dst);

	if (eh->frag[0] & (IP_DF >> 8)) {
		ip->stats[FragFails]++;
		ip->stats[OutDiscards]++;
		icmpcantfrag(f, bp, medialen);
		netlog(f, Logip, "%V: eh->frag[0] & (IP_DF>>8)\n", eh->dst);
		goto raise;
	}

	seglen = (medialen - IP4HDR) & ~7;
	if (seglen < 8) {
		ip->stats[FragFails]++;
		ip->stats[OutDiscards]++;
		netlog(f, Logip, "%V seglen < 8\n", eh->dst);
		goto raise;
	}

	dlen = len - IP4HDR;
	xp = bp;
	if (gating) {
		lid = nhgets(eh->id);
	} else {
		/* TODO: not sure what you want here.  The only thing we can
		 * guarantee about the ref you read is that it is > 0. */
		kref_get(&ip->id4, 1);
		lid = kref_refcnt(&ip->id4);
	}

	offset = IP4HDR;
	while (xp != NULL && offset && offset >= BLEN(xp)) {
		offset -= BLEN(xp);
		xp = xp->next;
	}
	xp->rp += offset;

	if (gating)
		fragoff = nhgets(eh->frag) << 3;
	else
		fragoff = 0;
	dlen += fragoff;
	for (; fragoff < dlen; fragoff += seglen) {
		nb = allocb(IP4HDR + seglen);
		feh = (struct Ip4hdr *)(nb->rp);

		memmove(nb->wp, eh, IP4HDR);
		nb->wp += IP4HDR;

		if ((fragoff + seglen) >= dlen) {
			seglen = dlen - fragoff;
			hnputs(feh->frag, fragoff >> 3);
		} else
			hnputs(feh->frag, (fragoff >> 3) | IP_MF);

		hnputs(feh->length, seglen + IP4HDR);
		hnputs(feh->id, lid);

		/* Copy up the data area */
		chunk = seglen;
		while (chunk) {
			if (!xp) {
				ip->stats[OutDiscards]++;
				ip->stats[FragFails]++;
				freeblist(nb);
				netlog(f, Logip, "!xp: chunk %d\n", chunk);
				goto raise;
			}
			blklen = chunk;
			if (BLEN(xp) < chunk)
				blklen = BLEN(xp);
			memmove(nb->wp, xp->rp, blklen);
			nb->wp += blklen;
			xp->rp += blklen;
			chunk -= blklen;
			if (xp->rp == xp->wp)
				xp = xp->next;
		}

		feh->cksum[0] = 0;
		feh->cksum[1] = 0;
		hnputs(feh->cksum, ipcsum(&feh->vihl));
		ifc->medium->bwrite(ifc, nb, V4, gate);
		ip->stats[FragCreates]++;
	}
	ip->stats[FragOKs]++;
raise:
	runlock(&ifc->rwlock);
	poperror();
free:
	freeblist(bp);
	return rv;
}

void ipiput4(struct fs *f, struct ipifc *ifc, struct block *bp)
{
	int hl;
	int hop, tos, proto, olen;
	struct Ip4hdr *h;
	struct proto *p;
	uint16_t frag;
	int notforme;
	uint8_t *dp, v6dst[IPaddrlen];
	struct IP *ip;
	struct route *r;
	struct conv conv;

	if (BLKIPVER(bp) != IP_VER4) {
		ipiput6(f, ifc, bp);
		return;
	}

	ip = f->ip;
	ip->stats[InReceives]++;

	/*
	 *  Ensure we have all the header info in the first
	 *  block.  Make life easier for other protocols by
	 *  collecting up to the first 64 bytes in the first block.
	 */
	if (BLEN(bp) < 64) {
		hl = blocklen(bp);
		if (hl < IP4HDR)
			hl = IP4HDR;
		if (hl > 64)
			hl = 64;
		bp = pullupblock(bp, hl);
		if (bp == NULL)
			return;
	}

	h = (struct Ip4hdr *)(bp->rp);

	/* dump anything that whose header doesn't checksum */
	if ((bp->flag & Bipck) == 0 && ipcsum(&h->vihl)) {
		ip->stats[InHdrErrors]++;
		netlog(f, Logip, "ip: checksum error %V\n", h->src);
		freeblist(bp);
		return;
	}
	v4tov6(v6dst, h->dst);
	notforme = ipforme(f, v6dst) == 0;

	/* Check header length and version */
	if ((h->vihl & 0x0F) != IP_HLEN4) {
		hl = (h->vihl & 0xF) << 2;
		if (hl < (IP_HLEN4 << 2)) {
			ip->stats[InHdrErrors]++;
			netlog(f, Logip, "ip: %V bad hivl %ux\n", h->src, h->vihl);
			freeblist(bp);
			return;
		}
		/* If this is not routed strip off the options */
		if (notforme == 0) {
			olen = nhgets(h->length);
			dp = bp->rp + (hl - (IP_HLEN4 << 2));
			memmove(dp, h, IP_HLEN4 << 2);
			bp->rp = dp;
			h = (struct Ip4hdr *)(bp->rp);
			h->vihl = (IP_VER4 | IP_HLEN4);
			hnputs(h->length, olen - hl + (IP_HLEN4 << 2));
		}
	}

	/* route */
	if (notforme) {
		if (!ip->iprouting) {
			freeblist(bp);
			return;
		}

		/* don't forward to source's network */
		memset(&conv, 0, sizeof conv);
		conv.r = NULL;
		r = v4lookup(f, h->dst, &conv);
		if (r == NULL || r->routeTree.ifc == ifc) {
			ip->stats[OutDiscards]++;
			freeblist(bp);
			return;
		}

		/* don't forward if packet has timed out */
		hop = h->ttl;
		if (hop < 1) {
			ip->stats[InHdrErrors]++;
			icmpttlexceeded(f, ifc->lifc->local, bp);
			freeblist(bp);
			return;
		}

		/* reassemble if the interface expects it */
		if (r->routeTree.ifc == NULL)
			panic("NULL route rfc");
		if (r->routeTree.ifc->reassemble) {
			frag = nhgets(h->frag);
			if (frag) {
				h->tos = 0;
				if (frag & IP_MF)
					h->tos = 1;
				bp = ip4reassemble(ip, frag, bp, h);
				if (bp == NULL)
					return;
				h = (struct Ip4hdr *)(bp->rp);
			}
		}

		ip->stats[ForwDatagrams]++;
		tos = h->tos;
		hop = h->ttl;
		ipoput4(f, bp, 1, hop - 1, tos, &conv);
		return;
	}

	frag = nhgets(h->frag);
	if (frag) {
		h->tos = 0;
		if (frag & IP_MF)
			h->tos = 1;
		bp = ip4reassemble(ip, frag, bp, h);
		if (bp == NULL)
			return;
		h = (struct Ip4hdr *)(bp->rp);
	}

	/* don't let any frag info go up the stack */
	h->frag[0] = 0;
	h->frag[1] = 0;

	proto = h->proto;
	p = Fsrcvpcol(f, proto);
	if (p != NULL && p->rcv != NULL) {
		ip->stats[InDelivers]++;
		(*p->rcv) (p, ifc, bp);
		return;
	}
	ip->stats[InDiscards]++;
	ip->stats[InUnknownProtos]++;
	freeblist(bp);
}

int ipstats(struct fs *f, char *buf, int len)
{
	struct IP *ip;
	char *p, *e;
	int i;

	ip = f->ip;
	ip->stats[DefaultTTL] = MAXTTL;

	p = buf;
	e = p + len;
	for (i = 0; i < Nipstats; i++)
		p = seprintf(p, e, "%s: %llud\n", statnames[i], ip->stats[i]);
	return p - buf;
}

struct block *ip4reassemble(struct IP *ip, int offset, struct block *bp,
							struct Ip4hdr *ih)
{
	int fend;
	uint16_t id;
	struct fragment4 *f, *fnext;
	uint32_t src, dst;
	struct block *bl, **l, *last, *prev;
	int ovlap, len, fragsize, pktposn;

	src = nhgetl(ih->src);
	dst = nhgetl(ih->dst);
	id = nhgets(ih->id);

	/*
	 *  block lists are too hard, pullupblock into a single block
	 */
	if (bp->next) {
		bp = pullupblock(bp, blocklen(bp));
		ih = (struct Ip4hdr *)(bp->rp);
	}

	qlock(&ip->fraglock4);

	/*
	 *  find a reassembly queue for this fragment
	 */
	for (f = ip->flisthead4; f; f = fnext) {
		fnext = f->next;	/* because ipfragfree4 changes the list */
		if (f->src == src && f->dst == dst && f->id == id)
			break;
		if (f->age < NOW) {
			ip->stats[ReasmTimeout]++;
			ipfragfree4(ip, f);
		}
	}

	/*
	 *  if this isn't a fragmented packet, accept it
	 *  and get rid of any fragments that might go
	 *  with it.
	 */
	if (!ih->tos && (offset & ~(IP_MF | IP_DF)) == 0) {
		if (f != NULL) {
			ipfragfree4(ip, f);
			ip->stats[ReasmFails]++;
		}
		qunlock(&ip->fraglock4);
		return bp;
	}

	if (bp->base + IPFRAGSZ >= bp->rp) {
		bp = padblock(bp, IPFRAGSZ);
		bp->rp += IPFRAGSZ;
	}

	BKFG(bp)->foff = offset << 3;
	BKFG(bp)->flen = nhgets(ih->length) - IP4HDR;

	/* First fragment allocates a reassembly queue */
	if (f == NULL) {
		f = ipfragallo4(ip);
		f->id = id;
		f->src = src;
		f->dst = dst;

		f->blist = bp;

		qunlock(&ip->fraglock4);
		ip->stats[ReasmReqds]++;
		return NULL;
	}

	/*
	 *  find the new fragment's position in the queue
	 */
	prev = NULL;
	l = &f->blist;
	bl = f->blist;
	while (bl != NULL && BKFG(bp)->foff > BKFG(bl)->foff) {
		prev = bl;
		l = &bl->next;
		bl = bl->next;
	}

	/* Check overlap of a previous fragment - trim away as necessary */
	if (prev) {
		ovlap = BKFG(prev)->foff + BKFG(prev)->flen - BKFG(bp)->foff;
		if (ovlap > 0) {
			if (ovlap >= BKFG(bp)->flen) {
				freeblist(bp);
				qunlock(&ip->fraglock4);
				return NULL;
			}
			BKFG(prev)->flen -= ovlap;
		}
	}

	/* Link onto assembly queue */
	bp->next = *l;
	*l = bp;

	/* Check to see if succeeding segments overlap */
	if (bp->next) {
		l = &bp->next;
		fend = BKFG(bp)->foff + BKFG(bp)->flen;
		/* Take completely covered segments out */
		while (*l) {
			ovlap = fend - BKFG(*l)->foff;
			if (ovlap <= 0)
				break;
			if (ovlap < BKFG(*l)->flen) {
				BKFG(*l)->flen -= ovlap;
				BKFG(*l)->foff += ovlap;
				/* move up ih hdrs */
				memmove((*l)->rp + ovlap, (*l)->rp, IP4HDR);
				(*l)->rp += ovlap;
				break;
			}
			last = (*l)->next;
			(*l)->next = NULL;
			freeblist(*l);
			*l = last;
		}
	}

	/*
	 *  look for a complete packet.  if we get to a fragment
	 *  without IP_MF set, we're done.
	 */
	pktposn = 0;
	for (bl = f->blist; bl; bl = bl->next) {
		if (BKFG(bl)->foff != pktposn)
			break;
		if ((BLKIP(bl)->frag[0] & (IP_MF >> 8)) == 0) {
			bl = f->blist;
			len = nhgets(BLKIP(bl)->length);
			bl->wp = bl->rp + len;

			/* Pullup all the fragment headers and
			 * return a complete packet
			 */
			for (bl = bl->next; bl; bl = bl->next) {
				fragsize = BKFG(bl)->flen;
				len += fragsize;
				bl->rp += IP4HDR;
				bl->wp = bl->rp + fragsize;
			}

			bl = f->blist;
			f->blist = NULL;
			ipfragfree4(ip, f);
			ih = BLKIP(bl);
			hnputs(ih->length, len);
			qunlock(&ip->fraglock4);
			ip->stats[ReasmOKs]++;
			return bl;
		}
		pktposn += BKFG(bl)->flen;
	}
	qunlock(&ip->fraglock4);
	return NULL;
}

/*
 * ipfragfree4 - Free a list of fragments - assume hold fraglock4
 */
void ipfragfree4(struct IP *ip, struct fragment4 *frag)
{
	struct fragment4 *fl, **l;

	if (frag->blist)
		freeblist(frag->blist);

	frag->src = 0;
	frag->id = 0;
	frag->blist = NULL;

	l = &ip->flisthead4;
	for (fl = *l; fl; fl = fl->next) {
		if (fl == frag) {
			*l = frag->next;
			break;
		}
		l = &fl->next;
	}

	frag->next = ip->fragfree4;
	ip->fragfree4 = frag;

}

/*
 * ipfragallo4 - allocate a reassembly queue - assume hold fraglock4
 */
struct fragment4 *ipfragallo4(struct IP *ip)
{
	struct fragment4 *f;

	while (ip->fragfree4 == NULL) {
		/* free last entry on fraglist */
		for (f = ip->flisthead4; f->next; f = f->next) ;
		ipfragfree4(ip, f);
	}
	f = ip->fragfree4;
	ip->fragfree4 = f->next;
	f->next = ip->flisthead4;
	ip->flisthead4 = f;
	f->age = NOW + 30000;

	return f;
}

uint16_t ipcsum(uint8_t * addr)
{
	int len;
	uint32_t sum;

	sum = 0;
	len = (addr[0] & 0xf) << 2;

	while (len > 0) {
		sum += addr[0] << 8 | addr[1];
		len -= 2;
		addr += 2;
	}

	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);

	return (sum ^ 0xffff);
}
