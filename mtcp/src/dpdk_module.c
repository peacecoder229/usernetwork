/* for io_module_func def'ns */
#include "io_module.h"
#ifndef DISABLE_DPDK
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"
/* for rte_max_eth_ports */
#include <rte_common.h>
/* for rte_eth_rxconf */
#include <rte_ethdev.h>
/* for delay funcs */
#include <rte_cycles.h>
#include <rte_errno.h>
//#define ENABLE_STATS_IOCTL		1
#ifdef ENABLE_STATS_IOCTL
/* for close */
#include <unistd.h>
/* for open */
#include <fcntl.h>
/* for ioctl */
#include <sys/ioctl.h>
#endif /* !ENABLE_STATS_IOCTL */
/* for ip pseudo-chksum */
#include <rte_ip.h>
//#define IP_DEFRAG			1
#ifdef IP_DEFRAG
/* for ip defragging */
#include <rte_ip_frag.h>
#endif
#include <unistd.h>
/* for ioctl funcs */
#include <dpdk_iface_common.h>
/* for Ethernet flow directors */
#include <rte_flow.h>
/* for GetNumCPUs() */
#include "cpu.h"
/* for retrieving rte version(s) */
#include <rte_version.h>
/*----------------------------------------------------------------------------*/
/* Essential macros */
#define MAX_RX_QUEUE_PER_LCORE		MAX_CPUS
#define MAX_TX_QUEUE_PER_PORT		MAX_CPUS

#ifdef ENABLELRO
#define BUF_SIZE			16384
#else
#define BUF_SIZE			2048
#endif /* !ENABLELRO */
#define MBUF_SIZE 			(BUF_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF				8192
#define MEMPOOL_CACHE_SIZE		256
#ifdef ENFORCE_RX_IDLE
#define RX_IDLE_ENABLE			1
#define RX_IDLE_TIMEOUT			1	/* in micro-seconds */
#endif

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 			8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 			8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 			4 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 			36 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH			0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH			0  /**< Default values of TX write-back threshold reg. */

#define MAX_PKT_BURST			128/*128*/

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	2048
#define RTE_TEST_TX_DESC_DEFAULT	2048

/*
 * Ethernet frame overhead
 */

#define ETHER_IFG			12
#define	ETHER_PREAMBLE			8
#define ETHER_OVR			(ETHER_CRC_LEN + ETHER_PREAMBLE + ETHER_IFG)

/*
 * Ethernet flow director filter
 */
#define ETHER_FILTER_THRESH		MAX_CPUS

static const uint16_t nb_rxd = 		RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = 		RTE_TEST_TX_DESC_DEFAULT;
/*----------------------------------------------------------------------------*/
/* packet memory pools for storing packet bufs */
static struct rte_mempool *pktmbuf_pool[MAX_CPUS] = {NULL};

//#define DEBUG				1
#ifdef DEBUG
/* ethernet addresses of ports */
static struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];
#endif

static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.max_rx_pkt_len = 	ETHER_MAX_LEN,
#if RTE_VERSION > RTE_VERSION_NUM(17, 8, 0, 0)
		.offloads	=	(
#if RTE_VERSION < RTE_VERSION_NUM(18, 5, 0, 0)
					 (DEV_RX_OFFLOAD_CRC_STRIP |
#endif /* !18.05 */
					 DEV_RX_OFFLOAD_CHECKSUM
#ifdef ENABLELRO
					 DEV_RX_OFFLOAD_TCP_LRO
#endif
					 ),
		.split_hdr_size = 	0,
#if RTE_VERSION < RTE_VERSION_NUM(18, 5, 0, 0)
		.header_split   = 	0, /**< Header Split disabled */
		.hw_ip_checksum = 	1, /**< IP checksum offload enabled */
		.hw_vlan_filter = 	0, /**< VLAN filtering disabled */
		.jumbo_frame    = 	0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 	1, /**< CRC stripped by hardware */
#endif /* !18.05 */
#ifdef ENABLELRO
		.enable_lro	=	1, /**< Enable LRO */
#endif
#endif
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = 	NULL,
			.rss_hf = 	ETH_RSS_TCP | ETH_RSS_UDP |
					ETH_RSS_IP | ETH_RSS_L2_PAYLOAD
		},
	},
	.txmode = {
		.mq_mode = 		ETH_MQ_TX_NONE,
#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
		.offloads	=	(DEV_TX_OFFLOAD_IPV4_CKSUM |
					 DEV_TX_OFFLOAD_UDP_CKSUM |
					 DEV_TX_OFFLOAD_TCP_CKSUM)
#endif /* !18.05 */
	},
	.fdir_conf = {
		.mode = RTE_FDIR_MODE_PERFECT,
		.pballoc = RTE_FDIR_PBALLOC_64K,
		.status = RTE_FDIR_REPORT_STATUS,
		.drop_queue = 127,
	},
};

static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = 		RX_PTHRESH, /* RX prefetch threshold reg */
		.hthresh = 		RX_HTHRESH, /* RX host threshold reg */
		.wthresh = 		RX_WTHRESH, /* RX write-back threshold reg */
	},
	.rx_free_thresh = 		32,
};

static const struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = 		TX_PTHRESH, /* TX prefetch threshold reg */
		.hthresh = 		TX_HTHRESH, /* TX host threshold reg */
		.wthresh = 		TX_WTHRESH, /* TX write-back threshold reg */
	},
	.tx_free_thresh = 		0, /* Use PMD default values */
	.tx_rs_thresh = 		0, /* Use PMD default values */
#if RTE_VERSION < RTE_VERSION_NUM(18, 5, 0, 0)
	/*
	 * As the example won't handle mult-segments and offload cases,
	 * set the flag by default.
	 */
	.txq_flags = 			0x0,
#endif
};

struct mbuf_table {
	uint16_t len; /* length of queued packets */
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct dpdk_private_context {
	struct mbuf_table rmbufs[RTE_MAX_ETHPORTS];
	struct mbuf_table wmbufs[RTE_MAX_ETHPORTS];
	struct rte_mempool *pktmbuf_pool;
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
#ifdef RX_IDLE_ENABLE
	uint8_t rx_idle;
#endif
#ifdef IP_DEFRAG
	struct rte_ip_frag_tbl *frag_tbl;
	struct rte_ip_frag_death_row death_row;
#endif
#ifdef ENABLELRO
	struct rte_mbuf *cur_rx_m;
#endif
#ifdef ENABLE_STATS_IOCTL
	int fd;
	uint32_t cur_ts;
#endif /* !ENABLE_STATS_IOCTL */
} __rte_cache_aligned;

#ifdef ENABLE_STATS_IOCTL
/**
 * stats struct passed on from user space to the driver
 */
struct stats_struct {
	uint64_t tx_bytes;
	uint64_t tx_pkts;
	uint64_t rx_bytes;
	uint64_t rx_pkts;
	uint64_t rmiss;
	uint64_t rerr;
	uint64_t terr;
	uint8_t qid;
	uint8_t dev;
};
#endif /* !ENABLE_STATS_IOCTL */

#ifdef IP_DEFRAG
/* Should be power of two. */
#define IP_FRAG_TBL_BUCKET_ENTRIES		16
#define RTE_LOGTYPE_IP_RSMBL 			RTE_LOGTYPE_USER1
#define MAX_FRAG_NUM				RTE_LIBRTE_IP_FRAG_MAX_FRAG
#endif /* !IP_DEFRAG */
/*----------------------------------------------------------------------------*/
void
dpdk_init_handle(struct mtcp_thread_context *ctxt)
{
	struct dpdk_private_context *dpc;
	int i, j;
	char mempool_name[RTE_MEMPOOL_NAMESIZE];

	/* create and initialize private I/O module context */
	ctxt->io_private_context = calloc(1, sizeof(struct dpdk_private_context));
	if (ctxt->io_private_context == NULL) {
		TRACE_ERROR("Failed to initialize ctxt->io_private_context: "
			    "Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}

	sprintf(mempool_name, "mbuf_pool-%d", ctxt->cpu);
	dpc = (struct dpdk_private_context *)ctxt->io_private_context;
	dpc->pktmbuf_pool = pktmbuf_pool[ctxt->cpu];

	/* set wmbufs correctly */
	for (j = 0; j < num_devices_attached; j++) {
		/* Allocate wmbufs for each registered port */
		for (i = 0; i < MAX_PKT_BURST; i++) {
			dpc->wmbufs[j].m_table[i] = rte_pktmbuf_alloc(pktmbuf_pool[ctxt->cpu]);
			if (dpc->wmbufs[j].m_table[i] == NULL) {
				TRACE_ERROR("Failed to allocate %d:wmbuf[%d] on device %d!\n",
					    ctxt->cpu, i, j);
				exit(EXIT_FAILURE);
			}
		}
		/* set mbufs queue length to 0 to begin with */
		dpc->wmbufs[j].len = 0;
	}

#ifdef IP_DEFRAG
	int max_flows;
	int socket;
	uint64_t frag_cycles;
	int num_cpus;

	num_cpus = (CONFIG.num_cores == 0) ? GetNumCPUs() : CONFIG.num_cores;
	max_flows = CONFIG.max_concurrency / num_cpus;
	frag_cycles = (rte_get_tsc_hz() + MS_PER_S - 1)
		/ MS_PER_S * max_flows;
	socket = rte_lcore_to_socket_id(ctxt->cpu);

	if ((dpc->frag_tbl = rte_ip_frag_table_create(max_flows,
						      IP_FRAG_TBL_BUCKET_ENTRIES,
						      max_flows,
						      frag_cycles,
						      socket)) == NULL) {
		RTE_LOG(ERR, IP_RSMBL, "ip_frag_tbl_create(%u) on "
			"lcore: %u for queue: %u failed\n",
			max_flows, ctxt->cpu, ctxt->cpu);
		exit(EXIT_FAILURE);
	}
#endif	/* !IP_DEFRAG */

#ifdef ENABLE_STATS_IOCTL
	dpc->fd = open(DEV_PATH, O_RDWR);
	if (dpc->fd == -1) {
		TRACE_ERROR("Can't open " DEV_PATH " for context->cpu: %d! "
			    "Are you using mlx4/mlx5 driver?\n",
			    ctxt->cpu);
	}
#endif /* !ENABLE_STATS_IOCTL */
}
/*----------------------------------------------------------------------------*/
int
dpdk_link_devices(struct mtcp_thread_context *ctxt)
{
	/* linking takes place during mtcp_init() */

	return 0;
}
/*----------------------------------------------------------------------------*/
void
dpdk_release_pkt(struct mtcp_thread_context *ctxt, int ifidx, unsigned char *pkt_data, int len)
{
	/*
	 * do nothing over here - memory reclamation
	 * will take place in dpdk_recv_pkts
	 */
}
/*----------------------------------------------------------------------------*/
int
dpdk_send_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct dpdk_private_context *dpc;
#ifdef NETSTAT
	mtcp_manager_t mtcp;
#endif
	int ret, i, portid = CONFIG.eths[ifidx].ifindex;

	dpc = (struct dpdk_private_context *)ctxt->io_private_context;
#ifdef NETSTAT
	mtcp = ctxt->mtcp_manager;
#endif
	ret = 0;

	/* if there are packets in the queue... flush them out to the wire */
	if (dpc->wmbufs[ifidx].len >/*= MAX_PKT_BURST*/ 0) {
		struct rte_mbuf **pkts;
#ifdef ENABLE_STATS_IOCTL
#ifdef NETSTAT
		struct rte_eth_stats stats;
		struct stats_struct ss;
#endif
#endif /* !ENABLE_STATS_IOCTL */
		int cnt = dpc->wmbufs[ifidx].len;
		pkts = dpc->wmbufs[ifidx].m_table;
#ifdef NETSTAT
		mtcp->nstat.tx_packets[ifidx] += cnt;
#ifdef ENABLE_STATS_IOCTL
		/* only pass stats after >= 1 sec interval */
		if (abs(mtcp->cur_ts - dpc->cur_ts) >= 1000 &&
		    likely(dpc->fd >= 0)) {
			/* rte_get_stats is global func, use only for 1 core */
			if (ctxt->cpu == 0) {
				rte_eth_stats_get(portid, &stats);
				ss.rmiss = stats.imissed;
				ss.rerr = stats.ierrors;
				ss.terr = stats.oerrors;
			} else 
				ss.rmiss = ss.rerr = ss.terr = 0;
			
			ss.tx_pkts = mtcp->nstat.tx_packets[ifidx];
			ss.tx_bytes = mtcp->nstat.tx_bytes[ifidx];
			ss.rx_pkts = mtcp->nstat.rx_packets[ifidx];
			ss.rx_bytes = mtcp->nstat.rx_bytes[ifidx];
			ss.qid = ctxt->cpu;
			ss.dev = portid;
			/* pass the info now */
			if (ioctl(dpc->fd, SEND_STATS, &ss) == -1)
				TRACE_ERROR("Can't update iface stats!\n");
			dpc->cur_ts = mtcp->cur_ts;
			if (ctxt->cpu == 0)
				rte_eth_stats_reset(portid);
		}
#endif /* !ENABLE_STATS_IOCTL */
#endif
		do {
			/* tx cnt # of packets */
			ret = rte_eth_tx_burst(portid, ctxt->cpu,
					       pkts, cnt);
			pkts += ret;
			cnt -= ret;
			/* if not all pkts were sent... then repeat the cycle */
		} while (cnt > 0);

		/* time to allocate fresh mbufs for the queue */
		for (i = 0; i < dpc->wmbufs[ifidx].len; i++) {
			dpc->wmbufs[ifidx].m_table[i] = rte_pktmbuf_alloc(pktmbuf_pool[ctxt->cpu]);
			/* error checking */
			if (unlikely(dpc->wmbufs[ifidx].m_table[i] == NULL)) {
				TRACE_ERROR("Failed to allocate %d:wmbuf[%d] on device %d!\n",
					    ctxt->cpu, i, ifidx);
				exit(EXIT_FAILURE);
			}
		}
		/* reset the len of mbufs var after flushing of packets */
		dpc->wmbufs[ifidx].len = 0;
	}

	return ret;
}
/*----------------------------------------------------------------------------*/
uint8_t *
dpdk_get_wptr(struct mtcp_thread_context *ctxt, int ifidx, uint16_t pktsize)
{
	struct dpdk_private_context *dpc;
#ifdef NETSTAT
	mtcp_manager_t mtcp;
#endif
	struct rte_mbuf *m;
	uint8_t *ptr;
	int len_of_mbuf;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
#ifdef NETSTAT
	mtcp = ctxt->mtcp_manager;
#endif

	/* sanity check */
	if (unlikely(dpc->wmbufs[ifidx].len == MAX_PKT_BURST))
		return NULL;

	len_of_mbuf = dpc->wmbufs[ifidx].len;
	m = dpc->wmbufs[ifidx].m_table[len_of_mbuf];

	/* retrieve the right write offset */
	ptr = (void *)rte_pktmbuf_mtod(m, struct ether_hdr *);
	m->pkt_len = m->data_len = pktsize;
	m->nb_segs = 1;
	m->next = NULL;

#ifdef NETSTAT
	mtcp->nstat.tx_bytes[ifidx] += pktsize + ETHER_OVR;
#endif

	/* increment the len_of_mbuf var */
	dpc->wmbufs[ifidx].len = len_of_mbuf + 1;

	return (uint8_t *)ptr;
}
/*----------------------------------------------------------------------------*/
static inline void
free_pkts(struct rte_mbuf **mtable, unsigned len)
{
	int i;

	/* free the freaking packets */
	for (i = 0; i < len; i++) {
		rte_pktmbuf_free(mtable[i]);
		RTE_MBUF_PREFETCH_TO_FREE(mtable[i+1]);
	}
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct dpdk_private_context *dpc;
	int ret;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;

	if (dpc->rmbufs[ifidx].len != 0) {
		free_pkts(dpc->rmbufs[ifidx].m_table, dpc->rmbufs[ifidx].len);
		dpc->rmbufs[ifidx].len = 0;
	}

	int portid = CONFIG.eths[ifidx].ifindex;
	ret = rte_eth_rx_burst((uint8_t)portid, ctxt->cpu,
			       dpc->pkts_burst, MAX_PKT_BURST);
#ifdef RX_IDLE_ENABLE
	dpc->rx_idle = (likely(ret != 0)) ? 0 : dpc->rx_idle + 1;
#endif
	dpc->rmbufs[ifidx].len = ret;

	return ret;
}
/*----------------------------------------------------------------------------*/
#ifdef IP_DEFRAG
struct rte_mbuf *
ip_reassemble(struct dpdk_private_context *dpc, struct rte_mbuf *m)
{
	struct ether_hdr *eth_hdr;
	struct rte_ip_frag_tbl *tbl;
	struct rte_ip_frag_death_row *dr;

	/* if packet is IPv4 */
	if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {
		struct ipv4_hdr *ip_hdr;

		eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
		ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);

		/* if it is a fragmented packet, then try to reassemble. */
		if (rte_ipv4_frag_pkt_is_fragmented(ip_hdr)) {
			struct rte_mbuf *mo;

			tbl = dpc->frag_tbl;
			dr = &dpc->death_row;

			/* prepare mbuf: setup l2_len/l3_len. */
			m->l2_len = sizeof(*eth_hdr);
			m->l3_len = sizeof(*ip_hdr);

			/* process this fragment. */
			mo = rte_ipv4_frag_reassemble_packet(tbl, dr, m, rte_rdtsc(), ip_hdr);
			if (mo == NULL)
				/* no packet to send out. */
				return NULL;

			/* we have our packet reassembled. */
			if (mo != m)
				m = mo;
		}
	}

	/* if packet isn't IPv4, just accept it! */
	return m;
}
#endif
/*----------------------------------------------------------------------------*/
uint8_t *
dpdk_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct dpdk_private_context *dpc;
	struct rte_mbuf *m;
	uint8_t *pktbuf;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;

	m = dpc->pkts_burst[index];
#ifdef IP_DEFRAG
	m = ip_reassemble(dpc, m);
#endif
	*len = m->pkt_len;
	pktbuf = rte_pktmbuf_mtod(m, uint8_t *);

	/* enqueue the pkt ptr in mbuf */
	dpc->rmbufs[ifidx].m_table[index] = m;

	/* verify checksum values from ol_flags */
	if ((m->ol_flags & (PKT_RX_L4_CKSUM_BAD | PKT_RX_IP_CKSUM_BAD)) != 0) {
		TRACE_ERROR("%s(%p, %d, %d): mbuf with invalid checksum: "
			    "%p(%lu);\n",
			    __func__, ctxt, ifidx, index, m, m->ol_flags);
		pktbuf = NULL;
	}
#ifdef ENABLELRO
	dpc->cur_rx_m = m;
#endif /* ENABLELRO */

	return pktbuf;
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_select(struct mtcp_thread_context *ctxt)
{
#ifdef RX_IDLE_ENABLE
	struct dpdk_private_context *dpc;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	if (dpc->rx_idle > RX_IDLE_THRESH) {
		dpc->rx_idle = 0;
		usleep(RX_IDLE_TIMEOUT);
	}
#endif
	return 0;
}
/*----------------------------------------------------------------------------*/
void
dpdk_destroy_handle(struct mtcp_thread_context *ctxt)
{
	struct dpdk_private_context *dpc;
	int i;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;

	/* free wmbufs */
	for (i = 0; i < num_devices_attached; i++)
		free_pkts(dpc->wmbufs[i].m_table, MAX_PKT_BURST);

#ifdef ENABLE_STATS_IOCTL
	/* free fd */
	if (dpc->fd >= 0)
		close(dpc->fd);
#endif /* !ENABLE_STATS_IOCTL */

	/* free it all up */
	free(dpc);
}
/*----------------------------------------------------------------------------*/
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 			100 /* 100ms */
#define MAX_CHECK_TIME 			90 /* 9s (90 * 100ms) in total */

	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}
/*----------------------------------------------------------------------------*/
void
dpdk_load_module(void)
{
	int portid, rxlcore_id, ret, num_cpus;
	/* for Ethernet flow control settings */
	struct rte_eth_fc_conf fc_conf;
	/* setting the rss key */
	static uint8_t key[] = {
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, /* 10 */
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, /* 20 */
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, /* 30 */
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, /* 40 */
		0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, /* 50 */
		0x05, 0x05  /* 60 - 8 */
	};

	port_conf.rx_adv_conf.rss_conf.rss_key = (uint8_t *)key;
	port_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(key);

	/* check if fdir is `on' */
	if (CONFIG.eth_filter == 0) port_conf.rxmode.mq_mode = ETH_MQ_RX_RSS;
	
	/* re-set num_cpus */
	num_cpus = (CONFIG.num_cores == 0) ? GetNumCPUs() : CONFIG.num_cores;
	
	if (!CONFIG.multi_process || (CONFIG.multi_process && CONFIG.multi_process_is_master)) {
		for (rxlcore_id = 0; rxlcore_id < num_cpus; rxlcore_id++) {
			char name[RTE_MEMPOOL_NAMESIZE];
			uint32_t nb_mbuf;
			sprintf(name, "mbuf_pool-%d", rxlcore_id);
			nb_mbuf = NB_MBUF;
#ifdef IP_DEFRAG
			int max_flows;
			max_flows = CONFIG.max_concurrency / num_cpus;

			/*
			 * At any given moment up to <max_flows * (MAX_FRAG_NUM)>
			 * mbufs could be stored int the fragment table.
			 * Plus, each TX queue can hold up to <max_flows> packets.
			 */

			nb_mbuf = RTE_MAX(max_flows, 2UL * MAX_PKT_BURST) * MAX_FRAG_NUM;
			nb_mbuf *= (port_conf.rxmode.max_rx_pkt_len + BUF_SIZE - 1) / BUF_SIZE;
			nb_mbuf += RTE_TEST_RX_DESC_DEFAULT + RTE_TEST_TX_DESC_DEFAULT;

			nb_mbuf = RTE_MAX(nb_mbuf, (uint32_t)NB_MBUF);
#endif
			/* create the mbuf pools */
			pktmbuf_pool[rxlcore_id] =
				rte_mempool_create(name, nb_mbuf,
				MBUF_SIZE, MEMPOOL_CACHE_SIZE,
				sizeof(struct rte_pktmbuf_pool_private),
				rte_pktmbuf_pool_init, NULL,
				rte_pktmbuf_init, NULL,
				rte_socket_id(), MEMPOOL_F_SP_PUT |
				MEMPOOL_F_SC_GET);

			if (pktmbuf_pool[rxlcore_id] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot init mbuf pool, errno: %d\n",
					 rte_errno);
		}

		/* Initialise each port */
		int i;
		for (i = 0; i < num_devices_attached; ++i) {
		        /* get portid form the index of attached devices */
		        portid = devices_attached[i];
#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
			/* re-adjust rss_hf */
			port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info[portid].flow_type_rss_offloads;
#endif

			/* init port */
			printf("Initializing port %u... ", (unsigned) portid);
			fflush(stdout);
			ret = rte_eth_dev_configure(portid, num_cpus, num_cpus, &port_conf);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					 ret, (unsigned) portid);

			/* init one RX queue per CPU */
			fflush(stdout);
#ifdef DEBUG
			rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
#endif
			/* check port capabilities */
			rte_eth_dev_info_get(portid, &dev_info[portid]);

			for (rxlcore_id = 0; rxlcore_id < num_cpus; rxlcore_id++) {
				ret = rte_eth_rx_queue_setup(portid, rxlcore_id, nb_rxd,
							     rte_eth_dev_socket_id(portid), &rx_conf,
							     pktmbuf_pool[rxlcore_id]);
				if (ret < 0)
					rte_exit(EXIT_FAILURE,
						 "rte_eth_rx_queue_setup:err=%d, port=%u, queueid: %d\n",
						 ret, (unsigned) portid, rxlcore_id);
			}

			/* init one TX queue on each port per CPU (this is redundant for this app) */
			fflush(stdout);
			for (rxlcore_id = 0; rxlcore_id < num_cpus; rxlcore_id++) {
				ret = rte_eth_tx_queue_setup(portid, rxlcore_id, nb_txd,
							     rte_eth_dev_socket_id(portid), &tx_conf);
				if (ret < 0)
					rte_exit(EXIT_FAILURE,
						 "rte_eth_tx_queue_setup:err=%d, port=%u, queueid: %d\n",
						 ret, (unsigned) portid, rxlcore_id);
			}

			/* Start device */
			ret = rte_eth_dev_start(portid);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					 ret, (unsigned) portid);

			printf("done: \n");
			rte_eth_promiscuous_enable(portid);

                        /* retrieve current flow control settings per port */
			memset(&fc_conf, 0, sizeof(fc_conf));
                        ret = rte_eth_dev_flow_ctrl_get(portid, &fc_conf);
			if (ret != 0)
                                TRACE_INFO("Failed to get flow control info!\n");

			/* and just disable the rx/tx flow control */
			fc_conf.mode = RTE_FC_NONE;
			ret = rte_eth_dev_flow_ctrl_set(portid, &fc_conf);
                        if (ret != 0)
                                TRACE_INFO("Failed to set flow control info!: errno: %d\n",
                                         ret);

#ifdef DEBUG
			printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
			       (unsigned) portid,
			       ports_eth_addr[portid].addr_bytes[0],
			       ports_eth_addr[portid].addr_bytes[1],
			       ports_eth_addr[portid].addr_bytes[2],
			       ports_eth_addr[portid].addr_bytes[3],
			       ports_eth_addr[portid].addr_bytes[4],
			       ports_eth_addr[portid].addr_bytes[5]);
#endif
		}
		/* only check for link status if the thread is master */
		check_all_ports_link_status(num_devices_attached, 0xFFFFFFFF);
	} else { /* CONFIG.multi_process && !CONFIG.multi_process_is_master */
		for (rxlcore_id = 0; rxlcore_id < num_cpus; rxlcore_id++) {
                        char name[RTE_MEMPOOL_NAMESIZE];
                        sprintf(name, "mbuf_pool-%d", rxlcore_id);
                        /* initialize the mbuf pools */
                        pktmbuf_pool[rxlcore_id] =
                                rte_mempool_lookup(name);
                        if (pktmbuf_pool[rxlcore_id] == NULL)
                                rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
                }

		int i;
		/* initializing dev_info struct */
		for (i = 0; i < num_devices_attached; i++) {
		        /* get portid form the index of attached devices */
		        portid = devices_attached[i];			
			/* check port capabilities */
			rte_eth_dev_info_get(i, &dev_info[portid]);
		}
	}
}
/*----------------------------------------------------------------------------*/
/**
 * Basic code structure to define Eternet flow directors per NIC using DPDK's
 * rte api. This piece of code has been derived from dpdk/examples/flow_filtering/
 * files.
 *
 * @param: nif: index of the dpdk-attached port
 * @param: qid: NIC queue ID
 * @param: err: error value store in case Ethernet flow director definition fails
 *
 * @ret: returns NULL on failure; populated rte_flow structure otherwise.
 */
static inline struct rte_flow *
attach_eth_filter(int nif, int qid, void *arg_ip_port, struct rte_flow_error *err)
{
	struct rte_flow_attr attr;
	struct rte_flow_item pattern[ETHER_FILTER_THRESH];
	struct rte_flow_action action[ETHER_FILTER_THRESH];
	struct rte_flow_action_queue queue = { .index = qid };
	struct rte_flow_item_eth eth_spec;
	struct rte_flow_item_eth eth_mask;
	struct rte_flow_item_vlan vlan_spec;
	struct rte_flow_item_vlan vlan_mask;
	struct rte_flow_item_ipv4 ip_spec;
	struct rte_flow_item_ipv4 ip_mask;
	struct rte_flow_item_tcp tcp_spec;
	struct rte_flow_item_tcp tcp_mask;
	struct sockaddr_in *addr;
	struct rte_flow *flow;
	int rc;

	addr = (struct sockaddr_in *)arg_ip_port;
	flow = NULL;
	memset(pattern, 0, sizeof(pattern));
	memset(action, 0, sizeof(action));

	/*
	 * set the rule attribute.
	 * in this case only ingress packets will be checked.
	 */
	memset(&attr, 0, sizeof(struct rte_flow_attr));
	attr.ingress = 1;

	/*
	 * create the action sequence.
	 * one action only, move packet to queue
	 */
	action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
	action[0].conf = &queue;
	action[1].type = RTE_FLOW_ACTION_TYPE_END;

	/*
	 * set the first level of the pattern (eth).
	 * since in this case, we get to L4, we set this level
	 * to allow all.
	 */
	memset(&eth_spec, 0, sizeof(struct rte_flow_item_eth));
	memset(&eth_mask, 0, sizeof(struct rte_flow_item_eth));
	eth_spec.type = 0;
	eth_mask.type = 0;
	pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
	pattern[0].spec = &eth_spec;
	pattern[0].mask = &eth_mask;
	
	/*
	 * setting the second level of the pattern (vlan).
	 * since in this case we want to get to all the
	 * way to L4, we also set this level to allow all.
	 */
	memset(&vlan_spec, 0, sizeof(struct rte_flow_item_vlan));
	memset(&vlan_mask, 0, sizeof(struct rte_flow_item_vlan));
	pattern[1].type = RTE_FLOW_ITEM_TYPE_VLAN;
	pattern[1].spec = &vlan_spec;
	pattern[1].mask = &vlan_mask;

	/*
	 * setting the third level of the pattern (ip).
	 * in this case this is the level we care about
	 * so we set it according to the parameters.
	 */
	memset(&ip_spec, 0, sizeof(struct rte_flow_item_ipv4));
	memset(&ip_mask, 0, sizeof(struct rte_flow_item_ipv4));
	ip_spec.hdr.dst_addr = addr->sin_addr.s_addr;
	ip_mask.hdr.dst_addr = 0xffffffff;
	ip_spec.hdr.src_addr = 0;
	ip_mask.hdr.src_addr = 0x0;
	pattern[2].type = RTE_FLOW_ITEM_TYPE_IPV4;
	pattern[2].spec = &ip_spec;
	pattern[2].mask = &ip_mask;

	/*
	 * setting the fourth level of the pattern (tcp).
	 * in this example this is the level we *really* care about
	 * so we set it according to the parameters.
	 */
	memset(&tcp_spec, 0, sizeof(struct rte_flow_item_tcp));
	memset(&tcp_mask, 0, sizeof(struct rte_flow_item_tcp));
	tcp_spec.hdr.dst_port = addr->sin_port;
	tcp_spec.hdr.src_port = 0;
	tcp_mask.hdr.dst_port = 0xffff;
	pattern[3].type = RTE_FLOW_ITEM_TYPE_TCP;
	pattern[3].spec = &tcp_spec;
	pattern[3].mask = &tcp_mask;

	/* the final level must be always type end */
	pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

	/* finally set the Ethernet flow director filter */
	rc = rte_flow_validate(nif, &attr, pattern, action, err);
	if (!rc)
		flow = rte_flow_create(nif, &attr, pattern, action, err);
#if 0
	rte_flow_flush(nif, err);
	rte_eth_dev_stop(nif);
	rte_eth_dev_start(nif);
#endif	
	return flow;
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_dev_ioctl(struct mtcp_thread_context *ctx, int nif, int cmd, void *argp)
{
	struct dpdk_private_context *dpc;
	struct rte_mbuf *m;
	int len_of_mbuf;
	struct iphdr *iph;
	struct tcphdr *tcph;
	void **argpptr = (void **)argp;
	struct rte_flow_error error;
	struct rte_flow *flow;
#ifdef ENABLELRO
	uint8_t *payload, *to;
	int seg_off;
#endif

	if (cmd == DRV_NAME) {
		*argpptr = (void *)dev_info[nif].driver_name;
		return 0;
	}

	int eidx = CONFIG.nif_to_eidx[nif];

	iph = (struct iphdr *)argp;
	dpc = (struct dpdk_private_context *)ctx->io_private_context;
	len_of_mbuf = dpc->wmbufs[eidx].len;

	switch (cmd) {
	case PKT_TX_IP_CSUM:
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM) == 0)
			goto dev_ioctl_err;
		m = dpc->wmbufs[eidx].m_table[len_of_mbuf - 1];
		m->ol_flags = PKT_TX_IP_CKSUM | PKT_TX_IPV4;
		m->l2_len = sizeof(struct ether_hdr);
		m->l3_len = (iph->ihl<<2);
		break;
	case PKT_TX_TCP_CSUM:
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_TCP_CKSUM) == 0)
			goto dev_ioctl_err;
		m = dpc->wmbufs[eidx].m_table[len_of_mbuf - 1];
		tcph = (struct tcphdr *)((unsigned char *)iph + (iph->ihl<<2));
		m->ol_flags |= PKT_TX_TCP_CKSUM;
		tcph->check = rte_ipv4_phdr_cksum((struct ipv4_hdr *)iph, m->ol_flags);
		break;
#ifdef ENABLELRO
	case PKT_RX_TCP_LROSEG:
		m = dpc->cur_rx_m;
		//if (m->next != NULL)
		//	rte_prefetch0(rte_pktmbuf_mtod(m->next, void *));
		iph = rte_pktmbuf_mtod_offset(m, struct iphdr *, sizeof(struct ether_hdr));
		tcph = (struct tcphdr *)((u_char *)iph + (iph->ihl << 2));
		payload = (uint8_t *)tcph + (tcph->doff << 2);

		seg_off = m->data_len -
			sizeof(struct ether_hdr) - (iph->ihl << 2) -
			(tcph->doff << 2);

		to = (uint8_t *) argp;
		m = m->next;
		memcpy(to, payload, seg_off);
		while (m != NULL) {
			//if (m->next != NULL)
			//	rte_prefetch0(rte_pktmbuf_mtod(m->next, void *));
			memcpy(to + seg_off,
			       rte_pktmbuf_mtod(m, uint8_t *),
			       m->data_len);
			seg_off += m->data_len;
			m = m->next;
		}
		break;
#endif
	case PKT_TX_TCPIP_CSUM:
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM) == 0)
			goto dev_ioctl_err;
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_TCP_CKSUM) == 0)
			goto dev_ioctl_err;
		m = dpc->wmbufs[eidx].m_table[len_of_mbuf - 1];
		iph = rte_pktmbuf_mtod_offset(m, struct iphdr *, sizeof(struct ether_hdr));
		tcph = (struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2));
		m->l2_len = sizeof(struct ether_hdr);
		m->l3_len = (iph->ihl<<2);
		m->l4_len = (tcph->doff<<2);
		m->ol_flags = PKT_TX_TCP_CKSUM | PKT_TX_IP_CKSUM | PKT_TX_IPV4;
		tcph->check = rte_ipv4_phdr_cksum((struct ipv4_hdr *)iph, m->ol_flags);
		break;
	case PKT_RX_IP_CSUM:
		if ((dev_info[nif].rx_offload_capa & DEV_RX_OFFLOAD_IPV4_CKSUM) == 0)
			goto dev_ioctl_err;
		break;
	case PKT_RX_TCP_CSUM:
		if ((dev_info[nif].rx_offload_capa & DEV_RX_OFFLOAD_TCP_CKSUM) == 0)
			goto dev_ioctl_err;
		break;
	case PKT_TX_TCPIP_CSUM_PEEK:
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_IPV4_CKSUM) == 0)
			goto dev_ioctl_err;
		if ((dev_info[nif].tx_offload_capa & DEV_TX_OFFLOAD_TCP_CKSUM) == 0)
			goto dev_ioctl_err;
		break;
	case ETH_ATTACH_FILTER:
		/* attach ip/port filter to the device */
		flow = attach_eth_filter(nif, ctx->cpu /*qid*/, argp, &error);
		if (!flow) {
			TRACE_ERROR("Failed to attach Ethernet flow directory filter: (%d) %s\n",
				    error.type,
				    error.message ? error.message : "(no stated reason)");
			goto dev_ioctl_err;
		}
		break;
	default:
		goto dev_ioctl_err;
	}
	return 0;
 dev_ioctl_err:
	return -1;
}
/*----------------------------------------------------------------------------*/
io_module_func dpdk_module_func = {
	.load_module		   = dpdk_load_module,
	.init_handle		   = dpdk_init_handle,
	.link_devices		   = dpdk_link_devices,
	.release_pkt		   = dpdk_release_pkt,
	.send_pkts		   = dpdk_send_pkts,
	.get_wptr   		   = dpdk_get_wptr,
	.recv_pkts		   = dpdk_recv_pkts,
	.get_rptr	   	   = dpdk_get_rptr,
	.select			   = dpdk_select,
	.destroy_handle		   = dpdk_destroy_handle,
	.dev_ioctl		   = dpdk_dev_ioctl
};
/*----------------------------------------------------------------------------*/
#else
io_module_func dpdk_module_func = {
	.load_module		   = NULL,
	.init_handle		   = NULL,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.send_pkts		   = NULL,
	.get_wptr   		   = NULL,
	.recv_pkts		   = NULL,
	.get_rptr	   	   = NULL,
	.select			   = NULL,
	.destroy_handle		   = NULL,
	.dev_ioctl		   = NULL
};
/*----------------------------------------------------------------------------*/
#endif /* !DISABLE_DPDK */
