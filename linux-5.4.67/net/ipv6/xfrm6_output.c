// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * xfrm6_output.c - Common IPsec encapsulation code for IPv6.
 * Copyright (C) 2002 USAGI/WIDE Project
 * Copyright (c) 2004 Herbert Xu <herbert@gondor.apana.org.au>
 */

#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/icmpv6.h>
#include <linux/netfilter_ipv6.h>
#include <net/dst.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#include <net/xfrm.h>

int xfrm6_find_1stfragopt(struct xfrm_state *x, struct sk_buff *skb,
			  u8 **prevhdr)
{
	return ip6_find_1stfragopt(skb, prevhdr);
}
EXPORT_SYMBOL(xfrm6_find_1stfragopt);

static int xfrm6_local_dontfrag(struct sk_buff *skb)
{
	int proto;
	struct sock *sk = skb->sk;

	if (sk) {
		if (sk->sk_family != AF_INET6)
			return 0;

		proto = sk->sk_protocol;
		if (proto == IPPROTO_UDP || proto == IPPROTO_RAW)
			return inet6_sk(sk)->dontfrag;
	}

	return 0;
}

static void xfrm6_local_rxpmtu(struct sk_buff *skb, u32 mtu)
{
	struct flowi6 fl6;
	struct sock *sk = skb->sk;

	fl6.flowi6_oif = sk->sk_bound_dev_if;
	fl6.daddr = ipv6_hdr(skb)->daddr;

	ipv6_local_rxpmtu(sk, &fl6, mtu);
}

void xfrm6_local_error(struct sk_buff *skb, u32 mtu)
{
	struct flowi6 fl6;
	const struct ipv6hdr *hdr;
	struct sock *sk = skb->sk;

	hdr = skb->encapsulation ? inner_ipv6_hdr(skb) : ipv6_hdr(skb);
	fl6.fl6_dport = inet_sk(sk)->inet_dport;
	fl6.daddr = hdr->daddr;

	ipv6_local_error(sk, EMSGSIZE, &fl6, mtu);
}

static int xfrm6_tunnel_check_size(struct sk_buff *skb)
{
	int mtu, ret = 0;
	struct dst_entry *dst = skb_dst(skb);

	if (skb->ignore_df)
		goto out;

	mtu = dst_mtu(dst);
	if (mtu < IPV6_MIN_MTU)
		mtu = IPV6_MIN_MTU;

	if ((!skb_is_gso(skb) && skb->len > mtu) ||
	    (skb_is_gso(skb) &&
	     !skb_gso_validate_network_len(skb, ip6_skb_dst_mtu(skb)))) {
		skb->dev = dst->dev;
		skb->protocol = htons(ETH_P_IPV6);

		if (xfrm6_local_dontfrag(skb))
			xfrm6_local_rxpmtu(skb, mtu);
		else if (skb->sk)
			xfrm_local_error(skb, mtu);
		else
			icmpv6_send(skb, ICMPV6_PKT_TOOBIG, 0, mtu);
		ret = -EMSGSIZE;
	}
out:
	return ret;
}

int xfrm6_extract_output(struct xfrm_state *x, struct sk_buff *skb)
{
	int err;

	err = xfrm6_tunnel_check_size(skb);
	if (err)
		return err;

	XFRM_MODE_SKB_CB(skb)->protocol = ipv6_hdr(skb)->nexthdr;

	return xfrm6_extract_header(skb);
}

int xfrm6_output_finish(struct sock *sk, struct sk_buff *skb)
{
	memset(IP6CB(skb), 0, sizeof(*IP6CB(skb)));

	IP6CB(skb)->flags |= IP6SKB_XFRM_TRANSFORMED;

	return xfrm_output(sk, skb);
}

static int __xfrm6_output_state_finish(struct xfrm_state *x, struct sock *sk,
				       struct sk_buff *skb)
{
	const struct xfrm_state_afinfo *afinfo;
	int ret = -EAFNOSUPPORT;

	rcu_read_lock();
	afinfo = xfrm_state_afinfo_get_rcu(x->outer_mode.family);
	if (likely(afinfo))
		ret = afinfo->output_finish(sk, skb);
	else
		kfree_skb(skb);
	rcu_read_unlock();

	return ret;
}

static int __xfrm6_output_finish(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	struct xfrm_state *x = skb_dst(skb)->xfrm;

	return __xfrm6_output_state_finish(x, sk, skb);
}

static int __xfrm6_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct xfrm_state *x = dst->xfrm;
	int mtu;
	bool toobig;

#ifdef CONFIG_NETFILTER
	if (!x) {
		IP6CB(skb)->flags |= IP6SKB_REROUTED;
		return dst_output(net, sk, skb);
	}
#endif

	if (x->props.mode != XFRM_MODE_TUNNEL)
		goto skip_frag;

	if (skb->protocol == htons(ETH_P_IPV6))
		mtu = ip6_skb_dst_mtu(skb);
	else
		mtu = dst_mtu(skb_dst(skb));

	toobig = skb->len > mtu && !skb_is_gso(skb);

	if (toobig && xfrm6_local_dontfrag(skb)) {
		xfrm6_local_rxpmtu(skb, mtu);
		kfree_skb(skb);
		return -EMSGSIZE;
	} else if (!skb->ignore_df && toobig && skb->sk) {
		xfrm_local_error(skb, mtu);
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	if (toobig || dst_allfrag(skb_dst(skb)))
		return ip6_fragment(net, sk, skb,
				    __xfrm6_output_finish);

skip_frag:
	return __xfrm6_output_state_finish(x, sk, skb);
}

int xfrm6_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	return NF_HOOK_COND(NFPROTO_IPV6, NF_INET_POST_ROUTING,
			    net, sk, skb,  NULL, skb_dst(skb)->dev,
			    __xfrm6_output,
			    !(IP6CB(skb)->flags & IP6SKB_REROUTED));
}
