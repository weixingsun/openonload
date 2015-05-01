/*
** Copyright 2005-2014  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of version 2.1 of the GNU Lesser General Public
** License as published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
*/

/****************************************************************************
 * Copyright 2002-2005: Level 5 Networks Inc.
 * Copyright 2005-2008: Solarflare Communications Inc,
 *                      9501 Jeronimo Road, Suite 250,
 *                      Irvine, CA 92618, USA
 *
 * Maintained by Solarflare Communications
 *  <linux-xen-drivers@solarflare.com>
 *  <onload-dev@solarflare.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************
 */

/*
 * \author  djr
 *  \brief  Routine to poll event queues.
 *   \date  2003/03/04
 */

/*! \cidoxg_lib_ef */
#include "ef_vi_internal.h"
#include "logging.h"
#include "mcdi_pcol.h"

typedef ci_qword_t ef_vi_event;

#define EF_VI_EVENT_OFFSET(q, i)					\
	(((q)->ep_state->evq.evq_ptr + (i) * sizeof(ef_vi_qword)) &	\
	 (q)->evq_mask)

#define EF_VI_EVENT_PTR(q, i)                                           \
	((ef_vi_event*) ((q)->evq_base + EF_VI_EVENT_OFFSET((q), (i))))



/* Due to crazy chipsets, we see the event words being written in
** arbitrary order (bug4539).  So test for presence of event must ensure
** that both halves have changed from the null.
*/
#define EF_VI_IS_EVENT(evp)				\
        (!(CI_DWORD_IS_ALL_ONES((evp)->dword[0]) |	\
           CI_DWORD_IS_ALL_ONES((evp)->dword[1])))


#define INC_ERROR_STAT(vi, name)		\
	do {					\
		if ((vi)->vi_stats != NULL)	\
			++(vi)->vi_stats->name;	\
	} while (0)


ef_vi_inline void huntington_rx_desc_consumed(ef_vi* vi, const ef_vi_event* ev,
					      ef_event** evs, int* evs_len,
					      int q_label, int desc_i)
{
	ef_vi_rxq_state* qs = &vi->ep_state->rxq;
	/* ABORT bit not included in this as it is not set by fw */
	const ci_uint32 discard_mask =
		CI_BSWAPC_LE32(1 << ESF_DZ_RX_ECC_ERR_LBN |
			       1 << ESF_DZ_RX_CRC1_ERR_LBN |
			       1 << ESF_DZ_RX_CRC0_ERR_LBN |
			       1 << ESF_DZ_RX_TCPUDP_CKSUM_ERR_LBN |
			       1 << ESF_DZ_RX_IPCKSUM_ERR_LBN |
			       1 << ESF_DZ_RX_ECRC_ERR_LBN);
	ef_event* ev_out = (*evs)++;
	unsigned rx_bytes;

	--(*evs_len);
	ev_out->rx.q_id = q_label;
	ev_out->rx.rq_id = vi->vi_rxq.ids[desc_i];
	vi->vi_rxq.ids[desc_i] = EF_REQUEST_ID_MASK;  /* ?? killme */

	rx_bytes = QWORD_GET_U(ESF_DZ_RX_BYTES, *ev);

	ev_out->rx.type = EF_EVENT_TYPE_RX;
	if( ! qs->in_jumbo ) {
		ev_out->rx.flags = EF_EVENT_FLAG_SOP;
		qs->bytes_acc = rx_bytes;
        }
	else {
		ev_out->rx.flags = 0;
		qs->bytes_acc += rx_bytes;
        }
	if( ! QWORD_GET_U(ESF_DZ_RX_CONT, *ev) )
		qs->in_jumbo = 0;
        else {
		ev_out->rx.flags |= EF_EVENT_FLAG_CONT;
		++qs->in_jumbo;
	}
	ev_out->rx.len = qs->bytes_acc;

	if( QWORD_GET_U(ESF_DZ_RX_MAC_CLASS, *ev) == ESE_DZ_MAC_CLASS_MCAST) {
		ev_out->rx.flags |= EF_EVENT_FLAG_MULTICAST;
	}

	/* Consider rx_bytes == 0 to indicate that the abort bit
	 * should have been set but wasn't - i.e. it's a frame
	 * trunc 
	 */
	if(likely( ! ((ev->u32[0] & discard_mask) || (rx_bytes == 0)) )) {
		++vi->ep_state->rxq.removed;
		return;
	}
	if( rx_bytes == 0 ) {
		/* If this is an abort then we didn't really consume a
		 * descriptor, so don't increment removed count.
		 */
		ev_out->rx_discard.type = EF_EVENT_TYPE_RX_NO_DESC_TRUNC;
		return;
	}
	ev_out->rx_discard.len = qs->bytes_acc;
	ev_out->rx_discard.type = EF_EVENT_TYPE_RX_DISCARD;

	if( QWORD_GET_U(ESF_DZ_RX_ECC_ERR, *ev) |
	    QWORD_GET_U(ESF_DZ_RX_CRC1_ERR, *ev) |
	    QWORD_GET_U(ESF_DZ_RX_CRC1_ERR, *ev) |
	    QWORD_GET_U(ESF_DZ_RX_ECRC_ERR, *ev) )
		ev_out->rx_discard.subtype = EF_EVENT_RX_DISCARD_CRC_BAD;
	else
		/* TCPUDP_CKSUM or IPCKSUM error
		 */
		ev_out->rx_discard.subtype = EF_EVENT_RX_DISCARD_CSUM_BAD;

	++vi->ep_state->rxq.removed;
}


ef_vi_inline void ef10_rx_event(ef_vi* evq_vi, const ef_vi_event* ev,
				ef_event** evs, int* evs_len)
{
	unsigned lbits_mask = __EFVI_MASK(ESF_DZ_RX_DSC_PTR_LBITS_WIDTH,
					  unsigned);
	unsigned q_label = QWORD_GET_U(ESF_DZ_RX_QLABEL, *ev);
	unsigned short_di, desc_i, q_mask;
	ef_vi *vi;

	vi = evq_vi->vi_qs[q_label];
	if (likely(vi != NULL)) {
		q_mask = vi->vi_rxq.mask;
		short_di = QWORD_GET_U(ESF_DZ_RX_DSC_PTR_LBITS, *ev);
		desc_i = (vi->ep_state->rxq.removed +
			  ((short_di - vi->ep_state->rxq.removed) &
			   lbits_mask) - 1) & q_mask;
		huntington_rx_desc_consumed(vi, ev, evs, evs_len,
					    q_label, desc_i);
	} else {
		INC_ERROR_STAT(evq_vi, rx_ev_bad_q_label);
	}
}


static uint32_t timestamp_extract(ef_vi_event ev)
{
	uint32_t lo = QWORD_GET_U(ESF_DZ_TX_DESCR_INDX, ev);
	uint32_t hi = QWORD_GET_U(ESF_DZ_TX_SOFT2, ev);
	return (hi << 16) | lo;
}


static void ef10_tx_event_ts_enabled(ef_vi* evq, const ef_vi_event* ev,
				     ef_event** evs, int* evs_len)
{
	EF_VI_ASSERT(evq->vi_flags & EF_VI_TX_TIMESTAMPS);
	/* When TX timestamping is enabled, we get three events for
	 * every transmit.  A TX completion and two timestamp events.
	 * We ignore the completion and store the first timestamp in
	 * the per TXQ state.  On the second timestamp we retrieve the
	 * first one and construct a EF_EVENT_TYPE_TX_WITH_TIMESTAMP
	 * event to send to the user. */
	if(QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev) == 
	   TX_TIMESTAMP_EVENT_TX_EV_COMPLETION) {
		/* TX completion event.  Ignored */
	}
	else if(QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev) ==
		TX_TIMESTAMP_EVENT_TX_EV_TSTAMP_LO) {
		ef_vi_txq_state* qs = &evq->ep_state->txq;
		EF_VI_BUG_ON(qs->ts_nsec != EF_VI_TX_TIMESTAMP_TS_NSEC_INVALID);
		qs->ts_nsec =
			(((((uint64_t)timestamp_extract(*ev)) *
			   1000000000UL) >> 29) << 2) |
			evq->ep_state->evq.sync_flags;
	}
	else if(QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev) ==
		TX_TIMESTAMP_EVENT_TX_EV_TSTAMP_HI) {
		ef_vi_txq_state* qs = &evq->ep_state->txq;
		ef_vi_txq* q = &evq->vi_txq;
		ef_event* ev_out = (*evs)++;
		--(*evs_len);
		ev_out->tx_timestamp.q_id = QWORD_GET_U(ESF_DZ_TX_QLABEL, *ev);
		ev_out->tx.type = EF_EVENT_TYPE_TX_WITH_TIMESTAMP;
		EF_VI_BUG_ON(qs->ts_nsec == EF_VI_TX_TIMESTAMP_TS_NSEC_INVALID);
		ev_out->tx_timestamp.ts_nsec = qs->ts_nsec;
		EF_VI_DEBUG(qs->ts_nsec = EF_VI_TX_TIMESTAMP_TS_NSEC_INVALID);
		ev_out->tx_timestamp.ts_sec = timestamp_extract(*ev);
		/* One TX pkt could have spanned multiple
		 * descriptors. Iterate to find the one that actually
		 * is finished. */
		while( q->ids[qs->removed & q->mask] == EF_REQUEST_ID_MASK )
			++qs->removed;
		ev_out->tx_timestamp.rq_id = q->ids[qs->removed & q->mask];
		q->ids[qs->removed & q->mask] = EF_REQUEST_ID_MASK;
		++qs->removed;
	}
	else {
		ef_log("%s:%d: ERROR: soft1=%x ev="CI_QWORD_FMT, __FUNCTION__,
		       __LINE__, (unsigned) QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev),
		       CI_QWORD_VAL(*ev));
	}
}


ef_vi_inline void ef10_tx_event(ef_vi* evq, const ef_vi_event* ev,
				ef_event** evs, int* evs_len)
{
	EF_VI_ASSERT(EF_VI_IS_EVENT(ev));
	if( ! (evq->vi_flags & EF_VI_TX_TIMESTAMPS) ) {
		if(likely(QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev) ==
			  TX_TIMESTAMP_EVENT_TX_EV_COMPLETION)) {
			/* Transmit completion event. */
			ef_event* ev_out = (*evs)++;
			--(*evs_len);
			ev_out->tx.q_id = QWORD_GET_U(ESF_DZ_TX_QLABEL, *ev);
			ev_out->tx.desc_id = QWORD_GET_U(ESF_DZ_TX_DESCR_INDX,
							 *ev) + 1;
			ev_out->tx.type = EF_EVENT_TYPE_TX;
		}
		else {
			ef_log("%s:%d: ERROR: soft1=%x ev="CI_QWORD_FMT,
			       __FUNCTION__, __LINE__, (unsigned)
			       QWORD_GET_U(ESF_DZ_TX_SOFT1, *ev),
			       CI_QWORD_VAL(*ev));
		}
	}
	else {
		ef10_tx_event_ts_enabled(evq, ev, evs, evs_len);
	}
}


int
ef_vi_receive_get_timestamp_with_sync_flags(ef_vi* vi, const void* pkt,
					    struct timespec* ts_out,
				            unsigned* flags_out)
{
#define FLAG_NO_TIMESTAMP        0x80000000
#define ONE_SEC                  0x8000000
#define MAX_RX_PKT_DELAY         0xCCCCCC  /* ONE_SECOND / 10 */
#define MAX_TIME_SYNC_DELAY      0x1999999 /* ONE_SECOND * 2 / 10 */
#define SYNC_EVENTS_PER_SECOND   4

	/* sync_timestamp_major contains the number of seconds and
	 * sync_timestamp_minor contains the upper bits of ns.
	 *
	 * The API dictates that this function be called before
	 * eventq_poll() is called again.  We do not allow
	 * eventq_poll() to process mcdi events (time sync events) if
	 * it has already processed any normal events.  Hence, we are
	 * guaranteed that the RX events should be happening in the
	 * range [MAX_RX_PKT_DELAY before time sync event, ONE_SECOND
	 * / SYNC_EVENTS_PER_SECOND + MAX_TIME_SYNC_DELAY after time
	 * sync event].
	 */

	/* Note that pkt_minor is not ns since last sync event but
	 * simply the current ns.
	 */

	/* Note that it is possible for us to incorrectly associate a
	 * pkt_minor with an invalid sync event and there is no way to
	 * detect it.
	 */

	ef_eventq_state* evqs = &(vi->ep_state->evq);
	uint32_t* data = (uint32_t*) ((uint8_t*)pkt +
                                      ES_DZ_RX_PREFIX_TSTAMP_OFST);
	/* pkt_minor contains 27 bits of ns */
	uint32_t pkt_minor_raw = CI_BSWAPC_LE32(*data);
	uint32_t pkt_minor =
		( pkt_minor_raw + vi->rx_ts_correction) & 0x7FFFFFF;
	uint32_t diff;

	EF_VI_ASSERT(vi->vi_flags & EF_VI_RX_TIMESTAMPS);

	if( evqs->sync_timestamp_synchronised &&
	    (pkt_minor_raw & FLAG_NO_TIMESTAMP) == 0 ) {
		ts_out->tv_nsec = ((uint64_t) pkt_minor * 1000000000) >> 27;
		diff = (pkt_minor - evqs->sync_timestamp_minor) & (ONE_SEC - 1);
		if (diff < (ONE_SEC / SYNC_EVENTS_PER_SECOND) +
			MAX_TIME_SYNC_DELAY) {
			/* pkt_minor taken after sync event in the
			 * valid range.  Adjust seconds if sync event
			 * happened, then the second boundary, and
			 * then the pkt_minor.
			 */
			ts_out->tv_sec = evqs->sync_timestamp_major;
			ts_out->tv_sec +=
				diff + evqs->sync_timestamp_minor >= ONE_SEC;
			*flags_out = evqs->sync_flags;
			return 0;
		} else if (diff > ONE_SEC - MAX_RX_PKT_DELAY) {
			/* pkt_minor taken before sync event in the
			 * valid range.  Adjust seconds if pkt_minor
			 * happened, then the second boundary, and
			 * then the sync event.
			 */
			ts_out->tv_sec = evqs->sync_timestamp_major;
			ts_out->tv_sec -=
				diff + evqs->sync_timestamp_minor <= ONE_SEC;
			*flags_out = evqs->sync_flags;
			return 0;
		} else {
			/* diff between pkt_minor and sync event in
			 * invalid range.  Either function used
			 * incorrectly or we lost some sync events.
			 */
			evqs->sync_timestamp_synchronised = 0;
		}
	}
        if( (pkt_minor_raw & FLAG_NO_TIMESTAMP) != 0 )
          return -ENODATA;
	return (evqs->sync_timestamp_major == ~0u) ? -ENOMSG : -EL2NSYNC;
}


extern int
ef_vi_receive_get_timestamp(ef_vi* vi, const void* pkt,
			    struct timespec* ts_out)
{
	unsigned flags_out;
	int rc = ef_vi_receive_get_timestamp_with_sync_flags
		(vi, pkt, ts_out, &flags_out);
	return rc < 0 ? -1 : 0;
}


static void ef10_major_tick(ef_vi* vi, unsigned major, unsigned minor,
			    unsigned sync_flags)
{
	ef_eventq_state* evqs = &(vi->ep_state->evq);
	evqs->sync_timestamp_major = major;
	evqs->sync_timestamp_minor = minor;
	evqs->sync_timestamp_synchronised = 1;
	evqs->sync_flags = sync_flags;
}


static void ef10_mcdi_event(ef_vi* evq, const ef_vi_event* ev)
{
	int code = QWORD_GET_U(MCDI_EVENT_CODE, *ev);
	uint32_t major, minor;
	/* Sync status reporting not supported, let's assume clock is
	 * always in sync */
	uint32_t sync_flags = EF_VI_SYNC_FLAG_CLOCK_SET |
			      EF_VI_SYNC_FLAG_CLOCK_IN_SYNC;

	switch( code ) {
	case MCDI_EVENT_CODE_PTP_TIME:
		major = QWORD_GET_U(MCDI_EVENT_PTP_TIME_MAJOR, *ev);
		minor = QWORD_GET_U(MCDI_EVENT_PTP_TIME_MINOR_26_21, *ev) << 21;
		if( evq->vi_out_flags & EF_VI_OUT_CLOCK_SYNC_STATUS )
		    sync_flags =
			(QWORD_GET_U(MCDI_EVENT_PTP_TIME_NIC_CLOCK_VALID, *ev) ?
				EF_VI_SYNC_FLAG_CLOCK_SET: 0) |
			(QWORD_GET_U(MCDI_EVENT_PTP_TIME_HOST_NIC_IN_SYNC, *ev) ?
				EF_VI_SYNC_FLAG_CLOCK_IN_SYNC: 0);
		ef10_major_tick(evq, major, minor, sync_flags);
		break;
	default:
		ef_log("%s: ERROR: Unhandled mcdi event code=%u", __FUNCTION__,
		       code);
		break;
	}
}


int ef10_ef_eventq_poll(ef_vi* evq, ef_event* evs, int evs_len)
{
	int evs_len_orig = evs_len;
	ef_vi_event *pev, ev;

	EF_VI_BUG_ON(evs == NULL);
	EF_VI_BUG_ON(evs_len < EF_VI_EVENT_POLL_MIN_EVS);

#ifdef __powerpc__
	if(unlikely( EF_VI_IS_EVENT(EF_VI_EVENT_PTR(evq, -17)) ))
		goto overflow;
#else
	if(unlikely( EF_VI_IS_EVENT(EF_VI_EVENT_PTR(evq, -1)) ))
		goto overflow;
#endif

not_empty:
	/* Read the event out of the ring, then fiddle with copied version.
	 * Reason is that the ring is likely to get pushed out of cache by
	 * another event being delivered by hardware.
	 */
	pev = EF_VI_EVENT_PTR(evq, 0);
	ev = *pev;
	if (!EF_VI_IS_EVENT(&ev))
		goto empty;
	do {
		/* Ugly: Exploit the fact that event code lies in top bits
		 * of event. */
		BUG_ON(ESF_DZ_EV_CODE_LBN < 32u);
		switch( CI_QWORD_FIELD(ev, ESF_DZ_EV_CODE) ) {
		case ESE_DZ_EV_CODE_RX_EV:
			ef10_rx_event(evq, &ev, &evs, &evs_len);
			break;

		case ESE_DZ_EV_CODE_TX_EV:
			ef10_tx_event(evq, &ev, &evs, &evs_len);
			break;

		case ESE_DZ_EV_CODE_MCDI_EV:
			/* Do not process MCDI events if we have
			 * already delivered other events to the
			 * app */
			if (evs_len != evs_len_orig)
				goto out;
			ef10_mcdi_event(evq, &ev);
			break;

		case ESE_DZ_EV_CODE_DRIVER_EV:
			if (QWORD_GET_U(ESF_DZ_DRV_SUB_CODE, ev) ==
			    ESE_DZ_DRV_START_UP_EV)
				/* Ignore. */
				break;
			/* ...deliberate fall-through... */
		default:
			ef_log("%s: ERROR: event type=%u ev="CI_QWORD_FMT,
			       __FUNCTION__,
			       (unsigned) CI_QWORD_FIELD(ev, ESF_DZ_EV_CODE),
			       CI_QWORD_VAL(ev));
			break;
		}

		/* Consume event.  Must do after event checking above,
   		 * in case we don't want to consume it. */
#ifdef __powerpc__
		CI_SET_QWORD(*EF_VI_EVENT_PTR(evq, -16));
#else
		CI_SET_QWORD(*pev);
#endif
		evq->ep_state->evq.evq_ptr += sizeof(ef_vi_event);

		if (evs_len < EF_VI_EVENT_POLL_MIN_EVS)
			break;

		pev = EF_VI_EVENT_PTR(evq, 0);
		ev = *pev;
	} while (EF_VI_IS_EVENT(&ev));

out:
	return evs_len_orig - evs_len;

empty:
	if (EF_VI_IS_EVENT(EF_VI_EVENT_PTR(evq, 1))) {
		smp_rmb();
		if (!EF_VI_IS_EVENT(EF_VI_EVENT_PTR(evq, 0))) {
			ef_log("%s: misplaced event (empty) in %u",
                               __FUNCTION__, evq->vi_i);
			/* No event in current slot, but there is one in
			 * the next slot.  Has NIC failed to write event
			 * somehow?
			 */
			evq->ep_state->evq.evq_ptr += sizeof(ef_vi_event);
			INC_ERROR_STAT(evq, evq_gap);
			goto not_empty;
		}
	}

	return 0;

overflow:
	ef_log("%s: ERROR: overflow in %d at %u", __FUNCTION__, evq->vi_i,
	       (unsigned) EF_VI_EVENT_OFFSET(evq, 0));
	evs->generic.type = EF_EVENT_TYPE_OFLOW;
	return 1;
}


void ef10_ef_eventq_prime(ef_vi* vi)
{
	unsigned ring_i = (ef_eventq_current(vi) & vi->evq_mask) / 8;
	EF_VI_ASSERT(vi->inited & EF_VI_INITED_IO);
	writel(ring_i << ERF_DZ_EVQ_RPTR_LBN, vi->io + ER_DZ_EVQ_RPTR_REG);
	mmiowb();
}


/*! \cidoxg_end */
