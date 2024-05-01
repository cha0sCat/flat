#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <linux/bpf.h>
#include <linux/bpf_common.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/pkt_cls.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>


#define TCPOPT_NOP		1	/* Padding */
#define TCPOPT_EOL		0	/* End of options */
#define TCPOPT_MSS		2	/* Segment size negotiating */
#define TCPOLEN_MSS            4

uint16_t tcp_parse_mss_option(const struct tcphdr *th, uint16_t user_mss, void* tail)
{
	const unsigned char *ptr = (const unsigned char *)(th + 1);
	int length = (th->doff * 4) - sizeof(struct tcphdr);
	uint16_t mss = 0;

	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		if (opcode > tail || ptr > tail) {
		    return mss;
        }

		switch (opcode) {
		case TCPOPT_EOL:
			return mss;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			if (length < 2)
				return mss;
			opsize = *ptr++;
			if (opsize < 2) /* "silly options" */
				return mss;
			if (opsize > length)
				return mss;	/* fail on partial options */
			if (opcode == TCPOPT_MSS && opsize == TCPOLEN_MSS) {
//				uint16_t in_mss = get_unaligned_be16(ptr);
//                uint16_t in_mss = bpf_ntohs(*(uint16_t *)ptr);

//				if (in_mss) {
//					if (user_mss && user_mss < in_mss)
//						in_mss = user_mss;
//					mss = in_mss;
//				}
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
	return mss;
}

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 512 * 1024); // 512 KB
} pipe SEC(".maps");

struct packet_t {
    struct in6_addr src_ip;
    struct in6_addr dst_ip;
    __be16 src_port;
    __be16 dst_port;
    __u8 protocol;
    __u8 ttl;
    bool syn;
    bool ack;
    uint64_t ts;
    uint16_t mss; // New field for storing the TCP MSS
};

static inline int handle_ip_packet(void* head, void* tail, uint32_t* offset, struct packet_t* pkt) {
    struct ethhdr* eth = head;
    struct iphdr* ip;
    struct ipv6hdr* ipv6;

    switch (bpf_ntohs(eth->h_proto)) {
    case ETH_P_IP:
        *offset = sizeof(struct ethhdr) + sizeof(struct iphdr);

        if (head + (*offset) > tail) { // If the next layer is not IP, let the packet pass
            return TC_ACT_OK;
        }

        ip = head + sizeof(struct ethhdr);

        if (ip->protocol != IPPROTO_TCP && ip->protocol != IPPROTO_UDP) {
            return TC_ACT_OK;
        }

        // Clean the structure before using it
        pkt->src_ip = (struct in6_addr){0};
        pkt->dst_ip = (struct in6_addr){0};

        // Create IPv4-Mapped IPv6 Address
        pkt->src_ip.in6_u.u6_addr32[3] = ip->saddr;
        pkt->dst_ip.in6_u.u6_addr32[3] = ip->daddr;

        // Pad the field before IP address with all Fs just like the RFC
        pkt->src_ip.in6_u.u6_addr16[5] = 0xffff;
        pkt->dst_ip.in6_u.u6_addr16[5] = 0xffff;

        pkt->protocol = ip->protocol;
        pkt->ttl = ip->ttl;

        return 1; // We have a TCP or UDP packet!

    case ETH_P_IPV6:
        *offset = sizeof(struct ethhdr) + sizeof(struct ipv6hdr);

        if (head + (*offset) > tail) {
            return TC_ACT_OK;
        }

        ipv6 = head + sizeof(struct ethhdr);

        if (ipv6->nexthdr != IPPROTO_TCP && ipv6->nexthdr != IPPROTO_UDP) {
            return TC_ACT_OK;
        }

        pkt->src_ip = ipv6->saddr;
        pkt->dst_ip = ipv6->daddr;

        pkt->protocol = ipv6->nexthdr;
        pkt->ttl = ipv6->hop_limit;

        return 1; // We have a TCP or UDP packet!

    default:
        return TC_ACT_OK;
    }
}

static inline int handle_ip_segment(void* head, void* tail, uint32_t* offset, struct packet_t* pkt) {
    struct tcphdr* tcp;
    struct udphdr* udp;

    switch (pkt->protocol) {
    case IPPROTO_TCP:
        tcp = head + *offset;

        if (tcp->syn) { // We have SYN or SYN/ACK
            pkt->src_port = tcp->source;
            pkt->dst_port = tcp->dest;
            pkt->syn = tcp->syn;
            pkt->ack = tcp->ack;
            pkt->ts = bpf_ktime_get_ns();

            return 1;
        }
    case IPPROTO_UDP:
        udp = head + *offset;

        pkt->src_port = udp->source;
        pkt->dst_port = udp->dest;
        pkt->ts = bpf_ktime_get_ns();

        return 1;

    default:
        return TC_ACT_OK;
    }
}

static inline int try_handle_ip_option(void* head, void* tail, uint32_t* offset, struct packet_t* pkt) {
    // only handle TCP packets
    if (pkt->protocol != IPPROTO_TCP) {
        return 0;
    }

    // only handle SYN / SYN.ACK packets
    if (pkt->syn == 0) {
        return 0;
    }

    struct tcphdr* tcp;
    tcp = head + *offset;
    if (!(tcp->doff > (sizeof(struct tcphdr)>>2))) {
        return 0;
    }

    pkt->mss = tcp_parse_mss_option(tcp, 0, tail);
    return 1;
}

SEC("tc")
int flat(struct __sk_buff* skb) {

    if (bpf_skb_pull_data(skb, 0) < 0) {
        return TC_ACT_OK;
    }

    // We only want unicast packets
    if (skb->pkt_type == PACKET_BROADCAST || skb->pkt_type == PACKET_MULTICAST) {
        return TC_ACT_OK;
    }

    void* head = (void*)(long)skb->data;     // Start of the packet data
    void* tail = (void*)(long)skb->data_end; // End of the packet data

    if (head + sizeof(struct ethhdr) > tail) { // Not an Ethernet frame
        return TC_ACT_OK;
    }

    struct packet_t* pkt;
    pkt = bpf_ringbuf_reserve(&pipe, sizeof(struct packet_t), 0);
    if (!pkt) {
        return TC_ACT_OK;
    }

    uint32_t offset = 0;

    if (handle_ip_packet(head, tail, &offset, pkt) == TC_ACT_OK) {
        bpf_ringbuf_discard(pkt, 0);
        return TC_ACT_OK;
    }

    // Check if TCP/UDP header is fitting this packet
    if (head + offset + sizeof(struct tcphdr) > tail || head + offset + sizeof(struct udphdr) > tail) {
        bpf_ringbuf_discard(pkt, 0);
        return TC_ACT_OK;
    }

    if (handle_ip_segment(head, tail, &offset, pkt) == TC_ACT_OK) {
        bpf_ringbuf_discard(pkt, 0);
        return TC_ACT_OK;
    }

    try_handle_ip_option(head, tail, &offset, pkt);

    bpf_ringbuf_submit(pkt, 0);

    return TC_ACT_OK;
}

char _license[] SEC("license") = "Dual MIT/GPL";
