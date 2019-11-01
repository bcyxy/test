/** 功能: 简单的转发数据包
 * 相邻的端口间转发：0 <--> 1、2 <--> 3 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define BURST_SIZE 32


typedef struct _Port_Rings
{
	uint32_t portId;
	uint32_t ringsId;
} Port_Rings;


static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IPV4 | ETH_RSS_IPV6,
		},
	},
};

const uint16_t tx_rings = 2;

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t ringCount) {
	int retval;
	retval = rte_eth_dev_configure(port,
                                   ringCount,
                                   0,
                                   &port_conf_default);
	if (retval != 0) {
		return retval;
    }

	uint16_t nb_rxd = 1024;
	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, NULL);
	if (retval != 0) {
		return retval;
    }

	uint16_t q;
	for (q = 0; q < ringCount; q++) {
		retval = rte_eth_rx_queue_setup(port,
                                        q,
                                        nb_rxd,
										rte_eth_dev_socket_id(port),
                                        NULL,
										mbuf_pool);
		if (retval < 0) {
			return retval;
        }
	}

	retval = rte_eth_dev_start(port);
	if (retval < 0) {
		return retval;
    }

	rte_eth_promiscuous_enable(port);

	return 0;
}


static inline int
lcore_main(void *arg) {
	Port_Rings *pPR = (Port_Rings*)arg;
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t buf;
	unsigned char *pktData;
	uint16_t pktDataLen;
	while (1) {
		//TODO 前边设置了多个队列，这里只收了一个队列的包
		const uint16_t nb_rx = rte_eth_rx_burst(pPR->portId,
												pPR->ringsId,
												bufs,
												BURST_SIZE);

		if (unlikely(nb_rx == 0)) {
			continue;
		}

		for (buf = 0; buf < nb_rx; buf++) {
			rte_prefetch0(rte_pktmbuf_mtod(bufs[buf], void *));
			pktDataLen = bufs[buf]->data_len;
			pktData = rte_pktmbuf_mtod(bufs[buf], unsigned char *);
			if (pktDataLen > 2) {
				printf("yxytest:%x|%x\n", pktData[0], pktData[1]);
			}
			rte_pktmbuf_free(bufs[buf]);
		}
	}
	return 0;
}


int main(int argc, char *argv[]) {
	const uint16_t ringsCountEachPort = 2;  // 每个端口的队列数

    //// 初始化环境
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	}

	uint32_t portCount = rte_eth_dev_count_avail();
	uint32_t lcoreCount = rte_lcore_count();
	if (portCount * ringsCountEachPort > lcoreCount - 1) {
		rte_exit(EXIT_FAILURE, "Error with lcore not enough.\n");
	}

	//// 创建报文内存池
	struct rte_mempool *mbuf_pool = NULL;
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
										10000,
										256,
										0,
										RTE_MBUF_DEFAULT_BUF_SIZE,
										rte_socket_id());
	if (mbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
	}

	//// 初始化端口
    uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if (port_init(portid, mbuf_pool, ringsCountEachPort) != 0) {
			rte_exit(EXIT_FAILURE, "Cannot init port %u\n", portid);
		}
	}

	//// 处理流量
	uint32_t i, j;
	uint32_t lcoreId = -1;
	for (i = 0; i < portCount; i++) {
		for (j = 0; j < ringsCountEachPort; j++) {
			lcoreId = rte_get_next_lcore(lcoreId, 1, 0);
			Port_Rings p_r = {
				.portId = i,
				.ringsId = j,
			};
			rte_eal_remote_launch(lcore_main, &p_r, lcoreId);
		}
	}

	RTE_LCORE_FOREACH_SLAVE(lcoreId) {
		if (rte_eal_wait_lcore(lcoreId) < 0)
			return -1;
	}

	return 0;
}
