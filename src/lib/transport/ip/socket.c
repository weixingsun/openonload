/*
** Copyright 2005-2014  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/************************************************************************** \
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  djr
**  \brief  ci_sock_cmn routines.
**   \date  2010/11/22
**    \cop  (c) Solarflare Communications, Inc.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_lib_transport_ip */
#include "ip_internal.h"
#include <ci/internal/cplane_ops.h>


void ci_sock_cmn_reinit(ci_netif* ni, ci_sock_cmn* s)
{
  s->so_error = 0;

  s->tx_errno = EPIPE;

  s->rx_errno = ENOTCONN;
  s->pkt.ether_type = CI_ETHERTYPE_IP;
  ci_ip_cache_init(&s->pkt);
}




void oo_sock_cplane_init(struct oo_sock_cplane* cp)
{
  cp->ip_laddr_be32 = 0;
  cp->lport_be16 = 0;
  cp->so_bindtodevice = CI_IFID_BAD;
  cp->ip_multicast_if = CI_IFID_BAD;
  cp->ip_multicast_if_laddr_be32 = 0;
  cp->ip_ttl = CI_IP_DFLT_TTL;
  cp->ip_mcast_ttl = 1;
  cp->sock_cp_flags = 0;
}


void ci_sock_cmn_init(ci_netif* ni, ci_sock_cmn* s)
{
  oo_p sp;

  /* Poison. */
  CI_DEBUG(memset(&s->b + 1, 0xf0, (char*) (s + 1) - (char*) (&s->b + 1)));

  citp_waitable_reinit(ni, &s->b);
  oo_sock_cplane_init(&s->cp);
  s->local_peer = OO_SP_NULL;

  s->s_flags = CI_SOCK_FLAG_CONNECT_MUST_BIND | CI_SOCK_FLAG_PMTU_DO;
  s->s_aflags = 0u;

  ci_assert_equal( 0, CI_IP_DFLT_TOS );
  s->so_priority = 0;

  /* SO_SNDBUF & SO_RCVBUF.  See also ci_tcp_set_established_state() which
   * may modify these values.
   */
  memset(&s->so, 0, sizeof(s->so));
  s->so.sndbuf = NI_OPTS(ni).tcp_sndbuf_def;
  s->so.rcvbuf = NI_OPTS(ni).tcp_rcvbuf_def;

  s->rx_bind2dev_ifindex = CI_IFID_BAD;
  /* These don't really need to be initialised, as only significant when
   * rx_bind2dev_ifindex != CI_IFID_BAD.  But makes stackdump output
   * cleaner this way...
   */
  s->rx_bind2dev_base_ifindex = 0;
  s->rx_bind2dev_vlan = 0;

  s->cmsg_flags = 0u;
  s->timestamping_flags = 0u;
  s->os_sock_status = OO_OS_STATUS_TX;

  ci_ip_queue_init(&s->timestamp_q);
  s->timestamp_q_extract = OO_PP_NULL;


  ci_sock_cmn_reinit(ni, s);

  sp = oo_sockp_to_statep(ni, SC_SP(s));
  OO_P_ADD(sp, CI_MEMBER_OFFSET(ci_sock_cmn, reap_link));
  ci_ni_dllist_link_init(ni, &s->reap_link, sp, "reap");
  ci_ni_dllist_self_link(ni, &s->reap_link);
}


static int ci_sock_cmn_timestamp_q_reapable(ci_netif* ni, ci_sock_cmn* s)
{
  int count = 0;
  oo_pkt_p p = s->timestamp_q.head;
  while( ! OO_PP_EQ(p, s->timestamp_q_extract) ) {
    ++count;
    p = PKT_CHK(ni, p)->tsq_next;
  }
  return count;
}


void ci_sock_cmn_timestamp_q_drop(ci_netif* netif, ci_sock_cmn* s)
{
  ci_ip_pkt_queue* qu = &s->timestamp_q;
  ci_ip_pkt_fmt* p;
  CI_DEBUG(int i = qu->num);

  ci_assert(netif);
  ci_assert(qu);

  while( OO_PP_NOT_NULL(qu->head)   CI_DEBUG( && i-- > 0) ) {
    p = PKT_CHK(netif, qu->head);
    qu->head = p->tsq_next;
    ci_netif_pkt_release(netif, p);
  }
  ci_assert_equal(i, 0);
  ci_assert(OO_PP_IS_NULL(qu->head));
  qu->num = 0;
}


void ci_sock_cmn_timestamp_q_enqueue(ci_netif* ni, ci_sock_cmn* s,
                                     ci_ip_pkt_fmt* pkt)
{
  ci_ip_pkt_queue* qu = &s->timestamp_q;
  oo_pkt_p prev_head = qu->head;

  /* This part is effectively ci_ip_queue_enqueue(ni, &s->timestamp_q, p);
   * but inlined to allow using tsq_next field 
   */
  pkt->tsq_next = OO_PP_NULL;
  if( ci_ip_queue_is_empty(qu) ) {
    ci_assert(OO_PP_IS_NULL(qu->head));
    qu->head = OO_PKT_P(pkt);
  }
  else {
    ci_assert(OO_PP_NOT_NULL(qu->head));
    /* This assumes the netif lock is held, so use
       ci_ip_queue_enqueue_nnl() if it's not */
    PKT(ni, qu->tail)->tsq_next = OO_PKT_P(pkt);
  }
  qu->tail = OO_PKT_P(pkt);
  qu->num++;


  if( OO_PP_IS_NULL(prev_head) ) {
    ci_assert(OO_PP_IS_NULL(s->timestamp_q_extract));
    s->timestamp_q_extract = qu->head;
  } 
  else {
    ci_sock_cmn_timestamp_q_reap(ni, s);
  }

  /* Tells post-poll loop to put socket on the [reap_list]. */
  s->b.sb_flags |= CI_SB_FLAG_RX_DELIVERED;
}


void ci_sock_cmn_timestamp_q_reap(ci_netif* ni, ci_sock_cmn* s)
{ 
  ci_assert(ci_netif_is_locked(ni));
  while( ! OO_PP_EQ(s->timestamp_q.head, s->timestamp_q_extract) ) {
    ci_ip_pkt_fmt* pkt = PKT_CHK(ni, s->timestamp_q.head);
    oo_pkt_p next = pkt->tsq_next;

    ci_netif_pkt_release(ni, pkt);
    --s->timestamp_q.num;
    s->timestamp_q.head = next;
  }
}




void ci_sock_cmn_dump(ci_netif* ni, ci_sock_cmn* s, const char* pf)
{
  int ts_q_reapable = ci_sock_cmn_timestamp_q_reapable(ni, s);
  log("%s  s_flags: "CI_SOCK_FLAGS_FMT, pf,
      CI_SOCK_FLAGS_PRI_ARG(s));
  log("%s  rcvbuf=%d sndbuf=%d bindtodev=%d(%d,%d:%d) ttl=%d", pf,
      s->so.rcvbuf, s->so.sndbuf, s->cp.so_bindtodevice,
      s->rx_bind2dev_ifindex, s->rx_bind2dev_base_ifindex,
      s->rx_bind2dev_vlan, s->cp.ip_ttl);
  log("%s  rcvtimeo_ms=%d sndtimeo_ms=%d sigown=%d "
      "cmsg="OO_CMSG_FLAGS_FMT"%s",
      pf, s->so.rcvtimeo_msec, s->so.sndtimeo_msec, s->b.sigown,
      OO_CMSG_FLAGS_PRI_ARG(s->cmsg_flags),
      (s->cp.sock_cp_flags & OO_SCP_NO_MULTICAST) ? " NO_MCAST_TX":"");
  log("%s  rx_errno=%x tx_errno=%x so_error=%d os_sock=%u%s%s", pf,
      s->rx_errno, s->tx_errno, s->so_error,
      s->os_sock_status >> OO_OS_STATUS_SEQ_SHIFT,
      (s->os_sock_status & OO_OS_STATUS_RX) ? ",RX":"",
      (s->os_sock_status & OO_OS_STATUS_TX) ? ",TX":"");
  if( s->timestamping_flags & ONLOAD_SOF_TIMESTAMPING_TX_HARDWARE )
    log("%s  TX timestamping queue: packets %d reap %d extract %d", pf, 
        s->timestamp_q.num - ts_q_reapable, ts_q_reapable, OO_PP_ID(s->timestamp_q_extract));
}
