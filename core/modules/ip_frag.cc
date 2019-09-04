/* for ip_frag decls */
#include "ip_frag.h"
/* for rte_zmalloc() */
#include <rte_malloc.h>
/* for RTE_ETHER macros */
#include "rte_ether.h"
/* for be32_t */
#include "utils/endian.h"
/* for ToIpv4Address() */
#include "utils/ip.h"
/* for eth header */
#include "utils/ether.h"
/*----------------------------------------------------------------------------------*/
using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::be32_t;
using bess::utils::be16_t;
using bess::utils::ToIpv4Address;

enum {DEFAULT_GATE = 0, FORWARD_GATE};
/*----------------------------------------------------------------------------------*/
/**
 * Returns NULL if packet is fragmented and needs more for reassembly.
 * Returns Packet ptr if the packet is unfragmented, or is freshly reassembled.
 */
bess::Packet *
IPFrag::FragmentPkt(Context *ctx, bess::Packet *p)
{
	struct rte_ether_hdr *ethh = (struct rte_ether_hdr *)(p->head_data<Ethernet *>());
	if (ethh->ether_type != (Ethernet::kIpv4))	return p;
	struct rte_ipv4_hdr *iph = (struct rte_ipv4_hdr *)((unsigned char *)ethh + sizeof(struct rte_ether_hdr));
	struct rte_mbuf *m = (struct rte_mbuf *)p;

	if (RTE_ETH_IS_IPV4_HDR(ethh->ether_type) &&
	    unlikely((RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN) < p->total_len())) {
#if 0
		EmitPacket(ctx, p, DEFAULT_GATE);
		return NULL;
#else
		volatile int32_t res;
		struct rte_ether_hdr ethh_copy;
		int32_t j;
		struct rte_mbuf *frag_tbl[64];
		unsigned char *orig_ip_payload;
		uint16_t orig_data_offset;

		/* retrieve Ethernet header */
		rte_memcpy(&ethh_copy, ethh, sizeof(struct rte_ether_hdr));

		/* remove the Ethernet header and trailer from the input packet */
		rte_pktmbuf_adj(m, (uint16_t)sizeof(struct rte_ether_hdr));

		/* retrieve orig ip payload for later re-use in ip frags */
		orig_ip_payload =
			rte_pktmbuf_mtod_offset(m, unsigned char *, sizeof(struct rte_ipv4_hdr));
		orig_data_offset = 0;

		/* fragment the IPV4 packet */
		res = rte_ipv4_fragment_packet(m,
					       &frag_tbl[0],
					       64,
					       RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN - RTE_ETHER_HDR_LEN,
					       m->pool,
					       indirect_pktmbuf_pool);

		if (unlikely(res < 0)) {
			EmitPacket(ctx, p, DEFAULT_GATE);
			return NULL;
		} else {
			/* now copy the Ethernet header + IP payload to each frag */
			for (j = 0; j < res; j++) {
				m = frag_tbl[j];
				ethh = (struct rte_ether_hdr *)
					rte_pktmbuf_prepend(m, (uint16_t)sizeof(struct rte_ether_hdr));
				if (ethh == NULL)
					rte_panic("No headroom in mbuf.\n");
				/* remove chained mbufs (as they are not needed) */
				struct rte_mbuf *del_mbuf = m->next;
				while (del_mbuf != NULL) {
					rte_pktmbuf_free_seg(del_mbuf);
					del_mbuf = del_mbuf->next;
				}

				/* setting mbuf metadata */
				m->l2_len = sizeof(struct rte_ether_hdr);
				m->data_len = m->pkt_len;
				m->nb_segs = 1;
				m->next = NULL;
				rte_memcpy(ethh, &ethh_copy, sizeof(struct rte_ether_hdr));
				
				ethh = (struct rte_ether_hdr *)rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
				iph = (struct rte_ipv4_hdr *)(ethh + 1);

				/* copy ip payload */
				unsigned char *ip_payload = (unsigned char *)((unsigned char *)iph +
									      ((iph->version_ihl & RTE_IPV4_HDR_IHL_MASK) << 2));
				uint16_t ip_payload_len = m->pkt_len - sizeof(struct rte_ether_hdr) -
					((iph->version_ihl & RTE_IPV4_HDR_IHL_MASK) << 2);

				/* if total frame size is less than minimum transmission unit, add IP padding */
				if (unlikely(ip_payload_len + sizeof(struct rte_ipv4_hdr) +
					     sizeof(struct rte_ether_hdr) + RTE_ETHER_CRC_LEN < RTE_ETHER_MIN_LEN)) {
					/* update ip->ihl first */
					iph->version_ihl &= 0xF0;
					iph->version_ihl |= (RTE_IPV4_HDR_IHL_MASK & (PADDED_IPV4_HDR_SIZE>>2));
					/* update ip->tot_len */
					iph->total_length = ntohs(ip_payload_len + PADDED_IPV4_HDR_SIZE);
					/* update l3_len */
					m->l3_len = PADDED_IPV4_HDR_SIZE;
					/* update data_len & pkt_len */
					m->data_len = m->pkt_len = m->pkt_len + IP_PADDING_LEN;
					/* ip_payload is currently the place you would add 0s */
					memset(ip_payload, 0, IP_PADDING_LEN);

					/* re-set ip_payload to the right `offset` (location) now */
					ip_payload += IP_PADDING_LEN;
				}
				rte_memcpy(ip_payload,
					   orig_ip_payload + orig_data_offset,
					   ip_payload_len);
				orig_data_offset += ip_payload_len;
				iph->hdr_checksum = 0;
			}
			for (int i = 0; i < res; i++)
				EmitPacket(ctx, (bess::Packet *)frag_tbl[i], FORWARD_GATE);
			/* all fragments successfully forwarded. Return NULL */
			return NULL;
		}
#endif
	}

	return p;
}
/*----------------------------------------------------------------------------------*/
void
IPFrag::ProcessBatch(Context *ctx, bess::PacketBatch *batch)
{
	int cnt = batch->cnt();
	for (int i = 0; i < cnt; i++) {
		bess::Packet *p = batch->pkts()[i];
		p = FragmentPkt(ctx, p);
		if (p) EmitPacket(ctx, p, FORWARD_GATE);
	}
}
/*----------------------------------------------------------------------------------*/
void
IPFrag::DeInit()
{
	if (indirect_pktmbuf_pool != NULL) {
		/* free allocated IP frags */
		rte_mempool_free(indirect_pktmbuf_pool);
		indirect_pktmbuf_pool = NULL;
	}
}
/*----------------------------------------------------------------------------------*/
CommandResponse
IPFrag::Init(const bess::pb::EmptyArg &) {

	std::string pool_name = this->name() + "_indirect_mbuf_pool";
	indirect_pktmbuf_pool = rte_pktmbuf_pool_create(pool_name.c_str(),
							NUM_MBUFS,
							MBUF_CACHE_SIZE, 0,
							RTE_MBUF_DEFAULT_BUF_SIZE,
							rte_socket_id());

	if (indirect_pktmbuf_pool == NULL)
		return CommandFailure(ENOMEM, "Cannot create indirect mempool!");
	return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(IPFrag, "ip_frag", "IP Fragmentation module")