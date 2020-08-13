/* Inject packets with PACKET_TX_RING and PACKET_VNET_HDR */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <linux/virtio_net.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool cfg_enable_ring = true;
static bool cfg_enable_vnet = false;
static char *cfg_ifname = "eth0";
static int cfg_ifindex;
static int cfg_num_frames = 3;
static int cfg_payload_len = 500;

static struct tpacket_req req;
static struct in_addr ip_saddr, ip_daddr;

/* must configure real daddr (should really infer or pass on cmdline) */
const char cfg_mac_src[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
const char cfg_mac_dst[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

static int socket_open(void)
{
	int fd, val;

	fd = socket(PF_PACKET, SOCK_RAW, 0 /* disable reading */);
	if (fd == -1)
		error(1, errno, "socket");

	val = TPACKET_V2;
	if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &val, sizeof(val)))
		error(1, errno, "setsockopt version");

	if (cfg_enable_vnet) {
		val = 1;
		if (setsockopt(fd, SOL_PACKET, PACKET_VNET_HDR,
			       &val, sizeof(val)))
			error(1, errno, "setsockopt vnet_hdr");
	}

	return fd;
}

static char * ring_open(int fd)
{
	char *ring;
	
	req.tp_frame_size = getpagesize() << 1;
	req.tp_frame_nr   = cfg_num_frames;
	req.tp_block_size = req.tp_frame_size;
	req.tp_block_nr   = cfg_num_frames;

	if (setsockopt(fd, SOL_PACKET, PACKET_TX_RING,
		       (void*) &req, sizeof(req)))
		error(1, errno, "setsockopt ring");

	ring = mmap(0, req.tp_block_size * req.tp_block_nr,
		    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED)
		error(1, errno, "mmap");

	return ring;
}

/* warning: does not handle odd length */
static unsigned long add_csum_hword(const uint16_t *start, int num_u16)
{
	unsigned long sum = 0;
	int i;

	for (i = 0; i < num_u16; i++)
		sum += start[i];

	return sum;
}

static uint16_t build_ip_csum(const uint16_t *start, int num_u16,
			      unsigned long sum)
{
	sum += add_csum_hword(start, num_u16);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return ~sum;
}

static uint16_t get_tcp_v4_csum(const struct iphdr *iph,
				const struct tcphdr *tcph,
				int length)
{
	unsigned long pseudo_sum = 0;

	pseudo_sum += add_csum_hword((void *) &iph->saddr, 2);
	pseudo_sum += add_csum_hword((void *) &iph->daddr, 2);
	pseudo_sum += htons(IPPROTO_TCP);
	pseudo_sum += htons(length);

	if (cfg_enable_vnet)
		return pseudo_sum;
	else
		return build_ip_csum((void *) tcph, length >> 1, pseudo_sum);
}

static int frame_fill(void *buffer, unsigned int payload_len)
{
	struct ethhdr *eth;
	struct iphdr *iph;
	struct tcphdr *tcph;
	int off = 0;

	if (cfg_enable_vnet) {
		struct virtio_net_hdr *vnet;

		vnet = buffer;

		vnet->hdr_len = ETH_HLEN + sizeof(*iph) + sizeof(*tcph);

		vnet->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
		vnet->csum_start = ETH_HLEN + sizeof(*iph);
		vnet->csum_offset = __builtin_offsetof(struct tcphdr, check);

		vnet->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		vnet->gso_size = ETH_DATA_LEN - sizeof(struct iphdr) -
						sizeof(struct tcphdr);
		off += sizeof(*vnet); 
	}

	eth = buffer + off;
	memcpy(&eth->h_source, cfg_mac_src, ETH_ALEN);
	memcpy(&eth->h_dest, cfg_mac_dst, ETH_ALEN);
	eth->h_proto = htons(ETH_P_IP);
	off += ETH_HLEN;

	iph = buffer + off;
	iph->ttl	= 8;
	iph->ihl	= 5;
	iph->version	= 4;
	iph->saddr	= ip_saddr.s_addr;
	iph->daddr	= ip_daddr.s_addr;
	iph->protocol	= IPPROTO_TCP;
	iph->tot_len	= htons(sizeof(*iph) + sizeof(*tcph) + payload_len);
	iph->check	= build_ip_csum((const void *) iph, 10 /* hwords */, 0); 
	off += sizeof(*iph);

	tcph = buffer + off;
	tcph->dest	= htons(9);
	tcph->source	= htons(9);
	tcph->doff	= sizeof(*tcph) >> 2;
	off += sizeof(*tcph);

	memset(buffer + off, 'a', payload_len);

	tcph->check	= get_tcp_v4_csum(iph, tcph,
					  (sizeof(*tcph) + payload_len));
	return off + payload_len;
}

static void ring_write(void *slot)
{
	struct tpacket2_hdr *header = slot;

	if (header->tp_status != TP_STATUS_AVAILABLE)
		error(1, 0, "write: slot not available");

	memset(slot + TPACKET2_HDRLEN, 0, req.tp_frame_size - TPACKET2_HDRLEN);

	header->tp_mac = TPACKET2_HDRLEN - sizeof(struct sockaddr_ll);
	header->tp_len = frame_fill(slot + header->tp_mac, cfg_payload_len);
	header->tp_status = TP_STATUS_SEND_REQUEST;
}

static void socket_write(int fd)
{
	static char buf[ETH_HLEN + (1 << 16)];
	int len, ret;

	memset(buf, 0, sizeof(buf));
	len = frame_fill(buf, cfg_payload_len);
	ret = send(fd, buf, len, 0);
	if (ret == -1)
		error(1, errno, "send");
	if (ret < len)
		error(1, 0, "send: %uB < %uB\n", ret, len);
}

static void socket_bind(int fd)
{
	struct sockaddr_ll addr = { 0 };

	addr.sll_family =	AF_PACKET;
	addr.sll_ifindex =	cfg_ifindex;
	addr.sll_protocol =	htons(ETH_P_IP);
	addr.sll_halen =	ETH_ALEN;

	if (bind(fd, (void *) &addr, sizeof(addr)))
		error(1, errno, "bind");
}

static void ring_wake_kernel(int fd)
{
	int ret;

	ret = send(fd, NULL, 0, 0);
	if (ret < 0)
		error(1, errno, "send");
	if (!ret)
		error(1, 0, "send: no data");

	fprintf(stderr, "send: %uB\n", ret);
}

static void ring_close(char *ring)
{
	if (munmap(ring, req.tp_block_size * req.tp_block_nr))
		error(1, errno, "munmap");
}

static void do_run_ring(int fd, char *ring)
{
	int i;

	for (i = 0; i < cfg_num_frames; i++)
		ring_write(ring + (i * req.tp_frame_size));

	ring_wake_kernel(fd);
}

static void do_run(int fd)
{
	int i;

	for (i = 0; i < cfg_num_frames; i++)
		socket_write(fd);
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "d:vi:l:ns:")) != -1)
	{
		switch (c) {
		case 'd':
			if (!inet_aton(optarg, &ip_daddr))
				error(1, 0, "bad ipv4 destination address");
			break;
		case 'i':
			cfg_ifname = optarg;
			break;
		case 'l':
			cfg_payload_len = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			cfg_enable_ring = false;
			break;
		case 's':
			if (!inet_aton(optarg, &ip_saddr))
				error(1, 0, "bad ipv4 destination address");
			break;
		case 'v':
			cfg_enable_vnet = true;
			break;
		default:
			error(1, 0, "unknown option %c", c);
		}
	}

	if (!ip_saddr.s_addr || !ip_daddr.s_addr)
		error(1, 0, "must specify ipv4 source and destination");

	cfg_ifindex = if_nametoindex(cfg_ifname);
	if (!cfg_ifindex)
		error(1, errno, "ifnametoindex");
}

int main(int argc, char **argv)
{
	char *ring;
	int fd;

	parse_opts(argc, argv);

	fprintf(stderr, "vnet: %sabled\n", cfg_enable_vnet ? "en" : "dis");

	fd = socket_open();
	socket_bind(fd);

	if (cfg_enable_ring) {
		ring = ring_open(fd);
		do_run_ring(fd, ring);
		ring_close(ring);
	} else {
		do_run(fd);
	}

	if (close(fd) == -1)
		error(1, errno, "close");

	return 0;
}
