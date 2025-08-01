// SPDX-License-Identifier: GPL-2.0-or-later
/**************************************************************************/
/*                                                                        */
/*  IBM System i and System p Virtual NIC Device Driver                   */
/*  Copyright (C) 2014 IBM Corp.                                          */
/*  Santiago Leon (santi_leon@yahoo.com)                                  */
/*  Thomas Falcon (tlfalcon@linux.vnet.ibm.com)                           */
/*  John Allen (jallen@linux.vnet.ibm.com)                                */
/*                                                                        */
/*                                                                        */
/* This module contains the implementation of a virtual ethernet device   */
/* for use with IBM i/p Series LPAR Linux. It utilizes the logical LAN    */
/* option of the RS/6000 Platform Architecture to interface with virtual  */
/* ethernet NICs that are presented to the partition by the hypervisor.   */
/*									   */
/* Messages are passed between the VNIC driver and the VNIC server using  */
/* Command/Response Queues (CRQs) and sub CRQs (sCRQs). CRQs are used to  */
/* issue and receive commands that initiate communication with the server */
/* on driver initialization. Sub CRQs (sCRQs) are similar to CRQs, but    */
/* are used by the driver to notify the server that a packet is           */
/* ready for transmission or that a buffer has been added to receive a    */
/* packet. Subsequently, sCRQs are used by the server to notify the       */
/* driver that a packet transmission has been completed or that a packet  */
/* has been received and placed in a waiting buffer.                      */
/*                                                                        */
/* In lieu of a more conventional "on-the-fly" DMA mapping strategy in    */
/* which skbs are DMA mapped and immediately unmapped when the transmit   */
/* or receive has been completed, the VNIC driver is required to use      */
/* "long term mapping". This entails that large, continuous DMA mapped    */
/* buffers are allocated on driver initialization and these buffers are   */
/* then continuously reused to pass skbs to and from the VNIC server.     */
/*                                                                        */
/**************************************************************************/

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <linux/ioport.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/ethtool.h>
#include <linux/proc_fs.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <net/net_namespace.h>
#include <asm/hvcall.h>
#include <linux/atomic.h>
#include <asm/vio.h>
#include <asm/xive.h>
#include <asm/iommu.h>
#include <linux/uaccess.h>
#include <asm/firmware.h>
#include <linux/workqueue.h>
#include <linux/if_vlan.h>
#include <linux/utsname.h>
#include <linux/cpu.h>

#include "ibmvnic.h"

static const char ibmvnic_driver_name[] = "ibmvnic";
static const char ibmvnic_driver_string[] = "IBM System i/p Virtual NIC Driver";

MODULE_AUTHOR("Santiago Leon");
MODULE_DESCRIPTION("IBM System i/p Virtual NIC Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(IBMVNIC_DRIVER_VERSION);

static int ibmvnic_version = IBMVNIC_INITIAL_VERSION;
static void release_sub_crqs(struct ibmvnic_adapter *, bool);
static int ibmvnic_reset_crq(struct ibmvnic_adapter *);
static int ibmvnic_send_crq_init(struct ibmvnic_adapter *);
static int ibmvnic_reenable_crq_queue(struct ibmvnic_adapter *);
static int ibmvnic_send_crq(struct ibmvnic_adapter *, union ibmvnic_crq *);
static int send_subcrq_indirect(struct ibmvnic_adapter *, u64, u64, u64);
static irqreturn_t ibmvnic_interrupt_rx(int irq, void *instance);
static int enable_scrq_irq(struct ibmvnic_adapter *,
			   struct ibmvnic_sub_crq_queue *);
static int disable_scrq_irq(struct ibmvnic_adapter *,
			    struct ibmvnic_sub_crq_queue *);
static int pending_scrq(struct ibmvnic_adapter *,
			struct ibmvnic_sub_crq_queue *);
static union sub_crq *ibmvnic_next_scrq(struct ibmvnic_adapter *,
					struct ibmvnic_sub_crq_queue *);
static int ibmvnic_poll(struct napi_struct *napi, int data);
static int reset_sub_crq_queues(struct ibmvnic_adapter *adapter);
static inline void reinit_init_done(struct ibmvnic_adapter *adapter);
static void send_query_map(struct ibmvnic_adapter *adapter);
static int send_request_map(struct ibmvnic_adapter *, dma_addr_t, u32, u8);
static int send_request_unmap(struct ibmvnic_adapter *, u8);
static int send_login(struct ibmvnic_adapter *adapter);
static void send_query_cap(struct ibmvnic_adapter *adapter);
static int init_sub_crqs(struct ibmvnic_adapter *);
static int init_sub_crq_irqs(struct ibmvnic_adapter *adapter);
static int ibmvnic_reset_init(struct ibmvnic_adapter *, bool reset);
static void release_crq_queue(struct ibmvnic_adapter *);
static int __ibmvnic_set_mac(struct net_device *, u8 *);
static int init_crq_queue(struct ibmvnic_adapter *adapter);
static int send_query_phys_parms(struct ibmvnic_adapter *adapter);
static void ibmvnic_tx_scrq_clean_buffer(struct ibmvnic_adapter *adapter,
					 struct ibmvnic_sub_crq_queue *tx_scrq);
static void free_long_term_buff(struct ibmvnic_adapter *adapter,
				struct ibmvnic_long_term_buff *ltb);
static void ibmvnic_disable_irqs(struct ibmvnic_adapter *adapter);
static void flush_reset_queue(struct ibmvnic_adapter *adapter);
static void print_subcrq_error(struct device *dev, int rc, const char *func);

struct ibmvnic_stat {
	char name[ETH_GSTRING_LEN];
	int offset;
};

#define IBMVNIC_STAT_OFF(stat) (offsetof(struct ibmvnic_adapter, stats) + \
			     offsetof(struct ibmvnic_statistics, stat))
#define IBMVNIC_GET_STAT(a, off) (*((u64 *)(((unsigned long)(a)) + (off))))

static const struct ibmvnic_stat ibmvnic_stats[] = {
	{"rx_packets", IBMVNIC_STAT_OFF(rx_packets)},
	{"rx_bytes", IBMVNIC_STAT_OFF(rx_bytes)},
	{"tx_packets", IBMVNIC_STAT_OFF(tx_packets)},
	{"tx_bytes", IBMVNIC_STAT_OFF(tx_bytes)},
	{"ucast_tx_packets", IBMVNIC_STAT_OFF(ucast_tx_packets)},
	{"ucast_rx_packets", IBMVNIC_STAT_OFF(ucast_rx_packets)},
	{"mcast_tx_packets", IBMVNIC_STAT_OFF(mcast_tx_packets)},
	{"mcast_rx_packets", IBMVNIC_STAT_OFF(mcast_rx_packets)},
	{"bcast_tx_packets", IBMVNIC_STAT_OFF(bcast_tx_packets)},
	{"bcast_rx_packets", IBMVNIC_STAT_OFF(bcast_rx_packets)},
	{"align_errors", IBMVNIC_STAT_OFF(align_errors)},
	{"fcs_errors", IBMVNIC_STAT_OFF(fcs_errors)},
	{"single_collision_frames", IBMVNIC_STAT_OFF(single_collision_frames)},
	{"multi_collision_frames", IBMVNIC_STAT_OFF(multi_collision_frames)},
	{"sqe_test_errors", IBMVNIC_STAT_OFF(sqe_test_errors)},
	{"deferred_tx", IBMVNIC_STAT_OFF(deferred_tx)},
	{"late_collisions", IBMVNIC_STAT_OFF(late_collisions)},
	{"excess_collisions", IBMVNIC_STAT_OFF(excess_collisions)},
	{"internal_mac_tx_errors", IBMVNIC_STAT_OFF(internal_mac_tx_errors)},
	{"carrier_sense", IBMVNIC_STAT_OFF(carrier_sense)},
	{"too_long_frames", IBMVNIC_STAT_OFF(too_long_frames)},
	{"internal_mac_rx_errors", IBMVNIC_STAT_OFF(internal_mac_rx_errors)},
};

static int send_crq_init_complete(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.generic.first = IBMVNIC_CRQ_INIT_CMD;
	crq.generic.cmd = IBMVNIC_CRQ_INIT_COMPLETE;

	return ibmvnic_send_crq(adapter, &crq);
}

static int send_version_xchg(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.version_exchange.first = IBMVNIC_CRQ_CMD;
	crq.version_exchange.cmd = VERSION_EXCHANGE;
	crq.version_exchange.version = cpu_to_be16(ibmvnic_version);

	return ibmvnic_send_crq(adapter, &crq);
}

static void ibmvnic_clean_queue_affinity(struct ibmvnic_adapter *adapter,
					 struct ibmvnic_sub_crq_queue *queue)
{
	if (!(queue && queue->irq))
		return;

	cpumask_clear(queue->affinity_mask);

	if (irq_set_affinity_and_hint(queue->irq, NULL))
		netdev_warn(adapter->netdev,
			    "%s: Clear affinity failed, queue addr = %p, IRQ = %d\n",
			    __func__, queue, queue->irq);
}

static void ibmvnic_clean_affinity(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_sub_crq_queue **rxqs;
	struct ibmvnic_sub_crq_queue **txqs;
	int num_rxqs, num_txqs;
	int i;

	rxqs = adapter->rx_scrq;
	txqs = adapter->tx_scrq;
	num_txqs = adapter->num_active_tx_scrqs;
	num_rxqs = adapter->num_active_rx_scrqs;

	netdev_dbg(adapter->netdev, "%s: Cleaning irq affinity hints", __func__);
	if (txqs) {
		for (i = 0; i < num_txqs; i++)
			ibmvnic_clean_queue_affinity(adapter, txqs[i]);
	}
	if (rxqs) {
		for (i = 0; i < num_rxqs; i++)
			ibmvnic_clean_queue_affinity(adapter, rxqs[i]);
	}
}

static int ibmvnic_set_queue_affinity(struct ibmvnic_sub_crq_queue *queue,
				      unsigned int *cpu, int *stragglers,
				      int stride)
{
	cpumask_var_t mask;
	int i;
	int rc = 0;

	if (!(queue && queue->irq))
		return rc;

	/* cpumask_var_t is either a pointer or array, allocation works here */
	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	/* while we have extra cpu give one extra to this irq */
	if (*stragglers) {
		stride++;
		(*stragglers)--;
	}
	/* atomic write is safer than writing bit by bit directly */
	for_each_online_cpu_wrap(i, *cpu) {
		if (!stride--) {
			/* For the next queue we start from the first
			 * unused CPU in this queue
			 */
			*cpu = i;
			break;
		}
		cpumask_set_cpu(i, mask);
	}

	/* set queue affinity mask */
	cpumask_copy(queue->affinity_mask, mask);
	rc = irq_set_affinity_and_hint(queue->irq, queue->affinity_mask);
	free_cpumask_var(mask);

	return rc;
}

/* assumes cpu read lock is held */
static void ibmvnic_set_affinity(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_sub_crq_queue **rxqs = adapter->rx_scrq;
	struct ibmvnic_sub_crq_queue **txqs = adapter->tx_scrq;
	struct ibmvnic_sub_crq_queue *queue;
	int num_rxqs = adapter->num_active_rx_scrqs, i_rxqs = 0;
	int num_txqs = adapter->num_active_tx_scrqs, i_txqs = 0;
	int total_queues, stride, stragglers, i;
	unsigned int num_cpu, cpu = 0;
	bool is_rx_queue;
	int rc = 0;

	netdev_dbg(adapter->netdev, "%s: Setting irq affinity hints", __func__);
	if (!(adapter->rx_scrq && adapter->tx_scrq)) {
		netdev_warn(adapter->netdev,
			    "%s: Set affinity failed, queues not allocated\n",
			    __func__);
		return;
	}

	total_queues = num_rxqs + num_txqs;
	num_cpu = num_online_cpus();
	/* number of cpu's assigned per irq */
	stride = max_t(int, num_cpu / total_queues, 1);
	/* number of leftover cpu's */
	stragglers = num_cpu >= total_queues ? num_cpu % total_queues : 0;

	for (i = 0; i < total_queues; i++) {
		is_rx_queue = false;
		/* balance core load by alternating rx and tx assignments
		 * ex: TX0 -> RX0 -> TX1 -> RX1 etc.
		 */
		if ((i % 2 == 1 && i_rxqs < num_rxqs) || i_txqs == num_txqs) {
			queue = rxqs[i_rxqs++];
			is_rx_queue = true;
		} else {
			queue = txqs[i_txqs++];
		}

		rc = ibmvnic_set_queue_affinity(queue, &cpu, &stragglers,
						stride);
		if (rc)
			goto out;

		if (!queue || is_rx_queue)
			continue;

		rc = __netif_set_xps_queue(adapter->netdev,
					   cpumask_bits(queue->affinity_mask),
					   i_txqs - 1, XPS_CPUS);
		if (rc)
			netdev_warn(adapter->netdev, "%s: Set XPS on queue %d failed, rc = %d.\n",
				    __func__, i_txqs - 1, rc);
	}

out:
	if (rc) {
		netdev_warn(adapter->netdev,
			    "%s: Set affinity failed, queue addr = %p, IRQ = %d, rc = %d.\n",
			    __func__, queue, queue->irq, rc);
		ibmvnic_clean_affinity(adapter);
	}
}

static int ibmvnic_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct ibmvnic_adapter *adapter;

	adapter = hlist_entry_safe(node, struct ibmvnic_adapter, node);
	ibmvnic_set_affinity(adapter);
	return 0;
}

static int ibmvnic_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct ibmvnic_adapter *adapter;

	adapter = hlist_entry_safe(node, struct ibmvnic_adapter, node_dead);
	ibmvnic_set_affinity(adapter);
	return 0;
}

static int ibmvnic_cpu_down_prep(unsigned int cpu, struct hlist_node *node)
{
	struct ibmvnic_adapter *adapter;

	adapter = hlist_entry_safe(node, struct ibmvnic_adapter, node);
	ibmvnic_clean_affinity(adapter);
	return 0;
}

static enum cpuhp_state ibmvnic_online;

static int ibmvnic_cpu_notif_add(struct ibmvnic_adapter *adapter)
{
	int ret;

	ret = cpuhp_state_add_instance_nocalls(ibmvnic_online, &adapter->node);
	if (ret)
		return ret;
	ret = cpuhp_state_add_instance_nocalls(CPUHP_IBMVNIC_DEAD,
					       &adapter->node_dead);
	if (!ret)
		return ret;
	cpuhp_state_remove_instance_nocalls(ibmvnic_online, &adapter->node);
	return ret;
}

static void ibmvnic_cpu_notif_remove(struct ibmvnic_adapter *adapter)
{
	cpuhp_state_remove_instance_nocalls(ibmvnic_online, &adapter->node);
	cpuhp_state_remove_instance_nocalls(CPUHP_IBMVNIC_DEAD,
					    &adapter->node_dead);
}

static long h_reg_sub_crq(unsigned long unit_address, unsigned long token,
			  unsigned long length, unsigned long *number,
			  unsigned long *irq)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	long rc;

	rc = plpar_hcall(H_REG_SUB_CRQ, retbuf, unit_address, token, length);
	*number = retbuf[0];
	*irq = retbuf[1];

	return rc;
}

/**
 * ibmvnic_wait_for_completion - Check device state and wait for completion
 * @adapter: private device data
 * @comp_done: completion structure to wait for
 * @timeout: time to wait in milliseconds
 *
 * Wait for a completion signal or until the timeout limit is reached
 * while checking that the device is still active.
 */
static int ibmvnic_wait_for_completion(struct ibmvnic_adapter *adapter,
				       struct completion *comp_done,
				       unsigned long timeout)
{
	struct net_device *netdev;
	unsigned long div_timeout;
	u8 retry;

	netdev = adapter->netdev;
	retry = 5;
	div_timeout = msecs_to_jiffies(timeout / retry);
	while (true) {
		if (!adapter->crq.active) {
			netdev_err(netdev, "Device down!\n");
			return -ENODEV;
		}
		if (!retry--)
			break;
		if (wait_for_completion_timeout(comp_done, div_timeout))
			return 0;
	}
	netdev_err(netdev, "Operation timed out.\n");
	return -ETIMEDOUT;
}

/**
 * reuse_ltb() - Check if a long term buffer can be reused
 * @ltb:  The long term buffer to be checked
 * @size: The size of the long term buffer.
 *
 * An LTB can be reused unless its size has changed.
 *
 * Return: Return true if the LTB can be reused, false otherwise.
 */
static bool reuse_ltb(struct ibmvnic_long_term_buff *ltb, int size)
{
	return (ltb->buff && ltb->size == size);
}

/**
 * alloc_long_term_buff() - Allocate a long term buffer (LTB)
 *
 * @adapter: ibmvnic adapter associated to the LTB
 * @ltb:     container object for the LTB
 * @size:    size of the LTB
 *
 * Allocate an LTB of the specified size and notify VIOS.
 *
 * If the given @ltb already has the correct size, reuse it. Otherwise if
 * its non-NULL, free it. Then allocate a new one of the correct size.
 * Notify the VIOS either way since we may now be working with a new VIOS.
 *
 * Allocating larger chunks of memory during resets, specially LPM or under
 * low memory situations can cause resets to fail/timeout and for LPAR to
 * lose connectivity. So hold onto the LTB even if we fail to communicate
 * with the VIOS and reuse it on next open. Free LTB when adapter is closed.
 *
 * Return: 0 if we were able to allocate the LTB and notify the VIOS and
 *	   a negative value otherwise.
 */
static int alloc_long_term_buff(struct ibmvnic_adapter *adapter,
				struct ibmvnic_long_term_buff *ltb, int size)
{
	struct device *dev = &adapter->vdev->dev;
	u64 prev = 0;
	int rc;

	if (!reuse_ltb(ltb, size)) {
		dev_dbg(dev,
			"LTB size changed from 0x%llx to 0x%x, reallocating\n",
			 ltb->size, size);
		prev = ltb->size;
		free_long_term_buff(adapter, ltb);
	}

	if (ltb->buff) {
		dev_dbg(dev, "Reusing LTB [map %d, size 0x%llx]\n",
			ltb->map_id, ltb->size);
	} else {
		ltb->buff = dma_alloc_coherent(dev, size, &ltb->addr,
					       GFP_KERNEL);
		if (!ltb->buff) {
			dev_err(dev, "Couldn't alloc long term buffer\n");
			return -ENOMEM;
		}
		ltb->size = size;

		ltb->map_id = find_first_zero_bit(adapter->map_ids,
						  MAX_MAP_ID);
		bitmap_set(adapter->map_ids, ltb->map_id, 1);

		dev_dbg(dev,
			"Allocated new LTB [map %d, size 0x%llx was 0x%llx]\n",
			 ltb->map_id, ltb->size, prev);
	}

	/* Ensure ltb is zeroed - specially when reusing it. */
	memset(ltb->buff, 0, ltb->size);

	mutex_lock(&adapter->fw_lock);
	adapter->fw_done_rc = 0;
	reinit_completion(&adapter->fw_done);

	rc = send_request_map(adapter, ltb->addr, ltb->size, ltb->map_id);
	if (rc) {
		dev_err(dev, "send_request_map failed, rc = %d\n", rc);
		goto out;
	}

	rc = ibmvnic_wait_for_completion(adapter, &adapter->fw_done, 10000);
	if (rc) {
		dev_err(dev, "LTB map request aborted or timed out, rc = %d\n",
			rc);
		goto out;
	}

	if (adapter->fw_done_rc) {
		dev_err(dev, "Couldn't map LTB, rc = %d\n",
			adapter->fw_done_rc);
		rc = -EIO;
		goto out;
	}
	rc = 0;
out:
	/* don't free LTB on communication error - see function header */
	mutex_unlock(&adapter->fw_lock);
	return rc;
}

static void free_long_term_buff(struct ibmvnic_adapter *adapter,
				struct ibmvnic_long_term_buff *ltb)
{
	struct device *dev = &adapter->vdev->dev;

	if (!ltb->buff)
		return;

	/* VIOS automatically unmaps the long term buffer at remote
	 * end for the following resets:
	 * FAILOVER, MOBILITY, TIMEOUT.
	 */
	if (adapter->reset_reason != VNIC_RESET_FAILOVER &&
	    adapter->reset_reason != VNIC_RESET_MOBILITY &&
	    adapter->reset_reason != VNIC_RESET_TIMEOUT)
		send_request_unmap(adapter, ltb->map_id);

	dma_free_coherent(dev, ltb->size, ltb->buff, ltb->addr);

	ltb->buff = NULL;
	/* mark this map_id free */
	bitmap_clear(adapter->map_ids, ltb->map_id, 1);
	ltb->map_id = 0;
}

/**
 * free_ltb_set - free the given set of long term buffers (LTBS)
 * @adapter: The ibmvnic adapter containing this ltb set
 * @ltb_set: The ltb_set to be freed
 *
 * Free the set of LTBs in the given set.
 */

static void free_ltb_set(struct ibmvnic_adapter *adapter,
			 struct ibmvnic_ltb_set *ltb_set)
{
	int i;

	for (i = 0; i < ltb_set->num_ltbs; i++)
		free_long_term_buff(adapter, &ltb_set->ltbs[i]);

	kfree(ltb_set->ltbs);
	ltb_set->ltbs = NULL;
	ltb_set->num_ltbs = 0;
}

/**
 * alloc_ltb_set() - Allocate a set of long term buffers (LTBs)
 *
 * @adapter: ibmvnic adapter associated to the LTB
 * @ltb_set: container object for the set of LTBs
 * @num_buffs: Number of buffers in the LTB
 * @buff_size: Size of each buffer in the LTB
 *
 * Allocate a set of LTBs to accommodate @num_buffs buffers of @buff_size
 * each. We currently cap size each LTB to IBMVNIC_ONE_LTB_SIZE. If the
 * new set of LTBs have fewer LTBs than the old set, free the excess LTBs.
 * If new set needs more than in old set, allocate the remaining ones.
 * Try and reuse as many LTBs as possible and avoid reallocation.
 *
 * Any changes to this allocation strategy must be reflected in
 * map_rxpool_buff_to_ltb() and map_txpool_buff_to_ltb().
 */
static int alloc_ltb_set(struct ibmvnic_adapter *adapter,
			 struct ibmvnic_ltb_set *ltb_set, int num_buffs,
			 int buff_size)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_ltb_set old_set;
	struct ibmvnic_ltb_set new_set;
	int rem_size;
	int tot_size;		/* size of all ltbs */
	int ltb_size;		/* size of one ltb */
	int nltbs;
	int rc;
	int n;
	int i;

	dev_dbg(dev, "%s() num_buffs %d, buff_size %d\n", __func__, num_buffs,
		buff_size);

	ltb_size = rounddown(IBMVNIC_ONE_LTB_SIZE, buff_size);
	tot_size = num_buffs * buff_size;

	if (ltb_size > tot_size)
		ltb_size = tot_size;

	nltbs = tot_size / ltb_size;
	if (tot_size % ltb_size)
		nltbs++;

	old_set = *ltb_set;

	if (old_set.num_ltbs == nltbs) {
		new_set = old_set;
	} else {
		int tmp = nltbs * sizeof(struct ibmvnic_long_term_buff);

		new_set.ltbs = kzalloc(tmp, GFP_KERNEL);
		if (!new_set.ltbs)
			return -ENOMEM;

		new_set.num_ltbs = nltbs;

		/* Free any excess ltbs in old set */
		for (i = new_set.num_ltbs; i < old_set.num_ltbs; i++)
			free_long_term_buff(adapter, &old_set.ltbs[i]);

		/* Copy remaining ltbs to new set. All LTBs except the
		 * last one are of the same size. alloc_long_term_buff()
		 * will realloc if the size changes.
		 */
		n = min(old_set.num_ltbs, new_set.num_ltbs);
		for (i = 0; i < n; i++)
			new_set.ltbs[i] = old_set.ltbs[i];

		/* Any additional ltbs in new set will have NULL ltbs for
		 * now and will be allocated in alloc_long_term_buff().
		 */

		/* We no longer need the old_set so free it. Note that we
		 * may have reused some ltbs from old set and freed excess
		 * ltbs above. So we only need to free the container now
		 * not the LTBs themselves. (i.e. dont free_ltb_set()!)
		 */
		kfree(old_set.ltbs);
		old_set.ltbs = NULL;
		old_set.num_ltbs = 0;

		/* Install the new set. If allocations fail below, we will
		 * retry later and know what size LTBs we need.
		 */
		*ltb_set = new_set;
	}

	i = 0;
	rem_size = tot_size;
	while (rem_size) {
		if (ltb_size > rem_size)
			ltb_size = rem_size;

		rem_size -= ltb_size;

		rc = alloc_long_term_buff(adapter, &new_set.ltbs[i], ltb_size);
		if (rc)
			goto out;
		i++;
	}

	WARN_ON(i != new_set.num_ltbs);

	return 0;
out:
	/* We may have allocated one/more LTBs before failing and we
	 * want to try and reuse on next reset. So don't free ltb set.
	 */
	return rc;
}

/**
 * map_rxpool_buf_to_ltb - Map given rxpool buffer to offset in an LTB.
 * @rxpool: The receive buffer pool containing buffer
 * @bufidx: Index of buffer in rxpool
 * @ltbp: (Output) pointer to the long term buffer containing the buffer
 * @offset: (Output) offset of buffer in the LTB from @ltbp
 *
 * Map the given buffer identified by [rxpool, bufidx] to an LTB in the
 * pool and its corresponding offset. Assume for now that each LTB is of
 * different size but could possibly be optimized based on the allocation
 * strategy in alloc_ltb_set().
 */
static void map_rxpool_buf_to_ltb(struct ibmvnic_rx_pool *rxpool,
				  unsigned int bufidx,
				  struct ibmvnic_long_term_buff **ltbp,
				  unsigned int *offset)
{
	struct ibmvnic_long_term_buff *ltb;
	int nbufs;	/* # of buffers in one ltb */
	int i;

	WARN_ON(bufidx >= rxpool->size);

	for (i = 0; i < rxpool->ltb_set.num_ltbs; i++) {
		ltb = &rxpool->ltb_set.ltbs[i];
		nbufs = ltb->size / rxpool->buff_size;
		if (bufidx < nbufs)
			break;
		bufidx -= nbufs;
	}

	*ltbp = ltb;
	*offset = bufidx * rxpool->buff_size;
}

/**
 * map_txpool_buf_to_ltb - Map given txpool buffer to offset in an LTB.
 * @txpool: The transmit buffer pool containing buffer
 * @bufidx: Index of buffer in txpool
 * @ltbp: (Output) pointer to the long term buffer (LTB) containing the buffer
 * @offset: (Output) offset of buffer in the LTB from @ltbp
 *
 * Map the given buffer identified by [txpool, bufidx] to an LTB in the
 * pool and its corresponding offset.
 */
static void map_txpool_buf_to_ltb(struct ibmvnic_tx_pool *txpool,
				  unsigned int bufidx,
				  struct ibmvnic_long_term_buff **ltbp,
				  unsigned int *offset)
{
	struct ibmvnic_long_term_buff *ltb;
	int nbufs;	/* # of buffers in one ltb */
	int i;

	WARN_ON_ONCE(bufidx >= txpool->num_buffers);

	for (i = 0; i < txpool->ltb_set.num_ltbs; i++) {
		ltb = &txpool->ltb_set.ltbs[i];
		nbufs = ltb->size / txpool->buf_size;
		if (bufidx < nbufs)
			break;
		bufidx -= nbufs;
	}

	*ltbp = ltb;
	*offset = bufidx * txpool->buf_size;
}

static void deactivate_rx_pools(struct ibmvnic_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_active_rx_pools; i++)
		adapter->rx_pool[i].active = 0;
}

static void replenish_rx_pool(struct ibmvnic_adapter *adapter,
			      struct ibmvnic_rx_pool *pool)
{
	int count = pool->size - atomic_read(&pool->available);
	u64 handle = adapter->rx_scrq[pool->index]->handle;
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_ind_xmit_queue *ind_bufp;
	struct ibmvnic_sub_crq_queue *rx_scrq;
	struct ibmvnic_long_term_buff *ltb;
	union sub_crq *sub_crq;
	int buffers_added = 0;
	unsigned long lpar_rc;
	struct sk_buff *skb;
	unsigned int offset;
	dma_addr_t dma_addr;
	unsigned char *dst;
	int shift = 0;
	int bufidx;
	int i;

	if (!pool->active)
		return;

	rx_scrq = adapter->rx_scrq[pool->index];
	ind_bufp = &rx_scrq->ind_buf;

	/* netdev_skb_alloc() could have failed after we saved a few skbs
	 * in the indir_buf and we would not have sent them to VIOS yet.
	 * To account for them, start the loop at ind_bufp->index rather
	 * than 0. If we pushed all the skbs to VIOS, ind_bufp->index will
	 * be 0.
	 */
	for (i = ind_bufp->index; i < count; ++i) {
		bufidx = pool->free_map[pool->next_free];

		/* We maybe reusing the skb from earlier resets. Allocate
		 * only if necessary. But since the LTB may have changed
		 * during reset (see init_rx_pools()), update LTB below
		 * even if reusing skb.
		 */
		skb = pool->rx_buff[bufidx].skb;
		if (!skb) {
			skb = netdev_alloc_skb(adapter->netdev,
					       pool->buff_size);
			if (!skb) {
				dev_err(dev, "Couldn't replenish rx buff\n");
				adapter->replenish_no_mem++;
				break;
			}
		}

		pool->free_map[pool->next_free] = IBMVNIC_INVALID_MAP;
		pool->next_free = (pool->next_free + 1) % pool->size;

		/* Copy the skb to the long term mapped DMA buffer */
		map_rxpool_buf_to_ltb(pool, bufidx, &ltb, &offset);
		dst = ltb->buff + offset;
		memset(dst, 0, pool->buff_size);
		dma_addr = ltb->addr + offset;

		/* add the skb to an rx_buff in the pool */
		pool->rx_buff[bufidx].data = dst;
		pool->rx_buff[bufidx].dma = dma_addr;
		pool->rx_buff[bufidx].skb = skb;
		pool->rx_buff[bufidx].pool_index = pool->index;
		pool->rx_buff[bufidx].size = pool->buff_size;

		/* queue the rx_buff for the next send_subcrq_indirect */
		sub_crq = &ind_bufp->indir_arr[ind_bufp->index++];
		memset(sub_crq, 0, sizeof(*sub_crq));
		sub_crq->rx_add.first = IBMVNIC_CRQ_CMD;
		sub_crq->rx_add.correlator =
		    cpu_to_be64((u64)&pool->rx_buff[bufidx]);
		sub_crq->rx_add.ioba = cpu_to_be32(dma_addr);
		sub_crq->rx_add.map_id = ltb->map_id;

		/* The length field of the sCRQ is defined to be 24 bits so the
		 * buffer size needs to be left shifted by a byte before it is
		 * converted to big endian to prevent the last byte from being
		 * truncated.
		 */
#ifdef __LITTLE_ENDIAN__
		shift = 8;
#endif
		sub_crq->rx_add.len = cpu_to_be32(pool->buff_size << shift);

		/* if send_subcrq_indirect queue is full, flush to VIOS */
		if (ind_bufp->index == IBMVNIC_MAX_IND_DESCS ||
		    i == count - 1) {
			lpar_rc =
				send_subcrq_indirect(adapter, handle,
						     (u64)ind_bufp->indir_dma,
						     (u64)ind_bufp->index);
			if (lpar_rc != H_SUCCESS)
				goto failure;
			buffers_added += ind_bufp->index;
			adapter->replenish_add_buff_success += ind_bufp->index;
			ind_bufp->index = 0;
		}
	}
	atomic_add(buffers_added, &pool->available);
	return;

failure:
	if (lpar_rc != H_PARAMETER && lpar_rc != H_CLOSED)
		dev_err_ratelimited(dev, "rx: replenish packet buffer failed\n");
	for (i = ind_bufp->index - 1; i >= 0; --i) {
		struct ibmvnic_rx_buff *rx_buff;

		pool->next_free = pool->next_free == 0 ?
				  pool->size - 1 : pool->next_free - 1;
		sub_crq = &ind_bufp->indir_arr[i];
		rx_buff = (struct ibmvnic_rx_buff *)
				be64_to_cpu(sub_crq->rx_add.correlator);
		bufidx = (int)(rx_buff - pool->rx_buff);
		pool->free_map[pool->next_free] = bufidx;
		dev_kfree_skb_any(pool->rx_buff[bufidx].skb);
		pool->rx_buff[bufidx].skb = NULL;
	}
	adapter->replenish_add_buff_failure += ind_bufp->index;
	atomic_add(buffers_added, &pool->available);
	ind_bufp->index = 0;
	if (lpar_rc == H_CLOSED || adapter->failover_pending) {
		/* Disable buffer pool replenishment and report carrier off if
		 * queue is closed or pending failover.
		 * Firmware guarantees that a signal will be sent to the
		 * driver, triggering a reset.
		 */
		deactivate_rx_pools(adapter);
		netif_carrier_off(adapter->netdev);
	}
}

static void replenish_pools(struct ibmvnic_adapter *adapter)
{
	int i;

	adapter->replenish_task_cycles++;
	for (i = 0; i < adapter->num_active_rx_pools; i++) {
		if (adapter->rx_pool[i].active)
			replenish_rx_pool(adapter, &adapter->rx_pool[i]);
	}

	netdev_dbg(adapter->netdev, "Replenished %d pools\n", i);
}

static void release_stats_buffers(struct ibmvnic_adapter *adapter)
{
	kfree(adapter->tx_stats_buffers);
	kfree(adapter->rx_stats_buffers);
	adapter->tx_stats_buffers = NULL;
	adapter->rx_stats_buffers = NULL;
}

static int init_stats_buffers(struct ibmvnic_adapter *adapter)
{
	adapter->tx_stats_buffers =
				kcalloc(IBMVNIC_MAX_QUEUES,
					sizeof(struct ibmvnic_tx_queue_stats),
					GFP_KERNEL);
	if (!adapter->tx_stats_buffers)
		return -ENOMEM;

	adapter->rx_stats_buffers =
				kcalloc(IBMVNIC_MAX_QUEUES,
					sizeof(struct ibmvnic_rx_queue_stats),
					GFP_KERNEL);
	if (!adapter->rx_stats_buffers)
		return -ENOMEM;

	return 0;
}

static void release_stats_token(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	if (!adapter->stats_token)
		return;

	dma_unmap_single(dev, adapter->stats_token,
			 sizeof(struct ibmvnic_statistics),
			 DMA_FROM_DEVICE);
	adapter->stats_token = 0;
}

static int init_stats_token(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	dma_addr_t stok;
	int rc;

	stok = dma_map_single(dev, &adapter->stats,
			      sizeof(struct ibmvnic_statistics),
			      DMA_FROM_DEVICE);
	rc = dma_mapping_error(dev, stok);
	if (rc) {
		dev_err(dev, "Couldn't map stats buffer, rc = %d\n", rc);
		return rc;
	}

	adapter->stats_token = stok;
	netdev_dbg(adapter->netdev, "Stats token initialized (%llx)\n", stok);
	return 0;
}

/**
 * release_rx_pools() - Release any rx pools attached to @adapter.
 * @adapter: ibmvnic adapter
 *
 * Safe to call this multiple times - even if no pools are attached.
 */
static void release_rx_pools(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rx_pool *rx_pool;
	int i, j;

	if (!adapter->rx_pool)
		return;

	for (i = 0; i < adapter->num_active_rx_pools; i++) {
		rx_pool = &adapter->rx_pool[i];

		netdev_dbg(adapter->netdev, "Releasing rx_pool[%d]\n", i);

		kfree(rx_pool->free_map);

		free_ltb_set(adapter, &rx_pool->ltb_set);

		if (!rx_pool->rx_buff)
			continue;

		for (j = 0; j < rx_pool->size; j++) {
			if (rx_pool->rx_buff[j].skb) {
				dev_kfree_skb_any(rx_pool->rx_buff[j].skb);
				rx_pool->rx_buff[j].skb = NULL;
			}
		}

		kfree(rx_pool->rx_buff);
	}

	kfree(adapter->rx_pool);
	adapter->rx_pool = NULL;
	adapter->num_active_rx_pools = 0;
	adapter->prev_rx_pool_size = 0;
}

/**
 * reuse_rx_pools() - Check if the existing rx pools can be reused.
 * @adapter: ibmvnic adapter
 *
 * Check if the existing rx pools in the adapter can be reused. The
 * pools can be reused if the pool parameters (number of pools,
 * number of buffers in the pool and size of each buffer) have not
 * changed.
 *
 * NOTE: This assumes that all pools have the same number of buffers
 *       which is the case currently. If that changes, we must fix this.
 *
 * Return: true if the rx pools can be reused, false otherwise.
 */
static bool reuse_rx_pools(struct ibmvnic_adapter *adapter)
{
	u64 old_num_pools, new_num_pools;
	u64 old_pool_size, new_pool_size;
	u64 old_buff_size, new_buff_size;

	if (!adapter->rx_pool)
		return false;

	old_num_pools = adapter->num_active_rx_pools;
	new_num_pools = adapter->req_rx_queues;

	old_pool_size = adapter->prev_rx_pool_size;
	new_pool_size = adapter->req_rx_add_entries_per_subcrq;

	old_buff_size = adapter->prev_rx_buf_sz;
	new_buff_size = adapter->cur_rx_buf_sz;

	if (old_buff_size != new_buff_size ||
	    old_num_pools != new_num_pools ||
	    old_pool_size != new_pool_size)
		return false;

	return true;
}

/**
 * init_rx_pools(): Initialize the set of receiver pools in the adapter.
 * @netdev: net device associated with the vnic interface
 *
 * Initialize the set of receiver pools in the ibmvnic adapter associated
 * with the net_device @netdev. If possible, reuse the existing rx pools.
 * Otherwise free any existing pools and  allocate a new set of pools
 * before initializing them.
 *
 * Return: 0 on success and negative value on error.
 */
static int init_rx_pools(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_rx_pool *rx_pool;
	u64 num_pools;
	u64 pool_size;		/* # of buffers in one pool */
	u64 buff_size;
	int i, j, rc;

	pool_size = adapter->req_rx_add_entries_per_subcrq;
	num_pools = adapter->req_rx_queues;
	buff_size = adapter->cur_rx_buf_sz;

	if (reuse_rx_pools(adapter)) {
		dev_dbg(dev, "Reusing rx pools\n");
		goto update_ltb;
	}

	/* Allocate/populate the pools. */
	release_rx_pools(adapter);

	adapter->rx_pool = kcalloc(num_pools,
				   sizeof(struct ibmvnic_rx_pool),
				   GFP_KERNEL);
	if (!adapter->rx_pool) {
		dev_err(dev, "Failed to allocate rx pools\n");
		return -ENOMEM;
	}

	/* Set num_active_rx_pools early. If we fail below after partial
	 * allocation, release_rx_pools() will know how many to look for.
	 */
	adapter->num_active_rx_pools = num_pools;

	for (i = 0; i < num_pools; i++) {
		rx_pool = &adapter->rx_pool[i];

		netdev_dbg(adapter->netdev,
			   "Initializing rx_pool[%d], %lld buffs, %lld bytes each\n",
			   i, pool_size, buff_size);

		rx_pool->size = pool_size;
		rx_pool->index = i;
		rx_pool->buff_size = ALIGN(buff_size, L1_CACHE_BYTES);

		rx_pool->free_map = kcalloc(rx_pool->size, sizeof(int),
					    GFP_KERNEL);
		if (!rx_pool->free_map) {
			dev_err(dev, "Couldn't alloc free_map %d\n", i);
			rc = -ENOMEM;
			goto out_release;
		}

		rx_pool->rx_buff = kcalloc(rx_pool->size,
					   sizeof(struct ibmvnic_rx_buff),
					   GFP_KERNEL);
		if (!rx_pool->rx_buff) {
			dev_err(dev, "Couldn't alloc rx buffers\n");
			rc = -ENOMEM;
			goto out_release;
		}
	}

	adapter->prev_rx_pool_size = pool_size;
	adapter->prev_rx_buf_sz = adapter->cur_rx_buf_sz;

update_ltb:
	for (i = 0; i < num_pools; i++) {
		rx_pool = &adapter->rx_pool[i];
		dev_dbg(dev, "Updating LTB for rx pool %d [%d, %d]\n",
			i, rx_pool->size, rx_pool->buff_size);

		rc = alloc_ltb_set(adapter, &rx_pool->ltb_set,
				   rx_pool->size, rx_pool->buff_size);
		if (rc)
			goto out;

		for (j = 0; j < rx_pool->size; ++j) {
			struct ibmvnic_rx_buff *rx_buff;

			rx_pool->free_map[j] = j;

			/* NOTE: Don't clear rx_buff->skb here - will leak
			 * memory! replenish_rx_pool() will reuse skbs or
			 * allocate as necessary.
			 */
			rx_buff = &rx_pool->rx_buff[j];
			rx_buff->dma = 0;
			rx_buff->data = 0;
			rx_buff->size = 0;
			rx_buff->pool_index = 0;
		}

		/* Mark pool "empty" so replenish_rx_pools() will
		 * update the LTB info for each buffer
		 */
		atomic_set(&rx_pool->available, 0);
		rx_pool->next_alloc = 0;
		rx_pool->next_free = 0;
		/* replenish_rx_pool() may have called deactivate_rx_pools()
		 * on failover. Ensure pool is active now.
		 */
		rx_pool->active = 1;
	}
	return 0;
out_release:
	release_rx_pools(adapter);
out:
	/* We failed to allocate one or more LTBs or map them on the VIOS.
	 * Hold onto the pools and any LTBs that we did allocate/map.
	 */
	return rc;
}

static void release_vpd_data(struct ibmvnic_adapter *adapter)
{
	if (!adapter->vpd)
		return;

	kfree(adapter->vpd->buff);
	kfree(adapter->vpd);

	adapter->vpd = NULL;
}

static void release_one_tx_pool(struct ibmvnic_adapter *adapter,
				struct ibmvnic_tx_pool *tx_pool)
{
	kfree(tx_pool->tx_buff);
	kfree(tx_pool->free_map);
	free_ltb_set(adapter, &tx_pool->ltb_set);
}

/**
 * release_tx_pools() - Release any tx pools attached to @adapter.
 * @adapter: ibmvnic adapter
 *
 * Safe to call this multiple times - even if no pools are attached.
 */
static void release_tx_pools(struct ibmvnic_adapter *adapter)
{
	int i;

	/* init_tx_pools() ensures that ->tx_pool and ->tso_pool are
	 * both NULL or both non-NULL. So we only need to check one.
	 */
	if (!adapter->tx_pool)
		return;

	for (i = 0; i < adapter->num_active_tx_pools; i++) {
		release_one_tx_pool(adapter, &adapter->tx_pool[i]);
		release_one_tx_pool(adapter, &adapter->tso_pool[i]);
	}

	kfree(adapter->tx_pool);
	adapter->tx_pool = NULL;
	kfree(adapter->tso_pool);
	adapter->tso_pool = NULL;
	adapter->num_active_tx_pools = 0;
	adapter->prev_tx_pool_size = 0;
}

static int init_one_tx_pool(struct net_device *netdev,
			    struct ibmvnic_tx_pool *tx_pool,
			    int pool_size, int buf_size)
{
	int i;

	tx_pool->tx_buff = kcalloc(pool_size,
				   sizeof(struct ibmvnic_tx_buff),
				   GFP_KERNEL);
	if (!tx_pool->tx_buff)
		return -ENOMEM;

	tx_pool->free_map = kcalloc(pool_size, sizeof(int), GFP_KERNEL);
	if (!tx_pool->free_map) {
		kfree(tx_pool->tx_buff);
		tx_pool->tx_buff = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < pool_size; i++)
		tx_pool->free_map[i] = i;

	tx_pool->consumer_index = 0;
	tx_pool->producer_index = 0;
	tx_pool->num_buffers = pool_size;
	tx_pool->buf_size = buf_size;

	return 0;
}

/**
 * reuse_tx_pools() - Check if the existing tx pools can be reused.
 * @adapter: ibmvnic adapter
 *
 * Check if the existing tx pools in the adapter can be reused. The
 * pools can be reused if the pool parameters (number of pools,
 * number of buffers in the pool and mtu) have not changed.
 *
 * NOTE: This assumes that all pools have the same number of buffers
 *       which is the case currently. If that changes, we must fix this.
 *
 * Return: true if the tx pools can be reused, false otherwise.
 */
static bool reuse_tx_pools(struct ibmvnic_adapter *adapter)
{
	u64 old_num_pools, new_num_pools;
	u64 old_pool_size, new_pool_size;
	u64 old_mtu, new_mtu;

	if (!adapter->tx_pool)
		return false;

	old_num_pools = adapter->num_active_tx_pools;
	new_num_pools = adapter->num_active_tx_scrqs;
	old_pool_size = adapter->prev_tx_pool_size;
	new_pool_size = adapter->req_tx_entries_per_subcrq;
	old_mtu = adapter->prev_mtu;
	new_mtu = adapter->req_mtu;

	if (old_mtu != new_mtu ||
	    old_num_pools != new_num_pools ||
	    old_pool_size != new_pool_size)
		return false;

	return true;
}

/**
 * init_tx_pools(): Initialize the set of transmit pools in the adapter.
 * @netdev: net device associated with the vnic interface
 *
 * Initialize the set of transmit pools in the ibmvnic adapter associated
 * with the net_device @netdev. If possible, reuse the existing tx pools.
 * Otherwise free any existing pools and  allocate a new set of pools
 * before initializing them.
 *
 * Return: 0 on success and negative value on error.
 */
static int init_tx_pools(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct device *dev = &adapter->vdev->dev;
	int num_pools;
	u64 pool_size;		/* # of buffers in pool */
	u64 buff_size;
	int i, j, rc;

	num_pools = adapter->req_tx_queues;

	/* We must notify the VIOS about the LTB on all resets - but we only
	 * need to alloc/populate pools if either the number of buffers or
	 * size of each buffer in the pool has changed.
	 */
	if (reuse_tx_pools(adapter)) {
		netdev_dbg(netdev, "Reusing tx pools\n");
		goto update_ltb;
	}

	/* Allocate/populate the pools. */
	release_tx_pools(adapter);

	pool_size = adapter->req_tx_entries_per_subcrq;
	num_pools = adapter->num_active_tx_scrqs;

	adapter->tx_pool = kcalloc(num_pools,
				   sizeof(struct ibmvnic_tx_pool), GFP_KERNEL);
	if (!adapter->tx_pool)
		return -ENOMEM;

	adapter->tso_pool = kcalloc(num_pools,
				    sizeof(struct ibmvnic_tx_pool), GFP_KERNEL);
	/* To simplify release_tx_pools() ensure that ->tx_pool and
	 * ->tso_pool are either both NULL or both non-NULL.
	 */
	if (!adapter->tso_pool) {
		kfree(adapter->tx_pool);
		adapter->tx_pool = NULL;
		return -ENOMEM;
	}

	/* Set num_active_tx_pools early. If we fail below after partial
	 * allocation, release_tx_pools() will know how many to look for.
	 */
	adapter->num_active_tx_pools = num_pools;

	buff_size = adapter->req_mtu + VLAN_HLEN;
	buff_size = ALIGN(buff_size, L1_CACHE_BYTES);

	for (i = 0; i < num_pools; i++) {
		dev_dbg(dev, "Init tx pool %d [%llu, %llu]\n",
			i, adapter->req_tx_entries_per_subcrq, buff_size);

		rc = init_one_tx_pool(netdev, &adapter->tx_pool[i],
				      pool_size, buff_size);
		if (rc)
			goto out_release;

		rc = init_one_tx_pool(netdev, &adapter->tso_pool[i],
				      IBMVNIC_TSO_BUFS,
				      IBMVNIC_TSO_BUF_SZ);
		if (rc)
			goto out_release;
	}

	adapter->prev_tx_pool_size = pool_size;
	adapter->prev_mtu = adapter->req_mtu;

update_ltb:
	/* NOTE: All tx_pools have the same number of buffers (which is
	 *       same as pool_size). All tso_pools have IBMVNIC_TSO_BUFS
	 *       buffers (see calls init_one_tx_pool() for these).
	 *       For consistency, we use tx_pool->num_buffers and
	 *       tso_pool->num_buffers below.
	 */
	rc = -1;
	for (i = 0; i < num_pools; i++) {
		struct ibmvnic_tx_pool *tso_pool;
		struct ibmvnic_tx_pool *tx_pool;

		tx_pool = &adapter->tx_pool[i];

		dev_dbg(dev, "Updating LTB for tx pool %d [%d, %d]\n",
			i, tx_pool->num_buffers, tx_pool->buf_size);

		rc = alloc_ltb_set(adapter, &tx_pool->ltb_set,
				   tx_pool->num_buffers, tx_pool->buf_size);
		if (rc)
			goto out;

		tx_pool->consumer_index = 0;
		tx_pool->producer_index = 0;

		for (j = 0; j < tx_pool->num_buffers; j++)
			tx_pool->free_map[j] = j;

		tso_pool = &adapter->tso_pool[i];

		dev_dbg(dev, "Updating LTB for tso pool %d [%d, %d]\n",
			i, tso_pool->num_buffers, tso_pool->buf_size);

		rc = alloc_ltb_set(adapter, &tso_pool->ltb_set,
				   tso_pool->num_buffers, tso_pool->buf_size);
		if (rc)
			goto out;

		tso_pool->consumer_index = 0;
		tso_pool->producer_index = 0;

		for (j = 0; j < tso_pool->num_buffers; j++)
			tso_pool->free_map[j] = j;
	}

	return 0;
out_release:
	release_tx_pools(adapter);
out:
	/* We failed to allocate one or more LTBs or map them on the VIOS.
	 * Hold onto the pools and any LTBs that we did allocate/map.
	 */
	return rc;
}

static void ibmvnic_napi_enable(struct ibmvnic_adapter *adapter)
{
	int i;

	if (adapter->napi_enabled)
		return;

	for (i = 0; i < adapter->req_rx_queues; i++)
		napi_enable(&adapter->napi[i]);

	adapter->napi_enabled = true;
}

static void ibmvnic_napi_disable(struct ibmvnic_adapter *adapter)
{
	int i;

	if (!adapter->napi_enabled)
		return;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		netdev_dbg(adapter->netdev, "Disabling napi[%d]\n", i);
		napi_disable(&adapter->napi[i]);
	}

	adapter->napi_enabled = false;
}

static int init_napi(struct ibmvnic_adapter *adapter)
{
	int i;

	adapter->napi = kcalloc(adapter->req_rx_queues,
				sizeof(struct napi_struct), GFP_KERNEL);
	if (!adapter->napi)
		return -ENOMEM;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		netdev_dbg(adapter->netdev, "Adding napi[%d]\n", i);
		netif_napi_add(adapter->netdev, &adapter->napi[i],
			       ibmvnic_poll);
	}

	adapter->num_active_rx_napi = adapter->req_rx_queues;
	return 0;
}

static void release_napi(struct ibmvnic_adapter *adapter)
{
	int i;

	if (!adapter->napi)
		return;

	for (i = 0; i < adapter->num_active_rx_napi; i++) {
		netdev_dbg(adapter->netdev, "Releasing napi[%d]\n", i);
		netif_napi_del(&adapter->napi[i]);
	}

	kfree(adapter->napi);
	adapter->napi = NULL;
	adapter->num_active_rx_napi = 0;
	adapter->napi_enabled = false;
}

static const char *adapter_state_to_string(enum vnic_state state)
{
	switch (state) {
	case VNIC_PROBING:
		return "PROBING";
	case VNIC_PROBED:
		return "PROBED";
	case VNIC_OPENING:
		return "OPENING";
	case VNIC_OPEN:
		return "OPEN";
	case VNIC_CLOSING:
		return "CLOSING";
	case VNIC_CLOSED:
		return "CLOSED";
	case VNIC_REMOVING:
		return "REMOVING";
	case VNIC_REMOVED:
		return "REMOVED";
	case VNIC_DOWN:
		return "DOWN";
	}
	return "UNKNOWN";
}

static int ibmvnic_login(struct net_device *netdev)
{
	unsigned long flags, timeout = msecs_to_jiffies(20000);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int retry_count = 0;
	int retries = 10;
	bool retry;
	int rc;

	do {
		retry = false;
		if (retry_count > retries) {
			netdev_warn(netdev, "Login attempts exceeded\n");
			return -EACCES;
		}

		adapter->init_done_rc = 0;
		reinit_completion(&adapter->init_done);
		rc = send_login(adapter);
		if (rc)
			return rc;

		if (!wait_for_completion_timeout(&adapter->init_done,
						 timeout)) {
			netdev_warn(netdev, "Login timed out\n");
			adapter->login_pending = false;
			goto partial_reset;
		}

		if (adapter->init_done_rc == ABORTED) {
			netdev_warn(netdev, "Login aborted, retrying...\n");
			retry = true;
			adapter->init_done_rc = 0;
			retry_count++;
			/* FW or device may be busy, so
			 * wait a bit before retrying login
			 */
			msleep(500);
		} else if (adapter->init_done_rc == PARTIALSUCCESS) {
			retry_count++;
			release_sub_crqs(adapter, 1);

			retry = true;
			netdev_dbg(netdev,
				   "Received partial success, retrying...\n");
			adapter->init_done_rc = 0;
			reinit_completion(&adapter->init_done);
			send_query_cap(adapter);
			if (!wait_for_completion_timeout(&adapter->init_done,
							 timeout)) {
				netdev_warn(netdev,
					    "Capabilities query timed out\n");
				return -ETIMEDOUT;
			}

			rc = init_sub_crqs(adapter);
			if (rc) {
				netdev_warn(netdev,
					    "SCRQ initialization failed\n");
				return rc;
			}

			rc = init_sub_crq_irqs(adapter);
			if (rc) {
				netdev_warn(netdev,
					    "SCRQ irq initialization failed\n");
				return rc;
			}
		/* Default/timeout error handling, reset and start fresh */
		} else if (adapter->init_done_rc) {
			netdev_warn(netdev, "Adapter login failed, init_done_rc = %d\n",
				    adapter->init_done_rc);

partial_reset:
			/* adapter login failed, so free any CRQs or sub-CRQs
			 * and register again before attempting to login again.
			 * If we don't do this then the VIOS may think that
			 * we are already logged in and reject any subsequent
			 * attempts
			 */
			netdev_warn(netdev,
				    "Freeing and re-registering CRQs before attempting to login again\n");
			retry = true;
			adapter->init_done_rc = 0;
			release_sub_crqs(adapter, true);
			/* Much of this is similar logic as ibmvnic_probe(),
			 * we are essentially re-initializing communication
			 * with the server. We really should not run any
			 * resets/failovers here because this is already a form
			 * of reset and we do not want parallel resets occurring
			 */
			do {
				reinit_init_done(adapter);
				/* Clear any failovers we got in the previous
				 * pass since we are re-initializing the CRQ
				 */
				adapter->failover_pending = false;
				release_crq_queue(adapter);
				/* If we don't sleep here then we risk an
				 * unnecessary failover event from the VIOS.
				 * This is a known VIOS issue caused by a vnic
				 * device freeing and registering a CRQ too
				 * quickly.
				 */
				msleep(1500);
				/* Avoid any resets, since we are currently
				 * resetting.
				 */
				spin_lock_irqsave(&adapter->rwi_lock, flags);
				flush_reset_queue(adapter);
				spin_unlock_irqrestore(&adapter->rwi_lock,
						       flags);

				rc = init_crq_queue(adapter);
				if (rc) {
					netdev_err(netdev, "login recovery: init CRQ failed %d\n",
						   rc);
					return -EIO;
				}

				rc = ibmvnic_reset_init(adapter, false);
				if (rc)
					netdev_err(netdev, "login recovery: Reset init failed %d\n",
						   rc);
				/* IBMVNIC_CRQ_INIT will return EAGAIN if it
				 * fails, since ibmvnic_reset_init will free
				 * irq's in failure, we won't be able to receive
				 * new CRQs so we need to keep trying. probe()
				 * handles this similarly.
				 */
			} while (rc == -EAGAIN && retry_count++ < retries);
		}
	} while (retry);

	__ibmvnic_set_mac(netdev, adapter->mac_addr);

	netdev_dbg(netdev, "[S:%s] Login succeeded\n", adapter_state_to_string(adapter->state));
	return 0;
}

static void release_login_buffer(struct ibmvnic_adapter *adapter)
{
	if (!adapter->login_buf)
		return;

	dma_unmap_single(&adapter->vdev->dev, adapter->login_buf_token,
			 adapter->login_buf_sz, DMA_TO_DEVICE);
	kfree(adapter->login_buf);
	adapter->login_buf = NULL;
}

static void release_login_rsp_buffer(struct ibmvnic_adapter *adapter)
{
	if (!adapter->login_rsp_buf)
		return;

	dma_unmap_single(&adapter->vdev->dev, adapter->login_rsp_buf_token,
			 adapter->login_rsp_buf_sz, DMA_FROM_DEVICE);
	kfree(adapter->login_rsp_buf);
	adapter->login_rsp_buf = NULL;
}

static void release_resources(struct ibmvnic_adapter *adapter)
{
	release_vpd_data(adapter);

	release_napi(adapter);
	release_login_buffer(adapter);
	release_login_rsp_buffer(adapter);
}

static int set_link_state(struct ibmvnic_adapter *adapter, u8 link_state)
{
	struct net_device *netdev = adapter->netdev;
	unsigned long timeout = msecs_to_jiffies(20000);
	union ibmvnic_crq crq;
	bool resend;
	int rc;

	netdev_dbg(netdev, "setting link state %d\n", link_state);

	memset(&crq, 0, sizeof(crq));
	crq.logical_link_state.first = IBMVNIC_CRQ_CMD;
	crq.logical_link_state.cmd = LOGICAL_LINK_STATE;
	crq.logical_link_state.link_state = link_state;

	do {
		resend = false;

		reinit_completion(&adapter->init_done);
		rc = ibmvnic_send_crq(adapter, &crq);
		if (rc) {
			netdev_err(netdev, "Failed to set link state\n");
			return rc;
		}

		if (!wait_for_completion_timeout(&adapter->init_done,
						 timeout)) {
			netdev_err(netdev, "timeout setting link state\n");
			return -ETIMEDOUT;
		}

		if (adapter->init_done_rc == PARTIALSUCCESS) {
			/* Partuial success, delay and re-send */
			mdelay(1000);
			resend = true;
		} else if (adapter->init_done_rc) {
			netdev_warn(netdev, "Unable to set link state, rc=%d\n",
				    adapter->init_done_rc);
			return adapter->init_done_rc;
		}
	} while (resend);

	return 0;
}

static int set_real_num_queues(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	netdev_dbg(netdev, "Setting real tx/rx queues (%llx/%llx)\n",
		   adapter->req_tx_queues, adapter->req_rx_queues);

	rc = netif_set_real_num_tx_queues(netdev, adapter->req_tx_queues);
	if (rc) {
		netdev_err(netdev, "failed to set the number of tx queues\n");
		return rc;
	}

	rc = netif_set_real_num_rx_queues(netdev, adapter->req_rx_queues);
	if (rc)
		netdev_err(netdev, "failed to set the number of rx queues\n");

	return rc;
}

static int ibmvnic_get_vpd(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	union ibmvnic_crq crq;
	int len = 0;
	int rc;

	if (adapter->vpd->buff)
		len = adapter->vpd->len;

	mutex_lock(&adapter->fw_lock);
	adapter->fw_done_rc = 0;
	reinit_completion(&adapter->fw_done);

	crq.get_vpd_size.first = IBMVNIC_CRQ_CMD;
	crq.get_vpd_size.cmd = GET_VPD_SIZE;
	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}

	rc = ibmvnic_wait_for_completion(adapter, &adapter->fw_done, 10000);
	if (rc) {
		dev_err(dev, "Could not retrieve VPD size, rc = %d\n", rc);
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}
	mutex_unlock(&adapter->fw_lock);

	if (!adapter->vpd->len)
		return -ENODATA;

	if (!adapter->vpd->buff)
		adapter->vpd->buff = kzalloc(adapter->vpd->len, GFP_KERNEL);
	else if (adapter->vpd->len != len)
		adapter->vpd->buff =
			krealloc(adapter->vpd->buff,
				 adapter->vpd->len, GFP_KERNEL);

	if (!adapter->vpd->buff) {
		dev_err(dev, "Could allocate VPD buffer\n");
		return -ENOMEM;
	}

	adapter->vpd->dma_addr =
		dma_map_single(dev, adapter->vpd->buff, adapter->vpd->len,
			       DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, adapter->vpd->dma_addr)) {
		dev_err(dev, "Could not map VPD buffer\n");
		kfree(adapter->vpd->buff);
		adapter->vpd->buff = NULL;
		return -ENOMEM;
	}

	mutex_lock(&adapter->fw_lock);
	adapter->fw_done_rc = 0;
	reinit_completion(&adapter->fw_done);

	crq.get_vpd.first = IBMVNIC_CRQ_CMD;
	crq.get_vpd.cmd = GET_VPD;
	crq.get_vpd.ioba = cpu_to_be32(adapter->vpd->dma_addr);
	crq.get_vpd.len = cpu_to_be32((u32)adapter->vpd->len);
	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		kfree(adapter->vpd->buff);
		adapter->vpd->buff = NULL;
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}

	rc = ibmvnic_wait_for_completion(adapter, &adapter->fw_done, 10000);
	if (rc) {
		dev_err(dev, "Unable to retrieve VPD, rc = %d\n", rc);
		kfree(adapter->vpd->buff);
		adapter->vpd->buff = NULL;
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}

	mutex_unlock(&adapter->fw_lock);
	return 0;
}

static int init_resources(struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int rc;

	rc = set_real_num_queues(netdev);
	if (rc)
		return rc;

	adapter->vpd = kzalloc(sizeof(*adapter->vpd), GFP_KERNEL);
	if (!adapter->vpd)
		return -ENOMEM;

	/* Vital Product Data (VPD) */
	rc = ibmvnic_get_vpd(adapter);
	if (rc) {
		netdev_err(netdev, "failed to initialize Vital Product Data (VPD)\n");
		return rc;
	}

	rc = init_napi(adapter);
	if (rc)
		return rc;

	send_query_map(adapter);

	rc = init_rx_pools(netdev);
	if (rc)
		return rc;

	rc = init_tx_pools(netdev);
	return rc;
}

static int __ibmvnic_open(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	enum vnic_state prev_state = adapter->state;
	int i, rc;

	adapter->state = VNIC_OPENING;
	replenish_pools(adapter);
	ibmvnic_napi_enable(adapter);

	/* We're ready to receive frames, enable the sub-crq interrupts and
	 * set the logical link state to up
	 */
	for (i = 0; i < adapter->req_rx_queues; i++) {
		netdev_dbg(netdev, "Enabling rx_scrq[%d] irq\n", i);
		if (prev_state == VNIC_CLOSED)
			enable_irq(adapter->rx_scrq[i]->irq);
		enable_scrq_irq(adapter, adapter->rx_scrq[i]);
	}

	for (i = 0; i < adapter->req_tx_queues; i++) {
		netdev_dbg(netdev, "Enabling tx_scrq[%d] irq\n", i);
		if (prev_state == VNIC_CLOSED)
			enable_irq(adapter->tx_scrq[i]->irq);
		enable_scrq_irq(adapter, adapter->tx_scrq[i]);
		/* netdev_tx_reset_queue will reset dql stats. During NON_FATAL
		 * resets, don't reset the stats because there could be batched
		 * skb's waiting to be sent. If we reset dql stats, we risk
		 * num_completed being greater than num_queued. This will cause
		 * a BUG_ON in dql_completed().
		 */
		if (adapter->reset_reason != VNIC_RESET_NON_FATAL)
			netdev_tx_reset_queue(netdev_get_tx_queue(netdev, i));
	}

	rc = set_link_state(adapter, IBMVNIC_LOGICAL_LNK_UP);
	if (rc) {
		ibmvnic_napi_disable(adapter);
		ibmvnic_disable_irqs(adapter);
		return rc;
	}

	adapter->tx_queues_active = true;

	/* Since queues were stopped until now, there shouldn't be any
	 * one in ibmvnic_complete_tx() or ibmvnic_xmit() so maybe we
	 * don't need the synchronize_rcu()? Leaving it for consistency
	 * with setting ->tx_queues_active = false.
	 */
	synchronize_rcu();

	netif_tx_start_all_queues(netdev);

	if (prev_state == VNIC_CLOSED) {
		for (i = 0; i < adapter->req_rx_queues; i++)
			napi_schedule(&adapter->napi[i]);
	}

	adapter->state = VNIC_OPEN;
	return rc;
}

static int ibmvnic_open(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	ASSERT_RTNL();

	/* If device failover is pending or we are about to reset, just set
	 * device state and return. Device operation will be handled by reset
	 * routine.
	 *
	 * It should be safe to overwrite the adapter->state here. Since
	 * we hold the rtnl, either the reset has not actually started or
	 * the rtnl got dropped during the set_link_state() in do_reset().
	 * In the former case, no one else is changing the state (again we
	 * have the rtnl) and in the latter case, do_reset() will detect and
	 * honor our setting below.
	 */
	if (adapter->failover_pending || (test_bit(0, &adapter->resetting))) {
		netdev_dbg(netdev, "[S:%s FOP:%d] Resetting, deferring open\n",
			   adapter_state_to_string(adapter->state),
			   adapter->failover_pending);
		adapter->state = VNIC_OPEN;
		rc = 0;
		goto out;
	}

	if (adapter->state != VNIC_CLOSED) {
		rc = ibmvnic_login(netdev);
		if (rc)
			goto out;

		rc = init_resources(adapter);
		if (rc) {
			netdev_err(netdev, "failed to initialize resources\n");
			goto out;
		}
	}

	rc = __ibmvnic_open(netdev);

out:
	/* If open failed and there is a pending failover or in-progress reset,
	 * set device state and return. Device operation will be handled by
	 * reset routine. See also comments above regarding rtnl.
	 */
	if (rc &&
	    (adapter->failover_pending || (test_bit(0, &adapter->resetting)))) {
		adapter->state = VNIC_OPEN;
		rc = 0;
	}

	if (rc) {
		release_resources(adapter);
		release_rx_pools(adapter);
		release_tx_pools(adapter);
	}

	return rc;
}

static void clean_rx_pools(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rx_pool *rx_pool;
	struct ibmvnic_rx_buff *rx_buff;
	u64 rx_entries;
	int rx_scrqs;
	int i, j;

	if (!adapter->rx_pool)
		return;

	rx_scrqs = adapter->num_active_rx_pools;
	rx_entries = adapter->req_rx_add_entries_per_subcrq;

	/* Free any remaining skbs in the rx buffer pools */
	for (i = 0; i < rx_scrqs; i++) {
		rx_pool = &adapter->rx_pool[i];
		if (!rx_pool || !rx_pool->rx_buff)
			continue;

		netdev_dbg(adapter->netdev, "Cleaning rx_pool[%d]\n", i);
		for (j = 0; j < rx_entries; j++) {
			rx_buff = &rx_pool->rx_buff[j];
			if (rx_buff && rx_buff->skb) {
				dev_kfree_skb_any(rx_buff->skb);
				rx_buff->skb = NULL;
			}
		}
	}
}

static void clean_one_tx_pool(struct ibmvnic_adapter *adapter,
			      struct ibmvnic_tx_pool *tx_pool)
{
	struct ibmvnic_tx_buff *tx_buff;
	u64 tx_entries;
	int i;

	if (!tx_pool || !tx_pool->tx_buff)
		return;

	tx_entries = tx_pool->num_buffers;

	for (i = 0; i < tx_entries; i++) {
		tx_buff = &tx_pool->tx_buff[i];
		if (tx_buff && tx_buff->skb) {
			dev_kfree_skb_any(tx_buff->skb);
			tx_buff->skb = NULL;
		}
	}
}

static void clean_tx_pools(struct ibmvnic_adapter *adapter)
{
	int tx_scrqs;
	int i;

	if (!adapter->tx_pool || !adapter->tso_pool)
		return;

	tx_scrqs = adapter->num_active_tx_pools;

	/* Free any remaining skbs in the tx buffer pools */
	for (i = 0; i < tx_scrqs; i++) {
		netdev_dbg(adapter->netdev, "Cleaning tx_pool[%d]\n", i);
		clean_one_tx_pool(adapter, &adapter->tx_pool[i]);
		clean_one_tx_pool(adapter, &adapter->tso_pool[i]);
	}
}

static void ibmvnic_disable_irqs(struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	if (adapter->tx_scrq) {
		for (i = 0; i < adapter->req_tx_queues; i++)
			if (adapter->tx_scrq[i]->irq) {
				netdev_dbg(netdev,
					   "Disabling tx_scrq[%d] irq\n", i);
				disable_scrq_irq(adapter, adapter->tx_scrq[i]);
				disable_irq(adapter->tx_scrq[i]->irq);
			}
	}

	if (adapter->rx_scrq) {
		for (i = 0; i < adapter->req_rx_queues; i++) {
			if (adapter->rx_scrq[i]->irq) {
				netdev_dbg(netdev,
					   "Disabling rx_scrq[%d] irq\n", i);
				disable_scrq_irq(adapter, adapter->rx_scrq[i]);
				disable_irq(adapter->rx_scrq[i]->irq);
			}
		}
	}
}

static void ibmvnic_cleanup(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	/* ensure that transmissions are stopped if called by do_reset */

	adapter->tx_queues_active = false;

	/* Ensure complete_tx() and ibmvnic_xmit() see ->tx_queues_active
	 * update so they don't restart a queue after we stop it below.
	 */
	synchronize_rcu();

	if (test_bit(0, &adapter->resetting))
		netif_tx_disable(netdev);
	else
		netif_tx_stop_all_queues(netdev);

	ibmvnic_napi_disable(adapter);
	ibmvnic_disable_irqs(adapter);
}

static int __ibmvnic_close(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc = 0;

	adapter->state = VNIC_CLOSING;
	rc = set_link_state(adapter, IBMVNIC_LOGICAL_LNK_DN);
	adapter->state = VNIC_CLOSED;
	return rc;
}

static int ibmvnic_close(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	netdev_dbg(netdev, "[S:%s FOP:%d FRR:%d] Closing\n",
		   adapter_state_to_string(adapter->state),
		   adapter->failover_pending,
		   adapter->force_reset_recovery);

	/* If device failover is pending, just set device state and return.
	 * Device operation will be handled by reset routine.
	 */
	if (adapter->failover_pending) {
		adapter->state = VNIC_CLOSED;
		return 0;
	}

	rc = __ibmvnic_close(netdev);
	ibmvnic_cleanup(netdev);
	clean_rx_pools(adapter);
	clean_tx_pools(adapter);

	return rc;
}

/**
 * get_hdr_lens - fills list of L2/L3/L4 hdr lens
 * @hdr_field: bitfield determining needed headers
 * @skb: socket buffer
 * @hdr_len: array of header lengths to be filled
 *
 * Reads hdr_field to determine which headers are needed by firmware.
 * Builds a buffer containing these headers.  Saves individual header
 * lengths and total buffer length to be used to build descriptors.
 *
 * Return: total len of all headers
 */
static int get_hdr_lens(u8 hdr_field, struct sk_buff *skb,
			int *hdr_len)
{
	int len = 0;


	if ((hdr_field >> 6) & 1) {
		hdr_len[0] = skb_mac_header_len(skb);
		len += hdr_len[0];
	}

	if ((hdr_field >> 5) & 1) {
		hdr_len[1] = skb_network_header_len(skb);
		len += hdr_len[1];
	}

	if (!((hdr_field >> 4) & 1))
		return len;

	if (skb->protocol == htons(ETH_P_IP)) {
		if (ip_hdr(skb)->protocol == IPPROTO_TCP)
			hdr_len[2] = tcp_hdrlen(skb);
		else if (ip_hdr(skb)->protocol == IPPROTO_UDP)
			hdr_len[2] = sizeof(struct udphdr);
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		if (ipv6_hdr(skb)->nexthdr == IPPROTO_TCP)
			hdr_len[2] = tcp_hdrlen(skb);
		else if (ipv6_hdr(skb)->nexthdr == IPPROTO_UDP)
			hdr_len[2] = sizeof(struct udphdr);
	}

	return len + hdr_len[2];
}

/**
 * create_hdr_descs - create header and header extension descriptors
 * @hdr_field: bitfield determining needed headers
 * @hdr_data: buffer containing header data
 * @len: length of data buffer
 * @hdr_len: array of individual header lengths
 * @scrq_arr: descriptor array
 *
 * Creates header and, if needed, header extension descriptors and
 * places them in a descriptor array, scrq_arr
 *
 * Return: Number of header descs
 */

static int create_hdr_descs(u8 hdr_field, u8 *hdr_data, int len, int *hdr_len,
			    union sub_crq *scrq_arr)
{
	union sub_crq *hdr_desc;
	int tmp_len = len;
	int num_descs = 0;
	u8 *data, *cur;
	int tmp;

	while (tmp_len > 0) {
		cur = hdr_data + len - tmp_len;

		hdr_desc = &scrq_arr[num_descs];
		if (num_descs) {
			data = hdr_desc->hdr_ext.data;
			tmp = tmp_len > 29 ? 29 : tmp_len;
			hdr_desc->hdr_ext.first = IBMVNIC_CRQ_CMD;
			hdr_desc->hdr_ext.type = IBMVNIC_HDR_EXT_DESC;
			hdr_desc->hdr_ext.len = tmp;
		} else {
			data = hdr_desc->hdr.data;
			tmp = tmp_len > 24 ? 24 : tmp_len;
			hdr_desc->hdr.first = IBMVNIC_CRQ_CMD;
			hdr_desc->hdr.type = IBMVNIC_HDR_DESC;
			hdr_desc->hdr.len = tmp;
			hdr_desc->hdr.l2_len = (u8)hdr_len[0];
			hdr_desc->hdr.l3_len = cpu_to_be16((u16)hdr_len[1]);
			hdr_desc->hdr.l4_len = (u8)hdr_len[2];
			hdr_desc->hdr.flag = hdr_field << 1;
		}
		memcpy(data, cur, tmp);
		tmp_len -= tmp;
		num_descs++;
	}

	return num_descs;
}

/**
 * build_hdr_descs_arr - build a header descriptor array
 * @skb: tx socket buffer
 * @indir_arr: indirect array
 * @num_entries: number of descriptors to be sent
 * @hdr_field: bit field determining which headers will be sent
 *
 * This function will build a TX descriptor array with applicable
 * L2/L3/L4 packet header descriptors to be sent by send_subcrq_indirect.
 */

static void build_hdr_descs_arr(struct sk_buff *skb,
				union sub_crq *indir_arr,
				int *num_entries, u8 hdr_field)
{
	int hdr_len[3] = {0, 0, 0};
	int tot_len;

	tot_len = get_hdr_lens(hdr_field, skb, hdr_len);
	*num_entries += create_hdr_descs(hdr_field, skb_mac_header(skb),
					 tot_len, hdr_len, indir_arr + 1);
}

static int ibmvnic_xmit_workarounds(struct sk_buff *skb,
				    struct net_device *netdev)
{
	/* For some backing devices, mishandling of small packets
	 * can result in a loss of connection or TX stall. Device
	 * architects recommend that no packet should be smaller
	 * than the minimum MTU value provided to the driver, so
	 * pad any packets to that length
	 */
	if (skb->len < netdev->min_mtu)
		return skb_put_padto(skb, netdev->min_mtu);

	return 0;
}

static void ibmvnic_tx_scrq_clean_buffer(struct ibmvnic_adapter *adapter,
					 struct ibmvnic_sub_crq_queue *tx_scrq)
{
	struct ibmvnic_ind_xmit_queue *ind_bufp;
	struct ibmvnic_tx_buff *tx_buff;
	struct ibmvnic_tx_pool *tx_pool;
	union sub_crq tx_scrq_entry;
	int queue_num;
	int entries;
	int index;
	int i;

	ind_bufp = &tx_scrq->ind_buf;
	entries = (u64)ind_bufp->index;
	queue_num = tx_scrq->pool_index;

	for (i = entries - 1; i >= 0; --i) {
		tx_scrq_entry = ind_bufp->indir_arr[i];
		if (tx_scrq_entry.v1.type != IBMVNIC_TX_DESC)
			continue;
		index = be32_to_cpu(tx_scrq_entry.v1.correlator);
		if (index & IBMVNIC_TSO_POOL_MASK) {
			tx_pool = &adapter->tso_pool[queue_num];
			index &= ~IBMVNIC_TSO_POOL_MASK;
		} else {
			tx_pool = &adapter->tx_pool[queue_num];
		}
		tx_pool->free_map[tx_pool->consumer_index] = index;
		tx_pool->consumer_index = tx_pool->consumer_index == 0 ?
					  tx_pool->num_buffers - 1 :
					  tx_pool->consumer_index - 1;
		tx_buff = &tx_pool->tx_buff[index];
		adapter->tx_stats_buffers[queue_num].batched_packets--;
		adapter->tx_stats_buffers[queue_num].bytes -=
						tx_buff->skb->len;
		dev_kfree_skb_any(tx_buff->skb);
		tx_buff->skb = NULL;
		adapter->netdev->stats.tx_dropped++;
	}

	ind_bufp->index = 0;

	if (atomic_sub_return(entries, &tx_scrq->used) <=
	    (adapter->req_tx_entries_per_subcrq / 2) &&
	    __netif_subqueue_stopped(adapter->netdev, queue_num)) {
		rcu_read_lock();

		if (adapter->tx_queues_active) {
			netif_wake_subqueue(adapter->netdev, queue_num);
			netdev_dbg(adapter->netdev, "Started queue %d\n",
				   queue_num);
		}

		rcu_read_unlock();
	}
}

static int send_subcrq_direct(struct ibmvnic_adapter *adapter,
			      u64 remote_handle, u64 *entry)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	int rc;

	/* Make sure the hypervisor sees the complete request */
	dma_wmb();
	rc = plpar_hcall_norets(H_SEND_SUB_CRQ, ua,
				cpu_to_be64(remote_handle),
				cpu_to_be64(entry[0]), cpu_to_be64(entry[1]),
				cpu_to_be64(entry[2]), cpu_to_be64(entry[3]));

	if (rc)
		print_subcrq_error(dev, rc, __func__);

	return rc;
}

static int ibmvnic_tx_scrq_flush(struct ibmvnic_adapter *adapter,
				 struct ibmvnic_sub_crq_queue *tx_scrq,
				 bool indirect)
{
	struct ibmvnic_ind_xmit_queue *ind_bufp;
	u64 dma_addr;
	u64 entries;
	u64 handle;
	int rc;

	ind_bufp = &tx_scrq->ind_buf;
	dma_addr = (u64)ind_bufp->indir_dma;
	entries = (u64)ind_bufp->index;
	handle = tx_scrq->handle;

	if (!entries)
		return 0;

	if (indirect)
		rc = send_subcrq_indirect(adapter, handle, dma_addr, entries);
	else
		rc = send_subcrq_direct(adapter, handle,
					(u64 *)ind_bufp->indir_arr);

	if (rc)
		ibmvnic_tx_scrq_clean_buffer(adapter, tx_scrq);
	else
		ind_bufp->index = 0;
	return rc;
}

static netdev_tx_t ibmvnic_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int queue_num = skb_get_queue_mapping(skb);
	u8 *hdrs = (u8 *)&adapter->tx_rx_desc_req;
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_ind_xmit_queue *ind_bufp;
	struct ibmvnic_tx_buff *tx_buff = NULL;
	struct ibmvnic_sub_crq_queue *tx_scrq;
	struct ibmvnic_long_term_buff *ltb;
	struct ibmvnic_tx_pool *tx_pool;
	unsigned int tx_send_failed = 0;
	netdev_tx_t ret = NETDEV_TX_OK;
	unsigned int tx_map_failed = 0;
	union sub_crq indir_arr[16];
	unsigned int tx_dropped = 0;
	unsigned int tx_dpackets = 0;
	unsigned int tx_bpackets = 0;
	unsigned int tx_bytes = 0;
	dma_addr_t data_dma_addr;
	struct netdev_queue *txq;
	unsigned long lpar_rc;
	unsigned int skblen;
	union sub_crq tx_crq;
	unsigned int offset;
	bool use_scrq_send_direct = false;
	int num_entries = 1;
	unsigned char *dst;
	int bufidx = 0;
	u8 proto = 0;

	/* If a reset is in progress, drop the packet since
	 * the scrqs may get torn down. Otherwise use the
	 * rcu to ensure reset waits for us to complete.
	 */
	rcu_read_lock();
	if (!adapter->tx_queues_active) {
		dev_kfree_skb_any(skb);

		tx_send_failed++;
		tx_dropped++;
		ret = NETDEV_TX_OK;
		goto out;
	}

	tx_scrq = adapter->tx_scrq[queue_num];
	txq = netdev_get_tx_queue(netdev, queue_num);
	ind_bufp = &tx_scrq->ind_buf;

	if (ibmvnic_xmit_workarounds(skb, netdev)) {
		tx_dropped++;
		tx_send_failed++;
		ret = NETDEV_TX_OK;
		lpar_rc = ibmvnic_tx_scrq_flush(adapter, tx_scrq, true);
		if (lpar_rc != H_SUCCESS)
			goto tx_err;
		goto out;
	}

	if (skb_is_gso(skb))
		tx_pool = &adapter->tso_pool[queue_num];
	else
		tx_pool = &adapter->tx_pool[queue_num];

	bufidx = tx_pool->free_map[tx_pool->consumer_index];

	if (bufidx == IBMVNIC_INVALID_MAP) {
		dev_kfree_skb_any(skb);
		tx_send_failed++;
		tx_dropped++;
		ret = NETDEV_TX_OK;
		lpar_rc = ibmvnic_tx_scrq_flush(adapter, tx_scrq, true);
		if (lpar_rc != H_SUCCESS)
			goto tx_err;
		goto out;
	}

	tx_pool->free_map[tx_pool->consumer_index] = IBMVNIC_INVALID_MAP;

	map_txpool_buf_to_ltb(tx_pool, bufidx, &ltb, &offset);

	dst = ltb->buff + offset;
	memset(dst, 0, tx_pool->buf_size);
	data_dma_addr = ltb->addr + offset;

	/* if we are going to send_subcrq_direct this then we need to
	 * update the checksum before copying the data into ltb. Essentially
	 * these packets force disable CSO so that we can guarantee that
	 * FW does not need header info and we can send direct. Also, vnic
	 * server must be able to xmit standard packets without header data
	 */
	if (*hdrs == 0 && !skb_is_gso(skb) &&
	    !ind_bufp->index && !netdev_xmit_more()) {
		use_scrq_send_direct = true;
		if (skb->ip_summed == CHECKSUM_PARTIAL &&
		    skb_checksum_help(skb))
			use_scrq_send_direct = false;
	}

	if (skb_shinfo(skb)->nr_frags) {
		int cur, i;

		/* Copy the head */
		skb_copy_from_linear_data(skb, dst, skb_headlen(skb));
		cur = skb_headlen(skb);

		/* Copy the frags */
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

			memcpy(dst + cur, skb_frag_address(frag),
			       skb_frag_size(frag));
			cur += skb_frag_size(frag);
		}
	} else {
		skb_copy_from_linear_data(skb, dst, skb->len);
	}

	tx_pool->consumer_index =
	    (tx_pool->consumer_index + 1) % tx_pool->num_buffers;

	tx_buff = &tx_pool->tx_buff[bufidx];

	/* Sanity checks on our free map to make sure it points to an index
	 * that is not being occupied by another skb. If skb memory is
	 * not freed then we see congestion control kick in and halt tx.
	 */
	if (unlikely(tx_buff->skb)) {
		dev_warn_ratelimited(dev, "TX free map points to untracked skb (%s %d idx=%d)\n",
				     skb_is_gso(skb) ? "tso_pool" : "tx_pool",
				     queue_num, bufidx);
		dev_kfree_skb_any(tx_buff->skb);
	}

	tx_buff->skb = skb;
	tx_buff->index = bufidx;
	tx_buff->pool_index = queue_num;
	skblen = skb->len;

	memset(&tx_crq, 0, sizeof(tx_crq));
	tx_crq.v1.first = IBMVNIC_CRQ_CMD;
	tx_crq.v1.type = IBMVNIC_TX_DESC;
	tx_crq.v1.n_crq_elem = 1;
	tx_crq.v1.n_sge = 1;
	tx_crq.v1.flags1 = IBMVNIC_TX_COMP_NEEDED;

	if (skb_is_gso(skb))
		tx_crq.v1.correlator =
			cpu_to_be32(bufidx | IBMVNIC_TSO_POOL_MASK);
	else
		tx_crq.v1.correlator = cpu_to_be32(bufidx);
	tx_crq.v1.dma_reg = cpu_to_be16(ltb->map_id);
	tx_crq.v1.sge_len = cpu_to_be32(skb->len);
	tx_crq.v1.ioba = cpu_to_be64(data_dma_addr);

	if (adapter->vlan_header_insertion && skb_vlan_tag_present(skb)) {
		tx_crq.v1.flags2 |= IBMVNIC_TX_VLAN_INSERT;
		tx_crq.v1.vlan_id = cpu_to_be16(skb->vlan_tci);
	}

	if (skb->protocol == htons(ETH_P_IP)) {
		tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_IPV4;
		proto = ip_hdr(skb)->protocol;
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_IPV6;
		proto = ipv6_hdr(skb)->nexthdr;
	}

	if (proto == IPPROTO_TCP)
		tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_TCP;
	else if (proto == IPPROTO_UDP)
		tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_UDP;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		tx_crq.v1.flags1 |= IBMVNIC_TX_CHKSUM_OFFLOAD;
		hdrs += 2;
	}
	if (skb_is_gso(skb)) {
		tx_crq.v1.flags1 |= IBMVNIC_TX_LSO;
		tx_crq.v1.mss = cpu_to_be16(skb_shinfo(skb)->gso_size);
		hdrs += 2;
	} else if (use_scrq_send_direct) {
		/* See above comment, CSO disabled with direct xmit */
		tx_crq.v1.flags1 &= ~(IBMVNIC_TX_CHKSUM_OFFLOAD);
		ind_bufp->index = 1;
		tx_buff->num_entries = 1;
		netdev_tx_sent_queue(txq, skb->len);
		ind_bufp->indir_arr[0] = tx_crq;
		lpar_rc = ibmvnic_tx_scrq_flush(adapter, tx_scrq, false);
		if (lpar_rc != H_SUCCESS)
			goto tx_err;

		tx_dpackets++;
		goto early_exit;
	}

	if ((*hdrs >> 7) & 1)
		build_hdr_descs_arr(skb, indir_arr, &num_entries, *hdrs);

	tx_crq.v1.n_crq_elem = num_entries;
	tx_buff->num_entries = num_entries;
	/* flush buffer if current entry can not fit */
	if (num_entries + ind_bufp->index > IBMVNIC_MAX_IND_DESCS) {
		lpar_rc = ibmvnic_tx_scrq_flush(adapter, tx_scrq, true);
		if (lpar_rc != H_SUCCESS)
			goto tx_flush_err;
	}

	indir_arr[0] = tx_crq;
	memcpy(&ind_bufp->indir_arr[ind_bufp->index], &indir_arr[0],
	       num_entries * sizeof(struct ibmvnic_generic_scrq));

	ind_bufp->index += num_entries;
	if (__netdev_tx_sent_queue(txq, skb->len,
				   netdev_xmit_more() &&
				   ind_bufp->index < IBMVNIC_MAX_IND_DESCS)) {
		lpar_rc = ibmvnic_tx_scrq_flush(adapter, tx_scrq, true);
		if (lpar_rc != H_SUCCESS)
			goto tx_err;
	}

	tx_bpackets++;

early_exit:
	if (atomic_add_return(num_entries, &tx_scrq->used)
					>= adapter->req_tx_entries_per_subcrq) {
		netdev_dbg(netdev, "Stopping queue %d\n", queue_num);
		netif_stop_subqueue(netdev, queue_num);
	}

	tx_bytes += skblen;
	txq_trans_cond_update(txq);
	ret = NETDEV_TX_OK;
	goto out;

tx_flush_err:
	dev_kfree_skb_any(skb);
	tx_buff->skb = NULL;
	tx_pool->consumer_index = tx_pool->consumer_index == 0 ?
				  tx_pool->num_buffers - 1 :
				  tx_pool->consumer_index - 1;
	tx_dropped++;
tx_err:
	if (lpar_rc != H_CLOSED && lpar_rc != H_PARAMETER)
		dev_err_ratelimited(dev, "tx: send failed\n");

	if (lpar_rc == H_CLOSED || adapter->failover_pending) {
		/* Disable TX and report carrier off if queue is closed
		 * or pending failover.
		 * Firmware guarantees that a signal will be sent to the
		 * driver, triggering a reset or some other action.
		 */
		netif_tx_stop_all_queues(netdev);
		netif_carrier_off(netdev);
	}
out:
	rcu_read_unlock();
	adapter->tx_send_failed += tx_send_failed;
	adapter->tx_map_failed += tx_map_failed;
	adapter->tx_stats_buffers[queue_num].batched_packets += tx_bpackets;
	adapter->tx_stats_buffers[queue_num].direct_packets += tx_dpackets;
	adapter->tx_stats_buffers[queue_num].bytes += tx_bytes;
	adapter->tx_stats_buffers[queue_num].dropped_packets += tx_dropped;

	return ret;
}

static void ibmvnic_set_multi(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct netdev_hw_addr *ha;
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_capability.first = IBMVNIC_CRQ_CMD;
	crq.request_capability.cmd = REQUEST_CAPABILITY;

	if (netdev->flags & IFF_PROMISC) {
		if (!adapter->promisc_supported)
			return;
	} else {
		if (netdev->flags & IFF_ALLMULTI) {
			/* Accept all multicast */
			memset(&crq, 0, sizeof(crq));
			crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
			crq.multicast_ctrl.cmd = MULTICAST_CTRL;
			crq.multicast_ctrl.flags = IBMVNIC_ENABLE_ALL;
			ibmvnic_send_crq(adapter, &crq);
		} else if (netdev_mc_empty(netdev)) {
			/* Reject all multicast */
			memset(&crq, 0, sizeof(crq));
			crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
			crq.multicast_ctrl.cmd = MULTICAST_CTRL;
			crq.multicast_ctrl.flags = IBMVNIC_DISABLE_ALL;
			ibmvnic_send_crq(adapter, &crq);
		} else {
			/* Accept one or more multicast(s) */
			netdev_for_each_mc_addr(ha, netdev) {
				memset(&crq, 0, sizeof(crq));
				crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
				crq.multicast_ctrl.cmd = MULTICAST_CTRL;
				crq.multicast_ctrl.flags = IBMVNIC_ENABLE_MC;
				ether_addr_copy(&crq.multicast_ctrl.mac_addr[0],
						ha->addr);
				ibmvnic_send_crq(adapter, &crq);
			}
		}
	}
}

static int __ibmvnic_set_mac(struct net_device *netdev, u8 *dev_addr)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	union ibmvnic_crq crq;
	int rc;

	if (!is_valid_ether_addr(dev_addr)) {
		rc = -EADDRNOTAVAIL;
		goto err;
	}

	memset(&crq, 0, sizeof(crq));
	crq.change_mac_addr.first = IBMVNIC_CRQ_CMD;
	crq.change_mac_addr.cmd = CHANGE_MAC_ADDR;
	ether_addr_copy(&crq.change_mac_addr.mac_addr[0], dev_addr);

	mutex_lock(&adapter->fw_lock);
	adapter->fw_done_rc = 0;
	reinit_completion(&adapter->fw_done);

	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		rc = -EIO;
		mutex_unlock(&adapter->fw_lock);
		goto err;
	}

	rc = ibmvnic_wait_for_completion(adapter, &adapter->fw_done, 10000);
	/* netdev->dev_addr is changed in handle_change_mac_rsp function */
	if (rc || adapter->fw_done_rc) {
		rc = -EIO;
		mutex_unlock(&adapter->fw_lock);
		goto err;
	}
	mutex_unlock(&adapter->fw_lock);
	return 0;
err:
	ether_addr_copy(adapter->mac_addr, netdev->dev_addr);
	return rc;
}

static int ibmvnic_set_mac(struct net_device *netdev, void *p)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct sockaddr *addr = p;
	int rc;

	rc = 0;
	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	ether_addr_copy(adapter->mac_addr, addr->sa_data);
	if (adapter->state != VNIC_PROBED)
		rc = __ibmvnic_set_mac(netdev, addr->sa_data);

	return rc;
}

static const char *reset_reason_to_string(enum ibmvnic_reset_reason reason)
{
	switch (reason) {
	case VNIC_RESET_FAILOVER:
		return "FAILOVER";
	case VNIC_RESET_MOBILITY:
		return "MOBILITY";
	case VNIC_RESET_FATAL:
		return "FATAL";
	case VNIC_RESET_NON_FATAL:
		return "NON_FATAL";
	case VNIC_RESET_TIMEOUT:
		return "TIMEOUT";
	case VNIC_RESET_CHANGE_PARAM:
		return "CHANGE_PARAM";
	case VNIC_RESET_PASSIVE_INIT:
		return "PASSIVE_INIT";
	}
	return "UNKNOWN";
}

/*
 * Initialize the init_done completion and return code values. We
 * can get a transport event just after registering the CRQ and the
 * tasklet will use this to communicate the transport event. To ensure
 * we don't miss the notification/error, initialize these _before_
 * regisering the CRQ.
 */
static inline void reinit_init_done(struct ibmvnic_adapter *adapter)
{
	reinit_completion(&adapter->init_done);
	adapter->init_done_rc = 0;
}

/*
 * do_reset returns zero if we are able to keep processing reset events, or
 * non-zero if we hit a fatal error and must halt.
 */
static int do_reset(struct ibmvnic_adapter *adapter,
		    struct ibmvnic_rwi *rwi, u32 reset_state)
{
	struct net_device *netdev = adapter->netdev;
	u64 old_num_rx_queues, old_num_tx_queues;
	u64 old_num_rx_slots, old_num_tx_slots;
	int rc;

	netdev_dbg(adapter->netdev,
		   "[S:%s FOP:%d] Reset reason: %s, reset_state: %s\n",
		   adapter_state_to_string(adapter->state),
		   adapter->failover_pending,
		   reset_reason_to_string(rwi->reset_reason),
		   adapter_state_to_string(reset_state));

	adapter->reset_reason = rwi->reset_reason;
	/* requestor of VNIC_RESET_CHANGE_PARAM already has the rtnl lock */
	if (!(adapter->reset_reason == VNIC_RESET_CHANGE_PARAM))
		rtnl_lock();

	/* Now that we have the rtnl lock, clear any pending failover.
	 * This will ensure ibmvnic_open() has either completed or will
	 * block until failover is complete.
	 */
	if (rwi->reset_reason == VNIC_RESET_FAILOVER)
		adapter->failover_pending = false;

	/* read the state and check (again) after getting rtnl */
	reset_state = adapter->state;

	if (reset_state == VNIC_REMOVING || reset_state == VNIC_REMOVED) {
		rc = -EBUSY;
		goto out;
	}

	netif_carrier_off(netdev);

	old_num_rx_queues = adapter->req_rx_queues;
	old_num_tx_queues = adapter->req_tx_queues;
	old_num_rx_slots = adapter->req_rx_add_entries_per_subcrq;
	old_num_tx_slots = adapter->req_tx_entries_per_subcrq;

	ibmvnic_cleanup(netdev);

	if (reset_state == VNIC_OPEN &&
	    adapter->reset_reason != VNIC_RESET_MOBILITY &&
	    adapter->reset_reason != VNIC_RESET_FAILOVER) {
		if (adapter->reset_reason == VNIC_RESET_CHANGE_PARAM) {
			rc = __ibmvnic_close(netdev);
			if (rc)
				goto out;
		} else {
			adapter->state = VNIC_CLOSING;

			/* Release the RTNL lock before link state change and
			 * re-acquire after the link state change to allow
			 * linkwatch_event to grab the RTNL lock and run during
			 * a reset.
			 */
			rtnl_unlock();
			rc = set_link_state(adapter, IBMVNIC_LOGICAL_LNK_DN);
			rtnl_lock();
			if (rc)
				goto out;

			if (adapter->state == VNIC_OPEN) {
				/* When we dropped rtnl, ibmvnic_open() got
				 * it and noticed that we are resetting and
				 * set the adapter state to OPEN. Update our
				 * new "target" state, and resume the reset
				 * from VNIC_CLOSING state.
				 */
				netdev_dbg(netdev,
					   "Open changed state from %s, updating.\n",
					   adapter_state_to_string(reset_state));
				reset_state = VNIC_OPEN;
				adapter->state = VNIC_CLOSING;
			}

			if (adapter->state != VNIC_CLOSING) {
				/* If someone else changed the adapter state
				 * when we dropped the rtnl, fail the reset
				 */
				rc = -EAGAIN;
				goto out;
			}
			adapter->state = VNIC_CLOSED;
		}
	}

	if (adapter->reset_reason == VNIC_RESET_CHANGE_PARAM) {
		release_resources(adapter);
		release_sub_crqs(adapter, 1);
		release_crq_queue(adapter);
	}

	if (adapter->reset_reason != VNIC_RESET_NON_FATAL) {
		/* remove the closed state so when we call open it appears
		 * we are coming from the probed state.
		 */
		adapter->state = VNIC_PROBED;

		reinit_init_done(adapter);

		if (adapter->reset_reason == VNIC_RESET_CHANGE_PARAM) {
			rc = init_crq_queue(adapter);
		} else if (adapter->reset_reason == VNIC_RESET_MOBILITY) {
			rc = ibmvnic_reenable_crq_queue(adapter);
			release_sub_crqs(adapter, 1);
		} else {
			rc = ibmvnic_reset_crq(adapter);
			if (rc == H_CLOSED || rc == H_SUCCESS) {
				rc = vio_enable_interrupts(adapter->vdev);
				if (rc)
					netdev_err(adapter->netdev,
						   "Reset failed to enable interrupts. rc=%d\n",
						   rc);
			}
		}

		if (rc) {
			netdev_err(adapter->netdev,
				   "Reset couldn't initialize crq. rc=%d\n", rc);
			goto out;
		}

		rc = ibmvnic_reset_init(adapter, true);
		if (rc)
			goto out;

		/* If the adapter was in PROBE or DOWN state prior to the reset,
		 * exit here.
		 */
		if (reset_state == VNIC_PROBED || reset_state == VNIC_DOWN) {
			rc = 0;
			goto out;
		}

		rc = ibmvnic_login(netdev);
		if (rc)
			goto out;

		if (adapter->reset_reason == VNIC_RESET_CHANGE_PARAM) {
			rc = init_resources(adapter);
			if (rc)
				goto out;
		} else if (adapter->req_rx_queues != old_num_rx_queues ||
		    adapter->req_tx_queues != old_num_tx_queues ||
		    adapter->req_rx_add_entries_per_subcrq !=
		    old_num_rx_slots ||
		    adapter->req_tx_entries_per_subcrq !=
		    old_num_tx_slots ||
		    !adapter->rx_pool ||
		    !adapter->tso_pool ||
		    !adapter->tx_pool) {
			release_napi(adapter);
			release_vpd_data(adapter);

			rc = init_resources(adapter);
			if (rc)
				goto out;

		} else {
			rc = init_tx_pools(netdev);
			if (rc) {
				netdev_dbg(netdev,
					   "init tx pools failed (%d)\n",
					   rc);
				goto out;
			}

			rc = init_rx_pools(netdev);
			if (rc) {
				netdev_dbg(netdev,
					   "init rx pools failed (%d)\n",
					   rc);
				goto out;
			}
		}
		ibmvnic_disable_irqs(adapter);
	}
	adapter->state = VNIC_CLOSED;

	if (reset_state == VNIC_CLOSED) {
		rc = 0;
		goto out;
	}

	rc = __ibmvnic_open(netdev);
	if (rc) {
		rc = IBMVNIC_OPEN_FAILED;
		goto out;
	}

	/* refresh device's multicast list */
	ibmvnic_set_multi(netdev);

	if (adapter->reset_reason == VNIC_RESET_FAILOVER ||
	    adapter->reset_reason == VNIC_RESET_MOBILITY)
		__netdev_notify_peers(netdev);

	rc = 0;

out:
	/* restore the adapter state if reset failed */
	if (rc)
		adapter->state = reset_state;
	/* requestor of VNIC_RESET_CHANGE_PARAM should still hold the rtnl lock */
	if (!(adapter->reset_reason == VNIC_RESET_CHANGE_PARAM))
		rtnl_unlock();

	netdev_dbg(adapter->netdev, "[S:%s FOP:%d] Reset done, rc %d\n",
		   adapter_state_to_string(adapter->state),
		   adapter->failover_pending, rc);
	return rc;
}

static int do_hard_reset(struct ibmvnic_adapter *adapter,
			 struct ibmvnic_rwi *rwi, u32 reset_state)
{
	struct net_device *netdev = adapter->netdev;
	int rc;

	netdev_dbg(adapter->netdev, "Hard resetting driver (%s)\n",
		   reset_reason_to_string(rwi->reset_reason));

	/* read the state and check (again) after getting rtnl */
	reset_state = adapter->state;

	if (reset_state == VNIC_REMOVING || reset_state == VNIC_REMOVED) {
		rc = -EBUSY;
		goto out;
	}

	netif_carrier_off(netdev);
	adapter->reset_reason = rwi->reset_reason;

	ibmvnic_cleanup(netdev);
	release_resources(adapter);
	release_sub_crqs(adapter, 0);
	release_crq_queue(adapter);

	/* remove the closed state so when we call open it appears
	 * we are coming from the probed state.
	 */
	adapter->state = VNIC_PROBED;

	reinit_init_done(adapter);

	rc = init_crq_queue(adapter);
	if (rc) {
		netdev_err(adapter->netdev,
			   "Couldn't initialize crq. rc=%d\n", rc);
		goto out;
	}

	rc = ibmvnic_reset_init(adapter, false);
	if (rc)
		goto out;

	/* If the adapter was in PROBE or DOWN state prior to the reset,
	 * exit here.
	 */
	if (reset_state == VNIC_PROBED || reset_state == VNIC_DOWN)
		goto out;

	rc = ibmvnic_login(netdev);
	if (rc)
		goto out;

	rc = init_resources(adapter);
	if (rc)
		goto out;

	ibmvnic_disable_irqs(adapter);
	adapter->state = VNIC_CLOSED;

	if (reset_state == VNIC_CLOSED)
		goto out;

	rc = __ibmvnic_open(netdev);
	if (rc) {
		rc = IBMVNIC_OPEN_FAILED;
		goto out;
	}

	__netdev_notify_peers(netdev);
out:
	/* restore adapter state if reset failed */
	if (rc)
		adapter->state = reset_state;
	netdev_dbg(adapter->netdev, "[S:%s FOP:%d] Hard reset done, rc %d\n",
		   adapter_state_to_string(adapter->state),
		   adapter->failover_pending, rc);
	return rc;
}

static struct ibmvnic_rwi *get_next_rwi(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rwi *rwi;
	unsigned long flags;

	spin_lock_irqsave(&adapter->rwi_lock, flags);

	if (!list_empty(&adapter->rwi_list)) {
		rwi = list_first_entry(&adapter->rwi_list, struct ibmvnic_rwi,
				       list);
		list_del(&rwi->list);
	} else {
		rwi = NULL;
	}

	spin_unlock_irqrestore(&adapter->rwi_lock, flags);
	return rwi;
}

/**
 * do_passive_init - complete probing when partner device is detected.
 * @adapter: ibmvnic_adapter struct
 *
 * If the ibmvnic device does not have a partner device to communicate with at boot
 * and that partner device comes online at a later time, this function is called
 * to complete the initialization process of ibmvnic device.
 * Caller is expected to hold rtnl_lock().
 *
 * Returns non-zero if sub-CRQs are not initialized properly leaving the device
 * in the down state.
 * Returns 0 upon success and the device is in PROBED state.
 */

static int do_passive_init(struct ibmvnic_adapter *adapter)
{
	unsigned long timeout = msecs_to_jiffies(30000);
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	int rc;

	netdev_dbg(netdev, "Partner device found, probing.\n");

	adapter->state = VNIC_PROBING;
	reinit_completion(&adapter->init_done);
	adapter->init_done_rc = 0;
	adapter->crq.active = true;

	rc = send_crq_init_complete(adapter);
	if (rc)
		goto out;

	rc = send_version_xchg(adapter);
	if (rc)
		netdev_dbg(adapter->netdev, "send_version_xchg failed, rc=%d\n", rc);

	if (!wait_for_completion_timeout(&adapter->init_done, timeout)) {
		dev_err(dev, "Initialization sequence timed out\n");
		rc = -ETIMEDOUT;
		goto out;
	}

	rc = init_sub_crqs(adapter);
	if (rc) {
		dev_err(dev, "Initialization of sub crqs failed, rc=%d\n", rc);
		goto out;
	}

	rc = init_sub_crq_irqs(adapter);
	if (rc) {
		dev_err(dev, "Failed to initialize sub crq irqs\n, rc=%d", rc);
		goto init_failed;
	}

	netdev->mtu = adapter->req_mtu - ETH_HLEN;
	netdev->min_mtu = adapter->min_mtu - ETH_HLEN;
	netdev->max_mtu = adapter->max_mtu - ETH_HLEN;

	adapter->state = VNIC_PROBED;
	netdev_dbg(netdev, "Probed successfully. Waiting for signal from partner device.\n");

	return 0;

init_failed:
	release_sub_crqs(adapter, 1);
out:
	adapter->state = VNIC_DOWN;
	return rc;
}

static void __ibmvnic_reset(struct work_struct *work)
{
	struct ibmvnic_adapter *adapter;
	unsigned int timeout = 5000;
	struct ibmvnic_rwi *tmprwi;
	bool saved_state = false;
	struct ibmvnic_rwi *rwi;
	unsigned long flags;
	struct device *dev;
	bool need_reset;
	int num_fails = 0;
	u32 reset_state;
	int rc = 0;

	adapter = container_of(work, struct ibmvnic_adapter, ibmvnic_reset);
		dev = &adapter->vdev->dev;

	/* Wait for ibmvnic_probe() to complete. If probe is taking too long
	 * or if another reset is in progress, defer work for now. If probe
	 * eventually fails it will flush and terminate our work.
	 *
	 * Three possibilities here:
	 * 1. Adpater being removed  - just return
	 * 2. Timed out on probe or another reset in progress - delay the work
	 * 3. Completed probe - perform any resets in queue
	 */
	if (adapter->state == VNIC_PROBING &&
	    !wait_for_completion_timeout(&adapter->probe_done, timeout)) {
		dev_err(dev, "Reset thread timed out on probe");
		queue_delayed_work(system_long_wq,
				   &adapter->ibmvnic_delayed_reset,
				   IBMVNIC_RESET_DELAY);
		return;
	}

	/* adapter is done with probe (i.e state is never VNIC_PROBING now) */
	if (adapter->state == VNIC_REMOVING)
		return;

	/* ->rwi_list is stable now (no one else is removing entries) */

	/* ibmvnic_probe() may have purged the reset queue after we were
	 * scheduled to process a reset so there maybe no resets to process.
	 * Before setting the ->resetting bit though, we have to make sure
	 * that there is infact a reset to process. Otherwise we may race
	 * with ibmvnic_open() and end up leaving the vnic down:
	 *
	 *	__ibmvnic_reset()	    ibmvnic_open()
	 *	-----------------	    --------------
	 *
	 *  set ->resetting bit
	 *  				find ->resetting bit is set
	 *  				set ->state to IBMVNIC_OPEN (i.e
	 *  				assume reset will open device)
	 *  				return
	 *  find reset queue empty
	 *  return
	 *
	 *  	Neither performed vnic login/open and vnic stays down
	 *
	 * If we hold the lock and conditionally set the bit, either we
	 * or ibmvnic_open() will complete the open.
	 */
	need_reset = false;
	spin_lock(&adapter->rwi_lock);
	if (!list_empty(&adapter->rwi_list)) {
		if (test_and_set_bit_lock(0, &adapter->resetting)) {
			queue_delayed_work(system_long_wq,
					   &adapter->ibmvnic_delayed_reset,
					   IBMVNIC_RESET_DELAY);
		} else {
			need_reset = true;
		}
	}
	spin_unlock(&adapter->rwi_lock);

	if (!need_reset)
		return;

	rwi = get_next_rwi(adapter);
	while (rwi) {
		spin_lock_irqsave(&adapter->state_lock, flags);

		if (adapter->state == VNIC_REMOVING ||
		    adapter->state == VNIC_REMOVED) {
			spin_unlock_irqrestore(&adapter->state_lock, flags);
			kfree(rwi);
			rc = EBUSY;
			break;
		}

		if (!saved_state) {
			reset_state = adapter->state;
			saved_state = true;
		}
		spin_unlock_irqrestore(&adapter->state_lock, flags);

		if (rwi->reset_reason == VNIC_RESET_PASSIVE_INIT) {
			rtnl_lock();
			rc = do_passive_init(adapter);
			rtnl_unlock();
			if (!rc)
				netif_carrier_on(adapter->netdev);
		} else if (adapter->force_reset_recovery) {
			/* Since we are doing a hard reset now, clear the
			 * failover_pending flag so we don't ignore any
			 * future MOBILITY or other resets.
			 */
			adapter->failover_pending = false;

			/* Transport event occurred during previous reset */
			if (adapter->wait_for_reset) {
				/* Previous was CHANGE_PARAM; caller locked */
				adapter->force_reset_recovery = false;
				rc = do_hard_reset(adapter, rwi, reset_state);
			} else {
				rtnl_lock();
				adapter->force_reset_recovery = false;
				rc = do_hard_reset(adapter, rwi, reset_state);
				rtnl_unlock();
			}
			if (rc)
				num_fails++;
			else
				num_fails = 0;

			/* If auto-priority-failover is enabled we can get
			 * back to back failovers during resets, resulting
			 * in at least two failed resets (from high-priority
			 * backing device to low-priority one and then back)
			 * If resets continue to fail beyond that, give the
			 * adapter some time to settle down before retrying.
			 */
			if (num_fails >= 3) {
				netdev_dbg(adapter->netdev,
					   "[S:%s] Hard reset failed %d times, waiting 60 secs\n",
					   adapter_state_to_string(adapter->state),
					   num_fails);
				set_current_state(TASK_UNINTERRUPTIBLE);
				schedule_timeout(60 * HZ);
			}
		} else {
			rc = do_reset(adapter, rwi, reset_state);
		}
		tmprwi = rwi;
		adapter->last_reset_time = jiffies;

		if (rc)
			netdev_dbg(adapter->netdev, "Reset failed, rc=%d\n", rc);

		rwi = get_next_rwi(adapter);

		/*
		 * If there are no resets queued and the previous reset failed,
		 * the adapter would be in an undefined state. So retry the
		 * previous reset as a hard reset.
		 *
		 * Else, free the previous rwi and, if there is another reset
		 * queued, process the new reset even if previous reset failed
		 * (the previous reset could have failed because of a fail
		 * over for instance, so process the fail over).
		 */
		if (!rwi && rc)
			rwi = tmprwi;
		else
			kfree(tmprwi);

		if (rwi && (rwi->reset_reason == VNIC_RESET_FAILOVER ||
			    rwi->reset_reason == VNIC_RESET_MOBILITY || rc))
			adapter->force_reset_recovery = true;
	}

	if (adapter->wait_for_reset) {
		adapter->reset_done_rc = rc;
		complete(&adapter->reset_done);
	}

	clear_bit_unlock(0, &adapter->resetting);

	netdev_dbg(adapter->netdev,
		   "[S:%s FRR:%d WFR:%d] Done processing resets\n",
		   adapter_state_to_string(adapter->state),
		   adapter->force_reset_recovery,
		   adapter->wait_for_reset);
}

static void __ibmvnic_delayed_reset(struct work_struct *work)
{
	struct ibmvnic_adapter *adapter;

	adapter = container_of(work, struct ibmvnic_adapter,
			       ibmvnic_delayed_reset.work);
	__ibmvnic_reset(&adapter->ibmvnic_reset);
}

static void flush_reset_queue(struct ibmvnic_adapter *adapter)
{
	struct list_head *entry, *tmp_entry;

	if (!list_empty(&adapter->rwi_list)) {
		list_for_each_safe(entry, tmp_entry, &adapter->rwi_list) {
			list_del(entry);
			kfree(list_entry(entry, struct ibmvnic_rwi, list));
		}
	}
}

static int ibmvnic_reset(struct ibmvnic_adapter *adapter,
			 enum ibmvnic_reset_reason reason)
{
	struct net_device *netdev = adapter->netdev;
	struct ibmvnic_rwi *rwi, *tmp;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&adapter->rwi_lock, flags);

	/* If failover is pending don't schedule any other reset.
	 * Instead let the failover complete. If there is already a
	 * a failover reset scheduled, we will detect and drop the
	 * duplicate reset when walking the ->rwi_list below.
	 */
	if (adapter->state == VNIC_REMOVING ||
	    adapter->state == VNIC_REMOVED ||
	    (adapter->failover_pending && reason != VNIC_RESET_FAILOVER)) {
		ret = EBUSY;
		netdev_dbg(netdev, "Adapter removing or pending failover, skipping reset\n");
		goto err;
	}

	list_for_each_entry(tmp, &adapter->rwi_list, list) {
		if (tmp->reset_reason == reason) {
			netdev_dbg(netdev, "Skipping matching reset, reason=%s\n",
				   reset_reason_to_string(reason));
			ret = EBUSY;
			goto err;
		}
	}

	rwi = kzalloc(sizeof(*rwi), GFP_ATOMIC);
	if (!rwi) {
		ret = ENOMEM;
		goto err;
	}
	/* if we just received a transport event,
	 * flush reset queue and process this reset
	 */
	if (adapter->force_reset_recovery)
		flush_reset_queue(adapter);

	rwi->reset_reason = reason;
	list_add_tail(&rwi->list, &adapter->rwi_list);
	netdev_dbg(adapter->netdev, "Scheduling reset (reason %s)\n",
		   reset_reason_to_string(reason));
	queue_work(system_long_wq, &adapter->ibmvnic_reset);

	ret = 0;
err:
	/* ibmvnic_close() below can block, so drop the lock first */
	spin_unlock_irqrestore(&adapter->rwi_lock, flags);

	if (ret == ENOMEM)
		ibmvnic_close(netdev);

	return -ret;
}

static void ibmvnic_get_stats64(struct net_device *netdev,
				struct rtnl_link_stats64 *stats)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int i;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		stats->rx_packets += adapter->rx_stats_buffers[i].packets;
		stats->rx_bytes   += adapter->rx_stats_buffers[i].bytes;
	}

	for (i = 0; i < adapter->req_tx_queues; i++) {
		stats->tx_packets += adapter->tx_stats_buffers[i].batched_packets;
		stats->tx_packets += adapter->tx_stats_buffers[i].direct_packets;
		stats->tx_bytes   += adapter->tx_stats_buffers[i].bytes;
		stats->tx_dropped += adapter->tx_stats_buffers[i].dropped_packets;
	}
}

static void ibmvnic_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);

	if (test_bit(0, &adapter->resetting)) {
		netdev_err(adapter->netdev,
			   "Adapter is resetting, skip timeout reset\n");
		return;
	}
	/* No queuing up reset until at least 5 seconds (default watchdog val)
	 * after last reset
	 */
	if (time_before(jiffies, (adapter->last_reset_time + dev->watchdog_timeo))) {
		netdev_dbg(dev, "Not yet time to tx timeout.\n");
		return;
	}
	ibmvnic_reset(adapter, VNIC_RESET_TIMEOUT);
}

static void remove_buff_from_pool(struct ibmvnic_adapter *adapter,
				  struct ibmvnic_rx_buff *rx_buff)
{
	struct ibmvnic_rx_pool *pool = &adapter->rx_pool[rx_buff->pool_index];

	rx_buff->skb = NULL;

	pool->free_map[pool->next_alloc] = (int)(rx_buff - pool->rx_buff);
	pool->next_alloc = (pool->next_alloc + 1) % pool->size;

	atomic_dec(&pool->available);
}

static int ibmvnic_poll(struct napi_struct *napi, int budget)
{
	struct ibmvnic_sub_crq_queue *rx_scrq;
	struct ibmvnic_adapter *adapter;
	struct net_device *netdev;
	int frames_processed;
	int scrq_num;

	netdev = napi->dev;
	adapter = netdev_priv(netdev);
	scrq_num = (int)(napi - adapter->napi);
	frames_processed = 0;
	rx_scrq = adapter->rx_scrq[scrq_num];

restart_poll:
	while (frames_processed < budget) {
		struct sk_buff *skb;
		struct ibmvnic_rx_buff *rx_buff;
		union sub_crq *next;
		u32 length;
		u16 offset;
		u8 flags = 0;

		if (unlikely(test_bit(0, &adapter->resetting) &&
			     adapter->reset_reason != VNIC_RESET_NON_FATAL)) {
			enable_scrq_irq(adapter, rx_scrq);
			napi_complete_done(napi, frames_processed);
			return frames_processed;
		}

		if (!pending_scrq(adapter, rx_scrq))
			break;
		next = ibmvnic_next_scrq(adapter, rx_scrq);
		rx_buff = (struct ibmvnic_rx_buff *)
			  be64_to_cpu(next->rx_comp.correlator);
		/* do error checking */
		if (next->rx_comp.rc) {
			netdev_dbg(netdev, "rx buffer returned with rc %x\n",
				   be16_to_cpu(next->rx_comp.rc));
			/* free the entry */
			next->rx_comp.first = 0;
			dev_kfree_skb_any(rx_buff->skb);
			remove_buff_from_pool(adapter, rx_buff);
			continue;
		} else if (!rx_buff->skb) {
			/* free the entry */
			next->rx_comp.first = 0;
			remove_buff_from_pool(adapter, rx_buff);
			continue;
		}

		length = be32_to_cpu(next->rx_comp.len);
		offset = be16_to_cpu(next->rx_comp.off_frame_data);
		flags = next->rx_comp.flags;
		skb = rx_buff->skb;
		/* load long_term_buff before copying to skb */
		dma_rmb();
		skb_copy_to_linear_data(skb, rx_buff->data + offset,
					length);

		/* VLAN Header has been stripped by the system firmware and
		 * needs to be inserted by the driver
		 */
		if (adapter->rx_vlan_header_insertion &&
		    (flags & IBMVNIC_VLAN_STRIPPED))
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       ntohs(next->rx_comp.vlan_tci));

		/* free the entry */
		next->rx_comp.first = 0;
		remove_buff_from_pool(adapter, rx_buff);

		skb_put(skb, length);
		skb->protocol = eth_type_trans(skb, netdev);
		skb_record_rx_queue(skb, scrq_num);

		if (flags & IBMVNIC_IP_CHKSUM_GOOD &&
		    flags & IBMVNIC_TCP_UDP_CHKSUM_GOOD) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		length = skb->len;
		napi_gro_receive(napi, skb); /* send it up */
		adapter->rx_stats_buffers[scrq_num].packets++;
		adapter->rx_stats_buffers[scrq_num].bytes += length;
		frames_processed++;
	}

	if (adapter->state != VNIC_CLOSING &&
	    (atomic_read(&adapter->rx_pool[scrq_num].available) <
	      adapter->req_rx_add_entries_per_subcrq / 2))
		replenish_rx_pool(adapter, &adapter->rx_pool[scrq_num]);
	if (frames_processed < budget) {
		if (napi_complete_done(napi, frames_processed)) {
			enable_scrq_irq(adapter, rx_scrq);
			if (pending_scrq(adapter, rx_scrq)) {
				if (napi_schedule(napi)) {
					disable_scrq_irq(adapter, rx_scrq);
					goto restart_poll;
				}
			}
		}
	}
	return frames_processed;
}

static int wait_for_reset(struct ibmvnic_adapter *adapter)
{
	int rc, ret;

	adapter->fallback.mtu = adapter->req_mtu;
	adapter->fallback.rx_queues = adapter->req_rx_queues;
	adapter->fallback.tx_queues = adapter->req_tx_queues;
	adapter->fallback.rx_entries = adapter->req_rx_add_entries_per_subcrq;
	adapter->fallback.tx_entries = adapter->req_tx_entries_per_subcrq;

	reinit_completion(&adapter->reset_done);
	adapter->wait_for_reset = true;
	rc = ibmvnic_reset(adapter, VNIC_RESET_CHANGE_PARAM);

	if (rc) {
		ret = rc;
		goto out;
	}
	rc = ibmvnic_wait_for_completion(adapter, &adapter->reset_done, 60000);
	if (rc) {
		ret = -ENODEV;
		goto out;
	}

	ret = 0;
	if (adapter->reset_done_rc) {
		ret = -EIO;
		adapter->desired.mtu = adapter->fallback.mtu;
		adapter->desired.rx_queues = adapter->fallback.rx_queues;
		adapter->desired.tx_queues = adapter->fallback.tx_queues;
		adapter->desired.rx_entries = adapter->fallback.rx_entries;
		adapter->desired.tx_entries = adapter->fallback.tx_entries;

		reinit_completion(&adapter->reset_done);
		adapter->wait_for_reset = true;
		rc = ibmvnic_reset(adapter, VNIC_RESET_CHANGE_PARAM);
		if (rc) {
			ret = rc;
			goto out;
		}
		rc = ibmvnic_wait_for_completion(adapter, &adapter->reset_done,
						 60000);
		if (rc) {
			ret = -ENODEV;
			goto out;
		}
	}
out:
	adapter->wait_for_reset = false;

	return ret;
}

static int ibmvnic_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	adapter->desired.mtu = new_mtu + ETH_HLEN;

	return wait_for_reset(adapter);
}

static netdev_features_t ibmvnic_features_check(struct sk_buff *skb,
						struct net_device *dev,
						netdev_features_t features)
{
	/* Some backing hardware adapters can not
	 * handle packets with a MSS less than 224
	 * or with only one segment.
	 */
	if (skb_is_gso(skb)) {
		if (skb_shinfo(skb)->gso_size < 224 ||
		    skb_shinfo(skb)->gso_segs == 1)
			features &= ~NETIF_F_GSO_MASK;
	}

	return features;
}

static const struct net_device_ops ibmvnic_netdev_ops = {
	.ndo_open		= ibmvnic_open,
	.ndo_stop		= ibmvnic_close,
	.ndo_start_xmit		= ibmvnic_xmit,
	.ndo_set_rx_mode	= ibmvnic_set_multi,
	.ndo_set_mac_address	= ibmvnic_set_mac,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_stats64	= ibmvnic_get_stats64,
	.ndo_tx_timeout		= ibmvnic_tx_timeout,
	.ndo_change_mtu		= ibmvnic_change_mtu,
	.ndo_features_check     = ibmvnic_features_check,
};

/* ethtool functions */

static int ibmvnic_get_link_ksettings(struct net_device *netdev,
				      struct ethtool_link_ksettings *cmd)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	rc = send_query_phys_parms(adapter);
	if (rc) {
		adapter->speed = SPEED_UNKNOWN;
		adapter->duplex = DUPLEX_UNKNOWN;
	}
	cmd->base.speed = adapter->speed;
	cmd->base.duplex = adapter->duplex;
	cmd->base.port = PORT_FIBRE;
	cmd->base.phy_address = 0;
	cmd->base.autoneg = AUTONEG_ENABLE;

	return 0;
}

static void ibmvnic_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *info)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	strscpy(info->driver, ibmvnic_driver_name, sizeof(info->driver));
	strscpy(info->version, IBMVNIC_DRIVER_VERSION, sizeof(info->version));
	strscpy(info->fw_version, adapter->fw_version,
		sizeof(info->fw_version));
}

static u32 ibmvnic_get_msglevel(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	return adapter->msg_enable;
}

static void ibmvnic_set_msglevel(struct net_device *netdev, u32 data)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	adapter->msg_enable = data;
}

static u32 ibmvnic_get_link(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	/* Don't need to send a query because we request a logical link up at
	 * init and then we wait for link state indications
	 */
	return adapter->logical_link_state;
}

static void ibmvnic_get_ringparam(struct net_device *netdev,
				  struct ethtool_ringparam *ring,
				  struct kernel_ethtool_ringparam *kernel_ring,
				  struct netlink_ext_ack *extack)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	ring->rx_max_pending = adapter->max_rx_add_entries_per_subcrq;
	ring->tx_max_pending = adapter->max_tx_entries_per_subcrq;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = adapter->req_rx_add_entries_per_subcrq;
	ring->tx_pending = adapter->req_tx_entries_per_subcrq;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int ibmvnic_set_ringparam(struct net_device *netdev,
				 struct ethtool_ringparam *ring,
				 struct kernel_ethtool_ringparam *kernel_ring,
				 struct netlink_ext_ack *extack)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	if (ring->rx_pending > adapter->max_rx_add_entries_per_subcrq  ||
	    ring->tx_pending > adapter->max_tx_entries_per_subcrq) {
		netdev_err(netdev, "Invalid request.\n");
		netdev_err(netdev, "Max tx buffers = %llu\n",
			   adapter->max_rx_add_entries_per_subcrq);
		netdev_err(netdev, "Max rx buffers = %llu\n",
			   adapter->max_tx_entries_per_subcrq);
		return -EINVAL;
	}

	adapter->desired.rx_entries = ring->rx_pending;
	adapter->desired.tx_entries = ring->tx_pending;

	return wait_for_reset(adapter);
}

static void ibmvnic_get_channels(struct net_device *netdev,
				 struct ethtool_channels *channels)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	channels->max_rx = adapter->max_rx_queues;
	channels->max_tx = adapter->max_tx_queues;
	channels->max_other = 0;
	channels->max_combined = 0;
	channels->rx_count = adapter->req_rx_queues;
	channels->tx_count = adapter->req_tx_queues;
	channels->other_count = 0;
	channels->combined_count = 0;
}

static int ibmvnic_set_channels(struct net_device *netdev,
				struct ethtool_channels *channels)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	adapter->desired.rx_queues = channels->rx_count;
	adapter->desired.tx_queues = channels->tx_count;

	return wait_for_reset(adapter);
}

static void ibmvnic_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmvnic_stats); i++)
		ethtool_puts(&data, ibmvnic_stats[i].name);

	for (i = 0; i < adapter->req_tx_queues; i++) {
		ethtool_sprintf(&data, "tx%d_batched_packets", i);
		ethtool_sprintf(&data, "tx%d_direct_packets", i);
		ethtool_sprintf(&data, "tx%d_bytes", i);
		ethtool_sprintf(&data, "tx%d_dropped_packets", i);
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		ethtool_sprintf(&data, "rx%d_packets", i);
		ethtool_sprintf(&data, "rx%d_bytes", i);
		ethtool_sprintf(&data, "rx%d_interrupts", i);
	}
}

static int ibmvnic_get_sset_count(struct net_device *dev, int sset)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ibmvnic_stats) +
		       adapter->req_tx_queues * NUM_TX_STATS +
		       adapter->req_rx_queues * NUM_RX_STATS;
	default:
		return -EOPNOTSUPP;
	}
}

static void ibmvnic_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);
	union ibmvnic_crq crq;
	int i, j;
	int rc;

	memset(&crq, 0, sizeof(crq));
	crq.request_statistics.first = IBMVNIC_CRQ_CMD;
	crq.request_statistics.cmd = REQUEST_STATISTICS;
	crq.request_statistics.ioba = cpu_to_be32(adapter->stats_token);
	crq.request_statistics.len =
	    cpu_to_be32(sizeof(struct ibmvnic_statistics));

	/* Wait for data to be written */
	reinit_completion(&adapter->stats_done);
	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc)
		return;
	rc = ibmvnic_wait_for_completion(adapter, &adapter->stats_done, 10000);
	if (rc)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmvnic_stats); i++)
		data[i] = be64_to_cpu(IBMVNIC_GET_STAT
				      (adapter, ibmvnic_stats[i].offset));

	for (j = 0; j < adapter->req_tx_queues; j++) {
		data[i] = adapter->tx_stats_buffers[j].batched_packets;
		i++;
		data[i] = adapter->tx_stats_buffers[j].direct_packets;
		i++;
		data[i] = adapter->tx_stats_buffers[j].bytes;
		i++;
		data[i] = adapter->tx_stats_buffers[j].dropped_packets;
		i++;
	}

	for (j = 0; j < adapter->req_rx_queues; j++) {
		data[i] = adapter->rx_stats_buffers[j].packets;
		i++;
		data[i] = adapter->rx_stats_buffers[j].bytes;
		i++;
		data[i] = adapter->rx_stats_buffers[j].interrupts;
		i++;
	}
}

static const struct ethtool_ops ibmvnic_ethtool_ops = {
	.get_drvinfo		= ibmvnic_get_drvinfo,
	.get_msglevel		= ibmvnic_get_msglevel,
	.set_msglevel		= ibmvnic_set_msglevel,
	.get_link		= ibmvnic_get_link,
	.get_ringparam		= ibmvnic_get_ringparam,
	.set_ringparam		= ibmvnic_set_ringparam,
	.get_channels		= ibmvnic_get_channels,
	.set_channels		= ibmvnic_set_channels,
	.get_strings            = ibmvnic_get_strings,
	.get_sset_count         = ibmvnic_get_sset_count,
	.get_ethtool_stats	= ibmvnic_get_ethtool_stats,
	.get_link_ksettings	= ibmvnic_get_link_ksettings,
};

/* Routines for managing CRQs/sCRQs  */

static int reset_one_sub_crq_queue(struct ibmvnic_adapter *adapter,
				   struct ibmvnic_sub_crq_queue *scrq)
{
	int rc;

	if (!scrq) {
		netdev_dbg(adapter->netdev, "Invalid scrq reset.\n");
		return -EINVAL;
	}

	if (scrq->irq) {
		free_irq(scrq->irq, scrq);
		irq_dispose_mapping(scrq->irq);
		scrq->irq = 0;
	}

	if (scrq->msgs) {
		memset(scrq->msgs, 0, 4 * PAGE_SIZE);
		atomic_set(&scrq->used, 0);
		scrq->cur = 0;
		scrq->ind_buf.index = 0;
	} else {
		netdev_dbg(adapter->netdev, "Invalid scrq reset\n");
		return -EINVAL;
	}

	rc = h_reg_sub_crq(adapter->vdev->unit_address, scrq->msg_token,
			   4 * PAGE_SIZE, &scrq->crq_num, &scrq->hw_irq);
	return rc;
}

static int reset_sub_crq_queues(struct ibmvnic_adapter *adapter)
{
	int i, rc;

	if (!adapter->tx_scrq || !adapter->rx_scrq)
		return -EINVAL;

	ibmvnic_clean_affinity(adapter);

	for (i = 0; i < adapter->req_tx_queues; i++) {
		netdev_dbg(adapter->netdev, "Re-setting tx_scrq[%d]\n", i);
		rc = reset_one_sub_crq_queue(adapter, adapter->tx_scrq[i]);
		if (rc)
			return rc;
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		netdev_dbg(adapter->netdev, "Re-setting rx_scrq[%d]\n", i);
		rc = reset_one_sub_crq_queue(adapter, adapter->rx_scrq[i]);
		if (rc)
			return rc;
	}

	return rc;
}

static void release_sub_crq_queue(struct ibmvnic_adapter *adapter,
				  struct ibmvnic_sub_crq_queue *scrq,
				  bool do_h_free)
{
	struct device *dev = &adapter->vdev->dev;
	long rc;

	netdev_dbg(adapter->netdev, "Releasing sub-CRQ\n");

	if (do_h_free) {
		/* Close the sub-crqs */
		do {
			rc = plpar_hcall_norets(H_FREE_SUB_CRQ,
						adapter->vdev->unit_address,
						scrq->crq_num);
		} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

		if (rc) {
			netdev_err(adapter->netdev,
				   "Failed to release sub-CRQ %16lx, rc = %ld\n",
				   scrq->crq_num, rc);
		}
	}

	dma_free_coherent(dev,
			  IBMVNIC_IND_ARR_SZ,
			  scrq->ind_buf.indir_arr,
			  scrq->ind_buf.indir_dma);

	dma_unmap_single(dev, scrq->msg_token, 4 * PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
	free_pages((unsigned long)scrq->msgs, 2);
	free_cpumask_var(scrq->affinity_mask);
	kfree(scrq);
}

static struct ibmvnic_sub_crq_queue *init_sub_crq_queue(struct ibmvnic_adapter
							*adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue *scrq;
	int rc;

	scrq = kzalloc(sizeof(*scrq), GFP_KERNEL);
	if (!scrq)
		return NULL;

	scrq->msgs =
		(union sub_crq *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 2);
	if (!scrq->msgs) {
		dev_warn(dev, "Couldn't allocate crq queue messages page\n");
		goto zero_page_failed;
	}
	if (!zalloc_cpumask_var(&scrq->affinity_mask, GFP_KERNEL))
		goto cpumask_alloc_failed;

	scrq->msg_token = dma_map_single(dev, scrq->msgs, 4 * PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, scrq->msg_token)) {
		dev_warn(dev, "Couldn't map crq queue messages page\n");
		goto map_failed;
	}

	rc = h_reg_sub_crq(adapter->vdev->unit_address, scrq->msg_token,
			   4 * PAGE_SIZE, &scrq->crq_num, &scrq->hw_irq);

	if (rc == H_RESOURCE)
		rc = ibmvnic_reset_crq(adapter);

	if (rc == H_CLOSED) {
		dev_warn(dev, "Partner adapter not ready, waiting.\n");
	} else if (rc) {
		dev_warn(dev, "Error %d registering sub-crq\n", rc);
		goto reg_failed;
	}

	scrq->adapter = adapter;
	scrq->size = 4 * PAGE_SIZE / sizeof(*scrq->msgs);
	scrq->ind_buf.index = 0;

	scrq->ind_buf.indir_arr =
		dma_alloc_coherent(dev,
				   IBMVNIC_IND_ARR_SZ,
				   &scrq->ind_buf.indir_dma,
				   GFP_KERNEL);

	if (!scrq->ind_buf.indir_arr)
		goto indir_failed;

	spin_lock_init(&scrq->lock);

	netdev_dbg(adapter->netdev,
		   "sub-crq initialized, num %lx, hw_irq=%lx, irq=%x\n",
		   scrq->crq_num, scrq->hw_irq, scrq->irq);

	return scrq;

indir_failed:
	do {
		rc = plpar_hcall_norets(H_FREE_SUB_CRQ,
					adapter->vdev->unit_address,
					scrq->crq_num);
	} while (rc == H_BUSY || rc == H_IS_LONG_BUSY(rc));
reg_failed:
	dma_unmap_single(dev, scrq->msg_token, 4 * PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
map_failed:
	free_cpumask_var(scrq->affinity_mask);
cpumask_alloc_failed:
	free_pages((unsigned long)scrq->msgs, 2);
zero_page_failed:
	kfree(scrq);

	return NULL;
}

static void release_sub_crqs(struct ibmvnic_adapter *adapter, bool do_h_free)
{
	int i;

	ibmvnic_clean_affinity(adapter);
	if (adapter->tx_scrq) {
		for (i = 0; i < adapter->num_active_tx_scrqs; i++) {
			if (!adapter->tx_scrq[i])
				continue;

			netdev_dbg(adapter->netdev, "Releasing tx_scrq[%d]\n",
				   i);
			ibmvnic_tx_scrq_clean_buffer(adapter, adapter->tx_scrq[i]);
			if (adapter->tx_scrq[i]->irq) {
				free_irq(adapter->tx_scrq[i]->irq,
					 adapter->tx_scrq[i]);
				irq_dispose_mapping(adapter->tx_scrq[i]->irq);
				adapter->tx_scrq[i]->irq = 0;
			}

			release_sub_crq_queue(adapter, adapter->tx_scrq[i],
					      do_h_free);
		}

		kfree(adapter->tx_scrq);
		adapter->tx_scrq = NULL;
		adapter->num_active_tx_scrqs = 0;
	}

	/* Clean any remaining outstanding SKBs
	 * we freed the irq so we won't be hearing
	 * from them
	 */
	clean_tx_pools(adapter);

	if (adapter->rx_scrq) {
		for (i = 0; i < adapter->num_active_rx_scrqs; i++) {
			if (!adapter->rx_scrq[i])
				continue;

			netdev_dbg(adapter->netdev, "Releasing rx_scrq[%d]\n",
				   i);
			if (adapter->rx_scrq[i]->irq) {
				free_irq(adapter->rx_scrq[i]->irq,
					 adapter->rx_scrq[i]);
				irq_dispose_mapping(adapter->rx_scrq[i]->irq);
				adapter->rx_scrq[i]->irq = 0;
			}

			release_sub_crq_queue(adapter, adapter->rx_scrq[i],
					      do_h_free);
		}

		kfree(adapter->rx_scrq);
		adapter->rx_scrq = NULL;
		adapter->num_active_rx_scrqs = 0;
	}
}

static int disable_scrq_irq(struct ibmvnic_adapter *adapter,
			    struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long rc;

	rc = plpar_hcall_norets(H_VIOCTL, adapter->vdev->unit_address,
				H_DISABLE_VIO_INTERRUPT, scrq->hw_irq, 0, 0);
	if (rc)
		dev_err(dev, "Couldn't disable scrq irq 0x%lx. rc=%ld\n",
			scrq->hw_irq, rc);
	return rc;
}

/* We can not use the IRQ chip EOI handler because that has the
 * unintended effect of changing the interrupt priority.
 */
static void ibmvnic_xics_eoi(struct device *dev, struct ibmvnic_sub_crq_queue *scrq)
{
	u64 val = 0xff000000 | scrq->hw_irq;
	unsigned long rc;

	rc = plpar_hcall_norets(H_EOI, val);
	if (rc)
		dev_err(dev, "H_EOI FAILED irq 0x%llx. rc=%ld\n", val, rc);
}

/* Due to a firmware bug, the hypervisor can send an interrupt to a
 * transmit or receive queue just prior to a partition migration.
 * Force an EOI after migration.
 */
static void ibmvnic_clear_pending_interrupt(struct device *dev,
					    struct ibmvnic_sub_crq_queue *scrq)
{
	if (!xive_enabled())
		ibmvnic_xics_eoi(dev, scrq);
}

static int enable_scrq_irq(struct ibmvnic_adapter *adapter,
			   struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long rc;

	if (scrq->hw_irq > 0x100000000ULL) {
		dev_err(dev, "bad hw_irq = %lx\n", scrq->hw_irq);
		return 1;
	}

	if (test_bit(0, &adapter->resetting) &&
	    adapter->reset_reason == VNIC_RESET_MOBILITY) {
		ibmvnic_clear_pending_interrupt(dev, scrq);
	}

	rc = plpar_hcall_norets(H_VIOCTL, adapter->vdev->unit_address,
				H_ENABLE_VIO_INTERRUPT, scrq->hw_irq, 0, 0);
	if (rc)
		dev_err(dev, "Couldn't enable scrq irq 0x%lx. rc=%ld\n",
			scrq->hw_irq, rc);
	return rc;
}

static int ibmvnic_complete_tx(struct ibmvnic_adapter *adapter,
			       struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	int num_packets = 0, total_bytes = 0;
	struct ibmvnic_tx_pool *tx_pool;
	struct ibmvnic_tx_buff *txbuff;
	struct netdev_queue *txq;
	union sub_crq *next;
	int index, i;

restart_loop:
	while (pending_scrq(adapter, scrq)) {
		unsigned int pool = scrq->pool_index;
		int num_entries = 0;
		next = ibmvnic_next_scrq(adapter, scrq);
		for (i = 0; i < next->tx_comp.num_comps; i++) {
			index = be32_to_cpu(next->tx_comp.correlators[i]);
			if (index & IBMVNIC_TSO_POOL_MASK) {
				tx_pool = &adapter->tso_pool[pool];
				index &= ~IBMVNIC_TSO_POOL_MASK;
			} else {
				tx_pool = &adapter->tx_pool[pool];
			}

			txbuff = &tx_pool->tx_buff[index];
			num_packets++;
			num_entries += txbuff->num_entries;
			if (txbuff->skb) {
				total_bytes += txbuff->skb->len;
				if (next->tx_comp.rcs[i]) {
					dev_err(dev, "tx error %x\n",
						next->tx_comp.rcs[i]);
					dev_kfree_skb_irq(txbuff->skb);
				} else {
					dev_consume_skb_irq(txbuff->skb);
				}
				txbuff->skb = NULL;
			} else {
				netdev_warn(adapter->netdev,
					    "TX completion received with NULL socket buffer\n");
			}
			tx_pool->free_map[tx_pool->producer_index] = index;
			tx_pool->producer_index =
				(tx_pool->producer_index + 1) %
					tx_pool->num_buffers;
		}
		/* remove tx_comp scrq*/
		next->tx_comp.first = 0;


		if (atomic_sub_return(num_entries, &scrq->used) <=
		    (adapter->req_tx_entries_per_subcrq / 2) &&
		    __netif_subqueue_stopped(adapter->netdev,
					     scrq->pool_index)) {
			rcu_read_lock();
			if (adapter->tx_queues_active) {
				netif_wake_subqueue(adapter->netdev,
						    scrq->pool_index);
				netdev_dbg(adapter->netdev,
					   "Started queue %d\n",
					   scrq->pool_index);
			}
			rcu_read_unlock();
		}
	}

	enable_scrq_irq(adapter, scrq);

	if (pending_scrq(adapter, scrq)) {
		disable_scrq_irq(adapter, scrq);
		goto restart_loop;
	}

	txq = netdev_get_tx_queue(adapter->netdev, scrq->pool_index);
	netdev_tx_completed_queue(txq, num_packets, total_bytes);

	return 0;
}

static irqreturn_t ibmvnic_interrupt_tx(int irq, void *instance)
{
	struct ibmvnic_sub_crq_queue *scrq = instance;
	struct ibmvnic_adapter *adapter = scrq->adapter;

	disable_scrq_irq(adapter, scrq);
	ibmvnic_complete_tx(adapter, scrq);

	return IRQ_HANDLED;
}

static irqreturn_t ibmvnic_interrupt_rx(int irq, void *instance)
{
	struct ibmvnic_sub_crq_queue *scrq = instance;
	struct ibmvnic_adapter *adapter = scrq->adapter;

	/* When booting a kdump kernel we can hit pending interrupts
	 * prior to completing driver initialization.
	 */
	if (unlikely(adapter->state != VNIC_OPEN))
		return IRQ_NONE;

	adapter->rx_stats_buffers[scrq->scrq_num].interrupts++;

	if (napi_schedule_prep(&adapter->napi[scrq->scrq_num])) {
		disable_scrq_irq(adapter, scrq);
		__napi_schedule(&adapter->napi[scrq->scrq_num]);
	}

	return IRQ_HANDLED;
}

static int init_sub_crq_irqs(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue *scrq;
	int i = 0, j = 0;
	int rc = 0;

	for (i = 0; i < adapter->req_tx_queues; i++) {
		netdev_dbg(adapter->netdev, "Initializing tx_scrq[%d] irq\n",
			   i);
		scrq = adapter->tx_scrq[i];
		scrq->irq = irq_create_mapping(NULL, scrq->hw_irq);

		if (!scrq->irq) {
			rc = -EINVAL;
			dev_err(dev, "Error mapping irq\n");
			goto req_tx_irq_failed;
		}

		snprintf(scrq->name, sizeof(scrq->name), "ibmvnic-%x-tx%d",
			 adapter->vdev->unit_address, i);
		rc = request_irq(scrq->irq, ibmvnic_interrupt_tx,
				 0, scrq->name, scrq);

		if (rc) {
			dev_err(dev, "Couldn't register tx irq 0x%x. rc=%d\n",
				scrq->irq, rc);
			irq_dispose_mapping(scrq->irq);
			goto req_tx_irq_failed;
		}
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		netdev_dbg(adapter->netdev, "Initializing rx_scrq[%d] irq\n",
			   i);
		scrq = adapter->rx_scrq[i];
		scrq->irq = irq_create_mapping(NULL, scrq->hw_irq);
		if (!scrq->irq) {
			rc = -EINVAL;
			dev_err(dev, "Error mapping irq\n");
			goto req_rx_irq_failed;
		}
		snprintf(scrq->name, sizeof(scrq->name), "ibmvnic-%x-rx%d",
			 adapter->vdev->unit_address, i);
		rc = request_irq(scrq->irq, ibmvnic_interrupt_rx,
				 0, scrq->name, scrq);
		if (rc) {
			dev_err(dev, "Couldn't register rx irq 0x%x. rc=%d\n",
				scrq->irq, rc);
			irq_dispose_mapping(scrq->irq);
			goto req_rx_irq_failed;
		}
	}

	cpus_read_lock();
	ibmvnic_set_affinity(adapter);
	cpus_read_unlock();

	return rc;

req_rx_irq_failed:
	for (j = 0; j < i; j++) {
		free_irq(adapter->rx_scrq[j]->irq, adapter->rx_scrq[j]);
		irq_dispose_mapping(adapter->rx_scrq[j]->irq);
	}
	i = adapter->req_tx_queues;
req_tx_irq_failed:
	for (j = 0; j < i; j++) {
		free_irq(adapter->tx_scrq[j]->irq, adapter->tx_scrq[j]);
		irq_dispose_mapping(adapter->tx_scrq[j]->irq);
	}
	release_sub_crqs(adapter, 1);
	return rc;
}

static int init_sub_crqs(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue **allqueues;
	int registered_queues = 0;
	int total_queues;
	int more = 0;
	int i;

	total_queues = adapter->req_tx_queues + adapter->req_rx_queues;

	allqueues = kcalloc(total_queues, sizeof(*allqueues), GFP_KERNEL);
	if (!allqueues)
		return -ENOMEM;

	for (i = 0; i < total_queues; i++) {
		allqueues[i] = init_sub_crq_queue(adapter);
		if (!allqueues[i]) {
			dev_warn(dev, "Couldn't allocate all sub-crqs\n");
			break;
		}
		registered_queues++;
	}

	/* Make sure we were able to register the minimum number of queues */
	if (registered_queues <
	    adapter->min_tx_queues + adapter->min_rx_queues) {
		dev_err(dev, "Fatal: Couldn't init  min number of sub-crqs\n");
		goto tx_failed;
	}

	/* Distribute the failed allocated queues*/
	for (i = 0; i < total_queues - registered_queues + more ; i++) {
		netdev_dbg(adapter->netdev, "Reducing number of queues\n");
		switch (i % 3) {
		case 0:
			if (adapter->req_rx_queues > adapter->min_rx_queues)
				adapter->req_rx_queues--;
			else
				more++;
			break;
		case 1:
			if (adapter->req_tx_queues > adapter->min_tx_queues)
				adapter->req_tx_queues--;
			else
				more++;
			break;
		}
	}

	adapter->tx_scrq = kcalloc(adapter->req_tx_queues,
				   sizeof(*adapter->tx_scrq), GFP_KERNEL);
	if (!adapter->tx_scrq)
		goto tx_failed;

	for (i = 0; i < adapter->req_tx_queues; i++) {
		adapter->tx_scrq[i] = allqueues[i];
		adapter->tx_scrq[i]->pool_index = i;
		adapter->num_active_tx_scrqs++;
	}

	adapter->rx_scrq = kcalloc(adapter->req_rx_queues,
				   sizeof(*adapter->rx_scrq), GFP_KERNEL);
	if (!adapter->rx_scrq)
		goto rx_failed;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		adapter->rx_scrq[i] = allqueues[i + adapter->req_tx_queues];
		adapter->rx_scrq[i]->scrq_num = i;
		adapter->num_active_rx_scrqs++;
	}

	kfree(allqueues);
	return 0;

rx_failed:
	kfree(adapter->tx_scrq);
	adapter->tx_scrq = NULL;
tx_failed:
	for (i = 0; i < registered_queues; i++)
		release_sub_crq_queue(adapter, allqueues[i], 1);
	kfree(allqueues);
	return -ENOMEM;
}

static void send_request_cap(struct ibmvnic_adapter *adapter, int retry)
{
	struct device *dev = &adapter->vdev->dev;
	union ibmvnic_crq crq;
	int max_entries;
	int cap_reqs;

	/* We send out 6 or 7 REQUEST_CAPABILITY CRQs below (depending on
	 * the PROMISC flag). Initialize this count upfront. When the tasklet
	 * receives a response to all of these, it will send the next protocol
	 * message (QUERY_IP_OFFLOAD).
	 */
	if (!(adapter->netdev->flags & IFF_PROMISC) ||
	    adapter->promisc_supported)
		cap_reqs = 7;
	else
		cap_reqs = 6;

	if (!retry) {
		/* Sub-CRQ entries are 32 byte long */
		int entries_page = 4 * PAGE_SIZE / (sizeof(u64) * 4);

		atomic_set(&adapter->running_cap_crqs, cap_reqs);

		if (adapter->min_tx_entries_per_subcrq > entries_page ||
		    adapter->min_rx_add_entries_per_subcrq > entries_page) {
			dev_err(dev, "Fatal, invalid entries per sub-crq\n");
			return;
		}

		if (adapter->desired.mtu)
			adapter->req_mtu = adapter->desired.mtu;
		else
			adapter->req_mtu = adapter->netdev->mtu + ETH_HLEN;

		if (!adapter->desired.tx_entries)
			adapter->desired.tx_entries =
					adapter->max_tx_entries_per_subcrq;
		if (!adapter->desired.rx_entries)
			adapter->desired.rx_entries =
					adapter->max_rx_add_entries_per_subcrq;

		max_entries = IBMVNIC_LTB_SET_SIZE /
			      (adapter->req_mtu + IBMVNIC_BUFFER_HLEN);

		if ((adapter->req_mtu + IBMVNIC_BUFFER_HLEN) *
			adapter->desired.tx_entries > IBMVNIC_LTB_SET_SIZE) {
			adapter->desired.tx_entries = max_entries;
		}

		if ((adapter->req_mtu + IBMVNIC_BUFFER_HLEN) *
			adapter->desired.rx_entries > IBMVNIC_LTB_SET_SIZE) {
			adapter->desired.rx_entries = max_entries;
		}

		if (adapter->desired.tx_entries)
			adapter->req_tx_entries_per_subcrq =
					adapter->desired.tx_entries;
		else
			adapter->req_tx_entries_per_subcrq =
					adapter->max_tx_entries_per_subcrq;

		if (adapter->desired.rx_entries)
			adapter->req_rx_add_entries_per_subcrq =
					adapter->desired.rx_entries;
		else
			adapter->req_rx_add_entries_per_subcrq =
					adapter->max_rx_add_entries_per_subcrq;

		if (adapter->desired.tx_queues)
			adapter->req_tx_queues =
					adapter->desired.tx_queues;
		else
			adapter->req_tx_queues =
					adapter->opt_tx_comp_sub_queues;

		if (adapter->desired.rx_queues)
			adapter->req_rx_queues =
					adapter->desired.rx_queues;
		else
			adapter->req_rx_queues =
					adapter->opt_rx_comp_queues;

		adapter->req_rx_add_queues = adapter->max_rx_add_queues;
	} else {
		atomic_add(cap_reqs, &adapter->running_cap_crqs);
	}
	memset(&crq, 0, sizeof(crq));
	crq.request_capability.first = IBMVNIC_CRQ_CMD;
	crq.request_capability.cmd = REQUEST_CAPABILITY;

	crq.request_capability.capability = cpu_to_be16(REQ_TX_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_tx_queues);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_RX_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_rx_queues);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_RX_ADD_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_rx_add_queues);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability =
	    cpu_to_be16(REQ_TX_ENTRIES_PER_SUBCRQ);
	crq.request_capability.number =
	    cpu_to_be64(adapter->req_tx_entries_per_subcrq);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability =
	    cpu_to_be16(REQ_RX_ADD_ENTRIES_PER_SUBCRQ);
	crq.request_capability.number =
	    cpu_to_be64(adapter->req_rx_add_entries_per_subcrq);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_MTU);
	crq.request_capability.number = cpu_to_be64(adapter->req_mtu);
	cap_reqs--;
	ibmvnic_send_crq(adapter, &crq);

	if (adapter->netdev->flags & IFF_PROMISC) {
		if (adapter->promisc_supported) {
			crq.request_capability.capability =
			    cpu_to_be16(PROMISC_REQUESTED);
			crq.request_capability.number = cpu_to_be64(1);
			cap_reqs--;
			ibmvnic_send_crq(adapter, &crq);
		}
	} else {
		crq.request_capability.capability =
		    cpu_to_be16(PROMISC_REQUESTED);
		crq.request_capability.number = cpu_to_be64(0);
		cap_reqs--;
		ibmvnic_send_crq(adapter, &crq);
	}

	/* Keep at end to catch any discrepancy between expected and actual
	 * CRQs sent.
	 */
	WARN_ON(cap_reqs != 0);
}

static int pending_scrq(struct ibmvnic_adapter *adapter,
			struct ibmvnic_sub_crq_queue *scrq)
{
	union sub_crq *entry = &scrq->msgs[scrq->cur];
	int rc;

	rc = !!(entry->generic.first & IBMVNIC_CRQ_CMD_RSP);

	/* Ensure that the SCRQ valid flag is loaded prior to loading the
	 * contents of the SCRQ descriptor
	 */
	dma_rmb();

	return rc;
}

static union sub_crq *ibmvnic_next_scrq(struct ibmvnic_adapter *adapter,
					struct ibmvnic_sub_crq_queue *scrq)
{
	union sub_crq *entry;
	unsigned long flags;

	spin_lock_irqsave(&scrq->lock, flags);
	entry = &scrq->msgs[scrq->cur];
	if (entry->generic.first & IBMVNIC_CRQ_CMD_RSP) {
		if (++scrq->cur == scrq->size)
			scrq->cur = 0;
	} else {
		entry = NULL;
	}
	spin_unlock_irqrestore(&scrq->lock, flags);

	/* Ensure that the SCRQ valid flag is loaded prior to loading the
	 * contents of the SCRQ descriptor
	 */
	dma_rmb();

	return entry;
}

static union ibmvnic_crq *ibmvnic_next_crq(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *queue = &adapter->crq;
	union ibmvnic_crq *crq;

	crq = &queue->msgs[queue->cur];
	if (crq->generic.first & IBMVNIC_CRQ_CMD_RSP) {
		if (++queue->cur == queue->size)
			queue->cur = 0;
	} else {
		crq = NULL;
	}

	return crq;
}

static void print_subcrq_error(struct device *dev, int rc, const char *func)
{
	switch (rc) {
	case H_PARAMETER:
		dev_warn_ratelimited(dev,
				     "%s failed: Send request is malformed or adapter failover pending. (rc=%d)\n",
				     func, rc);
		break;
	case H_CLOSED:
		dev_warn_ratelimited(dev,
				     "%s failed: Backing queue closed. Adapter is down or failover pending. (rc=%d)\n",
				     func, rc);
		break;
	default:
		dev_err_ratelimited(dev, "%s failed: (rc=%d)\n", func, rc);
		break;
	}
}

static int send_subcrq_indirect(struct ibmvnic_adapter *adapter,
				u64 remote_handle, u64 ioba, u64 num_entries)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	int rc;

	/* Make sure the hypervisor sees the complete request */
	dma_wmb();
	rc = plpar_hcall_norets(H_SEND_SUB_CRQ_INDIRECT, ua,
				cpu_to_be64(remote_handle),
				ioba, num_entries);

	if (rc)
		print_subcrq_error(dev, rc, __func__);

	return rc;
}

static int ibmvnic_send_crq(struct ibmvnic_adapter *adapter,
			    union ibmvnic_crq *crq)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	u64 *u64_crq = (u64 *)crq;
	int rc;

	netdev_dbg(adapter->netdev, "Sending CRQ: %016lx %016lx\n",
		   (unsigned long)cpu_to_be64(u64_crq[0]),
		   (unsigned long)cpu_to_be64(u64_crq[1]));

	if (!adapter->crq.active &&
	    crq->generic.first != IBMVNIC_CRQ_INIT_CMD) {
		dev_warn(dev, "Invalid request detected while CRQ is inactive, possible device state change during reset\n");
		return -EINVAL;
	}

	/* Make sure the hypervisor sees the complete request */
	dma_wmb();

	rc = plpar_hcall_norets(H_SEND_CRQ, ua,
				cpu_to_be64(u64_crq[0]),
				cpu_to_be64(u64_crq[1]));

	if (rc) {
		if (rc == H_CLOSED) {
			dev_warn(dev, "CRQ Queue closed\n");
			/* do not reset, report the fail, wait for passive init from server */
		}

		dev_warn(dev, "Send error (rc=%d)\n", rc);
	}

	return rc;
}

static int ibmvnic_send_crq_init(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	union ibmvnic_crq crq;
	int retries = 100;
	int rc;

	memset(&crq, 0, sizeof(crq));
	crq.generic.first = IBMVNIC_CRQ_INIT_CMD;
	crq.generic.cmd = IBMVNIC_CRQ_INIT;
	netdev_dbg(adapter->netdev, "Sending CRQ init\n");

	do {
		rc = ibmvnic_send_crq(adapter, &crq);
		if (rc != H_CLOSED)
			break;
		retries--;
		msleep(50);

	} while (retries > 0);

	if (rc) {
		dev_err(dev, "Failed to send init request, rc = %d\n", rc);
		return rc;
	}

	return 0;
}

struct vnic_login_client_data {
	u8	type;
	__be16	len;
	char	name[];
} __packed;

static int vnic_client_data_len(struct ibmvnic_adapter *adapter)
{
	int len;

	/* Calculate the amount of buffer space needed for the
	 * vnic client data in the login buffer. There are four entries,
	 * OS name, LPAR name, device name, and a null last entry.
	 */
	len = 4 * sizeof(struct vnic_login_client_data);
	len += 6; /* "Linux" plus NULL */
	len += strlen(utsname()->nodename) + 1;
	len += strlen(adapter->netdev->name) + 1;

	return len;
}

static void vnic_add_client_data(struct ibmvnic_adapter *adapter,
				 struct vnic_login_client_data *vlcd)
{
	const char *os_name = "Linux";
	int len;

	/* Type 1 - LPAR OS */
	vlcd->type = 1;
	len = strlen(os_name) + 1;
	vlcd->len = cpu_to_be16(len);
	strscpy(vlcd->name, os_name, len);
	vlcd = (struct vnic_login_client_data *)(vlcd->name + len);

	/* Type 2 - LPAR name */
	vlcd->type = 2;
	len = strlen(utsname()->nodename) + 1;
	vlcd->len = cpu_to_be16(len);
	strscpy(vlcd->name, utsname()->nodename, len);
	vlcd = (struct vnic_login_client_data *)(vlcd->name + len);

	/* Type 3 - device name */
	vlcd->type = 3;
	len = strlen(adapter->netdev->name) + 1;
	vlcd->len = cpu_to_be16(len);
	strscpy(vlcd->name, adapter->netdev->name, len);
}

static void ibmvnic_print_hex_dump(struct net_device *dev, void *buf,
				   size_t len)
{
	unsigned char hex_str[16 * 3];

	for (size_t i = 0; i < len; i += 16) {
		hex_dump_to_buffer((unsigned char *)buf + i, len - i, 16, 8,
				   hex_str, sizeof(hex_str), false);
		netdev_dbg(dev, "%s\n", hex_str);
	}
}

static int send_login(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_login_rsp_buffer *login_rsp_buffer;
	struct ibmvnic_login_buffer *login_buffer;
	struct device *dev = &adapter->vdev->dev;
	struct vnic_login_client_data *vlcd;
	dma_addr_t rsp_buffer_token;
	dma_addr_t buffer_token;
	size_t rsp_buffer_size;
	union ibmvnic_crq crq;
	int client_data_len;
	size_t buffer_size;
	__be64 *tx_list_p;
	__be64 *rx_list_p;
	int rc;
	int i;

	if (!adapter->tx_scrq || !adapter->rx_scrq) {
		netdev_err(adapter->netdev,
			   "RX or TX queues are not allocated, device login failed\n");
		return -ENOMEM;
	}

	release_login_buffer(adapter);
	release_login_rsp_buffer(adapter);

	client_data_len = vnic_client_data_len(adapter);

	buffer_size =
	    sizeof(struct ibmvnic_login_buffer) +
	    sizeof(u64) * (adapter->req_tx_queues + adapter->req_rx_queues) +
	    client_data_len;

	login_buffer = kzalloc(buffer_size, GFP_ATOMIC);
	if (!login_buffer)
		goto buf_alloc_failed;

	buffer_token = dma_map_single(dev, login_buffer, buffer_size,
				      DMA_TO_DEVICE);
	if (dma_mapping_error(dev, buffer_token)) {
		dev_err(dev, "Couldn't map login buffer\n");
		goto buf_map_failed;
	}

	rsp_buffer_size = sizeof(struct ibmvnic_login_rsp_buffer) +
			  sizeof(u64) * adapter->req_tx_queues +
			  sizeof(u64) * adapter->req_rx_queues +
			  sizeof(u64) * adapter->req_rx_queues +
			  sizeof(u8) * IBMVNIC_TX_DESC_VERSIONS;

	login_rsp_buffer = kmalloc(rsp_buffer_size, GFP_ATOMIC);
	if (!login_rsp_buffer)
		goto buf_rsp_alloc_failed;

	rsp_buffer_token = dma_map_single(dev, login_rsp_buffer,
					  rsp_buffer_size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, rsp_buffer_token)) {
		dev_err(dev, "Couldn't map login rsp buffer\n");
		goto buf_rsp_map_failed;
	}

	adapter->login_buf = login_buffer;
	adapter->login_buf_token = buffer_token;
	adapter->login_buf_sz = buffer_size;
	adapter->login_rsp_buf = login_rsp_buffer;
	adapter->login_rsp_buf_token = rsp_buffer_token;
	adapter->login_rsp_buf_sz = rsp_buffer_size;

	login_buffer->len = cpu_to_be32(buffer_size);
	login_buffer->version = cpu_to_be32(INITIAL_VERSION_LB);
	login_buffer->num_txcomp_subcrqs = cpu_to_be32(adapter->req_tx_queues);
	login_buffer->off_txcomp_subcrqs =
	    cpu_to_be32(sizeof(struct ibmvnic_login_buffer));
	login_buffer->num_rxcomp_subcrqs = cpu_to_be32(adapter->req_rx_queues);
	login_buffer->off_rxcomp_subcrqs =
	    cpu_to_be32(sizeof(struct ibmvnic_login_buffer) +
			sizeof(u64) * adapter->req_tx_queues);
	login_buffer->login_rsp_ioba = cpu_to_be32(rsp_buffer_token);
	login_buffer->login_rsp_len = cpu_to_be32(rsp_buffer_size);

	tx_list_p = (__be64 *)((char *)login_buffer +
				      sizeof(struct ibmvnic_login_buffer));
	rx_list_p = (__be64 *)((char *)login_buffer +
				      sizeof(struct ibmvnic_login_buffer) +
				      sizeof(u64) * adapter->req_tx_queues);

	for (i = 0; i < adapter->req_tx_queues; i++) {
		if (adapter->tx_scrq[i]) {
			tx_list_p[i] =
				cpu_to_be64(adapter->tx_scrq[i]->crq_num);
		}
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		if (adapter->rx_scrq[i]) {
			rx_list_p[i] =
				cpu_to_be64(adapter->rx_scrq[i]->crq_num);
		}
	}

	/* Insert vNIC login client data */
	vlcd = (struct vnic_login_client_data *)
		((char *)rx_list_p + (sizeof(u64) * adapter->req_rx_queues));
	login_buffer->client_data_offset =
			cpu_to_be32((char *)vlcd - (char *)login_buffer);
	login_buffer->client_data_len = cpu_to_be32(client_data_len);

	vnic_add_client_data(adapter, vlcd);

	netdev_dbg(adapter->netdev, "Login Buffer:\n");
	ibmvnic_print_hex_dump(adapter->netdev, adapter->login_buf,
			       adapter->login_buf_sz);

	memset(&crq, 0, sizeof(crq));
	crq.login.first = IBMVNIC_CRQ_CMD;
	crq.login.cmd = LOGIN;
	crq.login.ioba = cpu_to_be32(buffer_token);
	crq.login.len = cpu_to_be32(buffer_size);

	adapter->login_pending = true;
	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		adapter->login_pending = false;
		netdev_err(adapter->netdev, "Failed to send login, rc=%d\n", rc);
		goto buf_send_failed;
	}

	return 0;

buf_send_failed:
	dma_unmap_single(dev, rsp_buffer_token, rsp_buffer_size,
			 DMA_FROM_DEVICE);
buf_rsp_map_failed:
	kfree(login_rsp_buffer);
	adapter->login_rsp_buf = NULL;
buf_rsp_alloc_failed:
	dma_unmap_single(dev, buffer_token, buffer_size, DMA_TO_DEVICE);
buf_map_failed:
	kfree(login_buffer);
	adapter->login_buf = NULL;
buf_alloc_failed:
	return -ENOMEM;
}

static int send_request_map(struct ibmvnic_adapter *adapter, dma_addr_t addr,
			    u32 len, u8 map_id)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_map.first = IBMVNIC_CRQ_CMD;
	crq.request_map.cmd = REQUEST_MAP;
	crq.request_map.map_id = map_id;
	crq.request_map.ioba = cpu_to_be32(addr);
	crq.request_map.len = cpu_to_be32(len);
	return ibmvnic_send_crq(adapter, &crq);
}

static int send_request_unmap(struct ibmvnic_adapter *adapter, u8 map_id)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_unmap.first = IBMVNIC_CRQ_CMD;
	crq.request_unmap.cmd = REQUEST_UNMAP;
	crq.request_unmap.map_id = map_id;
	return ibmvnic_send_crq(adapter, &crq);
}

static void send_query_map(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.query_map.first = IBMVNIC_CRQ_CMD;
	crq.query_map.cmd = QUERY_MAP;
	ibmvnic_send_crq(adapter, &crq);
}

/* Send a series of CRQs requesting various capabilities of the VNIC server */
static void send_query_cap(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;
	int cap_reqs;

	/* We send out 25 QUERY_CAPABILITY CRQs below.  Initialize this count
	 * upfront. When the tasklet receives a response to all of these, it
	 * can send out the next protocol messaage (REQUEST_CAPABILITY).
	 */
	cap_reqs = 25;

	atomic_set(&adapter->running_cap_crqs, cap_reqs);

	memset(&crq, 0, sizeof(crq));
	crq.query_capability.first = IBMVNIC_CRQ_CMD;
	crq.query_capability.cmd = QUERY_CAPABILITY;

	crq.query_capability.capability = cpu_to_be16(MIN_TX_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MIN_RX_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MIN_RX_ADD_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_TX_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_RX_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_RX_ADD_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
	    cpu_to_be16(MIN_TX_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
	    cpu_to_be16(MIN_RX_ADD_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
	    cpu_to_be16(MAX_TX_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
	    cpu_to_be16(MAX_RX_ADD_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(TCP_IP_OFFLOAD);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(PROMISC_SUPPORTED);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MIN_MTU);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_MTU);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_MULTICAST_FILTERS);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(VLAN_HEADER_INSERTION);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(RX_VLAN_HEADER_INSERTION);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(MAX_TX_SG_ENTRIES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(RX_SG_SUPPORTED);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(OPT_TX_COMP_SUB_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(OPT_RX_COMP_QUEUES);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
			cpu_to_be16(OPT_RX_BUFADD_Q_PER_RX_COMP_Q);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
			cpu_to_be16(OPT_TX_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability =
			cpu_to_be16(OPT_RXBA_ENTRIES_PER_SUBCRQ);
	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	crq.query_capability.capability = cpu_to_be16(TX_RX_DESC_REQ);

	ibmvnic_send_crq(adapter, &crq);
	cap_reqs--;

	/* Keep at end to catch any discrepancy between expected and actual
	 * CRQs sent.
	 */
	WARN_ON(cap_reqs != 0);
}

static void send_query_ip_offload(struct ibmvnic_adapter *adapter)
{
	int buf_sz = sizeof(struct ibmvnic_query_ip_offload_buffer);
	struct device *dev = &adapter->vdev->dev;
	union ibmvnic_crq crq;

	adapter->ip_offload_tok =
		dma_map_single(dev,
			       &adapter->ip_offload_buf,
			       buf_sz,
			       DMA_FROM_DEVICE);

	if (dma_mapping_error(dev, adapter->ip_offload_tok)) {
		if (!firmware_has_feature(FW_FEATURE_CMO))
			dev_err(dev, "Couldn't map offload buffer\n");
		return;
	}

	memset(&crq, 0, sizeof(crq));
	crq.query_ip_offload.first = IBMVNIC_CRQ_CMD;
	crq.query_ip_offload.cmd = QUERY_IP_OFFLOAD;
	crq.query_ip_offload.len = cpu_to_be32(buf_sz);
	crq.query_ip_offload.ioba =
	    cpu_to_be32(adapter->ip_offload_tok);

	ibmvnic_send_crq(adapter, &crq);
}

static void send_control_ip_offload(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_control_ip_offload_buffer *ctrl_buf = &adapter->ip_offload_ctrl;
	struct ibmvnic_query_ip_offload_buffer *buf = &adapter->ip_offload_buf;
	struct device *dev = &adapter->vdev->dev;
	netdev_features_t old_hw_features = 0;
	union ibmvnic_crq crq;

	adapter->ip_offload_ctrl_tok =
		dma_map_single(dev,
			       ctrl_buf,
			       sizeof(adapter->ip_offload_ctrl),
			       DMA_TO_DEVICE);

	if (dma_mapping_error(dev, adapter->ip_offload_ctrl_tok)) {
		dev_err(dev, "Couldn't map ip offload control buffer\n");
		return;
	}

	ctrl_buf->len = cpu_to_be32(sizeof(adapter->ip_offload_ctrl));
	ctrl_buf->version = cpu_to_be32(INITIAL_VERSION_IOB);
	ctrl_buf->ipv4_chksum = buf->ipv4_chksum;
	ctrl_buf->ipv6_chksum = buf->ipv6_chksum;
	ctrl_buf->tcp_ipv4_chksum = buf->tcp_ipv4_chksum;
	ctrl_buf->udp_ipv4_chksum = buf->udp_ipv4_chksum;
	ctrl_buf->tcp_ipv6_chksum = buf->tcp_ipv6_chksum;
	ctrl_buf->udp_ipv6_chksum = buf->udp_ipv6_chksum;
	ctrl_buf->large_tx_ipv4 = buf->large_tx_ipv4;
	ctrl_buf->large_tx_ipv6 = buf->large_tx_ipv6;

	/* large_rx disabled for now, additional features needed */
	ctrl_buf->large_rx_ipv4 = 0;
	ctrl_buf->large_rx_ipv6 = 0;

	if (adapter->state != VNIC_PROBING) {
		old_hw_features = adapter->netdev->hw_features;
		adapter->netdev->hw_features = 0;
	}

	adapter->netdev->hw_features = NETIF_F_SG | NETIF_F_GSO | NETIF_F_GRO;

	if (buf->tcp_ipv4_chksum || buf->udp_ipv4_chksum)
		adapter->netdev->hw_features |= NETIF_F_IP_CSUM;

	if (buf->tcp_ipv6_chksum || buf->udp_ipv6_chksum)
		adapter->netdev->hw_features |= NETIF_F_IPV6_CSUM;

	if ((adapter->netdev->features &
	    (NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM)))
		adapter->netdev->hw_features |= NETIF_F_RXCSUM;

	if (buf->large_tx_ipv4)
		adapter->netdev->hw_features |= NETIF_F_TSO;
	if (buf->large_tx_ipv6)
		adapter->netdev->hw_features |= NETIF_F_TSO6;

	if (adapter->state == VNIC_PROBING) {
		adapter->netdev->features |= adapter->netdev->hw_features;
	} else if (old_hw_features != adapter->netdev->hw_features) {
		netdev_features_t tmp = 0;

		/* disable features no longer supported */
		adapter->netdev->features &= adapter->netdev->hw_features;
		/* turn on features now supported if previously enabled */
		tmp = (old_hw_features ^ adapter->netdev->hw_features) &
			adapter->netdev->hw_features;
		adapter->netdev->features |=
				tmp & adapter->netdev->wanted_features;
	}

	memset(&crq, 0, sizeof(crq));
	crq.control_ip_offload.first = IBMVNIC_CRQ_CMD;
	crq.control_ip_offload.cmd = CONTROL_IP_OFFLOAD;
	crq.control_ip_offload.len =
	    cpu_to_be32(sizeof(adapter->ip_offload_ctrl));
	crq.control_ip_offload.ioba = cpu_to_be32(adapter->ip_offload_ctrl_tok);
	ibmvnic_send_crq(adapter, &crq);
}

static void handle_vpd_size_rsp(union ibmvnic_crq *crq,
				struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	if (crq->get_vpd_size_rsp.rc.code) {
		dev_err(dev, "Error retrieving VPD size, rc=%x\n",
			crq->get_vpd_size_rsp.rc.code);
		complete(&adapter->fw_done);
		return;
	}

	adapter->vpd->len = be64_to_cpu(crq->get_vpd_size_rsp.len);
	complete(&adapter->fw_done);
}

static void handle_vpd_rsp(union ibmvnic_crq *crq,
			   struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned char *substr = NULL;
	u8 fw_level_len = 0;

	memset(adapter->fw_version, 0, 32);

	dma_unmap_single(dev, adapter->vpd->dma_addr, adapter->vpd->len,
			 DMA_FROM_DEVICE);

	if (crq->get_vpd_rsp.rc.code) {
		dev_err(dev, "Error retrieving VPD from device, rc=%x\n",
			crq->get_vpd_rsp.rc.code);
		goto complete;
	}

	/* get the position of the firmware version info
	 * located after the ASCII 'RM' substring in the buffer
	 */
	substr = strnstr(adapter->vpd->buff, "RM", adapter->vpd->len);
	if (!substr) {
		dev_info(dev, "Warning - No FW level has been provided in the VPD buffer by the VIOS Server\n");
		goto complete;
	}

	/* get length of firmware level ASCII substring */
	if ((substr + 2) < (adapter->vpd->buff + adapter->vpd->len)) {
		fw_level_len = *(substr + 2);
	} else {
		dev_info(dev, "Length of FW substr extrapolated VDP buff\n");
		goto complete;
	}

	/* copy firmware version string from vpd into adapter */
	if ((substr + 3 + fw_level_len) <
	    (adapter->vpd->buff + adapter->vpd->len)) {
		strscpy(adapter->fw_version, substr + 3,
			sizeof(adapter->fw_version));
	} else {
		dev_info(dev, "FW substr extrapolated VPD buff\n");
	}

complete:
	if (adapter->fw_version[0] == '\0')
		strscpy((char *)adapter->fw_version, "N/A", sizeof(adapter->fw_version));
	complete(&adapter->fw_done);
}

static void handle_query_ip_offload_rsp(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_query_ip_offload_buffer *buf = &adapter->ip_offload_buf;

	dma_unmap_single(dev, adapter->ip_offload_tok,
			 sizeof(adapter->ip_offload_buf), DMA_FROM_DEVICE);

	netdev_dbg(adapter->netdev, "Query IP Offload Buffer:\n");
	ibmvnic_print_hex_dump(adapter->netdev, buf,
			       sizeof(adapter->ip_offload_buf));

	netdev_dbg(adapter->netdev, "ipv4_chksum = %d\n", buf->ipv4_chksum);
	netdev_dbg(adapter->netdev, "ipv6_chksum = %d\n", buf->ipv6_chksum);
	netdev_dbg(adapter->netdev, "tcp_ipv4_chksum = %d\n",
		   buf->tcp_ipv4_chksum);
	netdev_dbg(adapter->netdev, "tcp_ipv6_chksum = %d\n",
		   buf->tcp_ipv6_chksum);
	netdev_dbg(adapter->netdev, "udp_ipv4_chksum = %d\n",
		   buf->udp_ipv4_chksum);
	netdev_dbg(adapter->netdev, "udp_ipv6_chksum = %d\n",
		   buf->udp_ipv6_chksum);
	netdev_dbg(adapter->netdev, "large_tx_ipv4 = %d\n",
		   buf->large_tx_ipv4);
	netdev_dbg(adapter->netdev, "large_tx_ipv6 = %d\n",
		   buf->large_tx_ipv6);
	netdev_dbg(adapter->netdev, "large_rx_ipv4 = %d\n",
		   buf->large_rx_ipv4);
	netdev_dbg(adapter->netdev, "large_rx_ipv6 = %d\n",
		   buf->large_rx_ipv6);
	netdev_dbg(adapter->netdev, "max_ipv4_hdr_sz = %d\n",
		   buf->max_ipv4_header_size);
	netdev_dbg(adapter->netdev, "max_ipv6_hdr_sz = %d\n",
		   buf->max_ipv6_header_size);
	netdev_dbg(adapter->netdev, "max_tcp_hdr_size = %d\n",
		   buf->max_tcp_header_size);
	netdev_dbg(adapter->netdev, "max_udp_hdr_size = %d\n",
		   buf->max_udp_header_size);
	netdev_dbg(adapter->netdev, "max_large_tx_size = %d\n",
		   buf->max_large_tx_size);
	netdev_dbg(adapter->netdev, "max_large_rx_size = %d\n",
		   buf->max_large_rx_size);
	netdev_dbg(adapter->netdev, "ipv6_ext_hdr = %d\n",
		   buf->ipv6_extension_header);
	netdev_dbg(adapter->netdev, "tcp_pseudosum_req = %d\n",
		   buf->tcp_pseudosum_req);
	netdev_dbg(adapter->netdev, "num_ipv6_ext_hd = %d\n",
		   buf->num_ipv6_ext_headers);
	netdev_dbg(adapter->netdev, "off_ipv6_ext_hd = %d\n",
		   buf->off_ipv6_ext_headers);

	send_control_ip_offload(adapter);
}

static const char *ibmvnic_fw_err_cause(u16 cause)
{
	switch (cause) {
	case ADAPTER_PROBLEM:
		return "adapter problem";
	case BUS_PROBLEM:
		return "bus problem";
	case FW_PROBLEM:
		return "firmware problem";
	case DD_PROBLEM:
		return "device driver problem";
	case EEH_RECOVERY:
		return "EEH recovery";
	case FW_UPDATED:
		return "firmware updated";
	case LOW_MEMORY:
		return "low Memory";
	default:
		return "unknown";
	}
}

static void handle_error_indication(union ibmvnic_crq *crq,
				    struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	u16 cause;

	cause = be16_to_cpu(crq->error_indication.error_cause);

	dev_warn_ratelimited(dev,
			     "Firmware reports %serror, cause: %s. Starting recovery...\n",
			     crq->error_indication.flags
				& IBMVNIC_FATAL_ERROR ? "FATAL " : "",
			     ibmvnic_fw_err_cause(cause));

	if (crq->error_indication.flags & IBMVNIC_FATAL_ERROR)
		ibmvnic_reset(adapter, VNIC_RESET_FATAL);
	else
		ibmvnic_reset(adapter, VNIC_RESET_NON_FATAL);
}

static int handle_change_mac_rsp(union ibmvnic_crq *crq,
				 struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->change_mac_addr_rsp.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in CHANGE_MAC_ADDR_RSP\n", rc);
		goto out;
	}
	/* crq->change_mac_addr.mac_addr is the requested one
	 * crq->change_mac_addr_rsp.mac_addr is the returned valid one.
	 */
	eth_hw_addr_set(netdev, &crq->change_mac_addr_rsp.mac_addr[0]);
	ether_addr_copy(adapter->mac_addr,
			&crq->change_mac_addr_rsp.mac_addr[0]);
out:
	complete(&adapter->fw_done);
	return rc;
}

static void handle_request_cap_rsp(union ibmvnic_crq *crq,
				   struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	u64 *req_value;
	char *name;

	atomic_dec(&adapter->running_cap_crqs);
	netdev_dbg(adapter->netdev, "Outstanding request-caps: %d\n",
		   atomic_read(&adapter->running_cap_crqs));
	switch (be16_to_cpu(crq->request_capability_rsp.capability)) {
	case REQ_TX_QUEUES:
		req_value = &adapter->req_tx_queues;
		name = "tx";
		break;
	case REQ_RX_QUEUES:
		req_value = &adapter->req_rx_queues;
		name = "rx";
		break;
	case REQ_RX_ADD_QUEUES:
		req_value = &adapter->req_rx_add_queues;
		name = "rx_add";
		break;
	case REQ_TX_ENTRIES_PER_SUBCRQ:
		req_value = &adapter->req_tx_entries_per_subcrq;
		name = "tx_entries_per_subcrq";
		break;
	case REQ_RX_ADD_ENTRIES_PER_SUBCRQ:
		req_value = &adapter->req_rx_add_entries_per_subcrq;
		name = "rx_add_entries_per_subcrq";
		break;
	case REQ_MTU:
		req_value = &adapter->req_mtu;
		name = "mtu";
		break;
	case PROMISC_REQUESTED:
		req_value = &adapter->promisc;
		name = "promisc";
		break;
	default:
		dev_err(dev, "Got invalid cap request rsp %d\n",
			crq->request_capability.capability);
		return;
	}

	switch (crq->request_capability_rsp.rc.code) {
	case SUCCESS:
		break;
	case PARTIALSUCCESS:
		dev_info(dev, "req=%lld, rsp=%ld in %s queue, retrying.\n",
			 *req_value,
			 (long)be64_to_cpu(crq->request_capability_rsp.number),
			 name);

		if (be16_to_cpu(crq->request_capability_rsp.capability) ==
		    REQ_MTU) {
			pr_err("mtu of %llu is not supported. Reverting.\n",
			       *req_value);
			*req_value = adapter->fallback.mtu;
		} else {
			*req_value =
				be64_to_cpu(crq->request_capability_rsp.number);
		}

		send_request_cap(adapter, 1);
		return;
	default:
		dev_err(dev, "Error %d in request cap rsp\n",
			crq->request_capability_rsp.rc.code);
		return;
	}

	/* Done receiving requested capabilities, query IP offload support */
	if (atomic_read(&adapter->running_cap_crqs) == 0)
		send_query_ip_offload(adapter);
}

static int handle_login_rsp(union ibmvnic_crq *login_rsp_crq,
			    struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;
	struct ibmvnic_login_rsp_buffer *login_rsp = adapter->login_rsp_buf;
	struct ibmvnic_login_buffer *login = adapter->login_buf;
	u64 *tx_handle_array;
	u64 *rx_handle_array;
	int num_tx_pools;
	int num_rx_pools;
	u64 *size_array;
	u32 rsp_len;
	int i;

	/* CHECK: Test/set of login_pending does not need to be atomic
	 * because only ibmvnic_tasklet tests/clears this.
	 */
	if (!adapter->login_pending) {
		netdev_warn(netdev, "Ignoring unexpected login response\n");
		return 0;
	}
	adapter->login_pending = false;

	/* If the number of queues requested can't be allocated by the
	 * server, the login response will return with code 1. We will need
	 * to resend the login buffer with fewer queues requested.
	 */
	if (login_rsp_crq->generic.rc.code) {
		adapter->init_done_rc = login_rsp_crq->generic.rc.code;
		complete(&adapter->init_done);
		return 0;
	}

	if (adapter->failover_pending) {
		adapter->init_done_rc = -EAGAIN;
		netdev_dbg(netdev, "Failover pending, ignoring login response\n");
		complete(&adapter->init_done);
		/* login response buffer will be released on reset */
		return 0;
	}

	netdev->mtu = adapter->req_mtu - ETH_HLEN;

	netdev_dbg(adapter->netdev, "Login Response Buffer:\n");
	ibmvnic_print_hex_dump(netdev, adapter->login_rsp_buf,
			       adapter->login_rsp_buf_sz);

	/* Sanity checks */
	if (login->num_txcomp_subcrqs != login_rsp->num_txsubm_subcrqs ||
	    (be32_to_cpu(login->num_rxcomp_subcrqs) *
	     adapter->req_rx_add_queues !=
	     be32_to_cpu(login_rsp->num_rxadd_subcrqs))) {
		dev_err(dev, "FATAL: Inconsistent login and login rsp\n");
		ibmvnic_reset(adapter, VNIC_RESET_FATAL);
		return -EIO;
	}

	rsp_len = be32_to_cpu(login_rsp->len);
	if (be32_to_cpu(login->login_rsp_len) < rsp_len ||
	    rsp_len <= be32_to_cpu(login_rsp->off_txsubm_subcrqs) ||
	    rsp_len <= be32_to_cpu(login_rsp->off_rxadd_subcrqs) ||
	    rsp_len <= be32_to_cpu(login_rsp->off_rxadd_buff_size) ||
	    rsp_len <= be32_to_cpu(login_rsp->off_supp_tx_desc)) {
		/* This can happen if a login request times out and there are
		 * 2 outstanding login requests sent, the LOGIN_RSP crq
		 * could have been for the older login request. So we are
		 * parsing the newer response buffer which may be incomplete
		 */
		dev_err(dev, "FATAL: Login rsp offsets/lengths invalid\n");
		ibmvnic_reset(adapter, VNIC_RESET_FATAL);
		return -EIO;
	}

	size_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
		be32_to_cpu(adapter->login_rsp_buf->off_rxadd_buff_size));
	/* variable buffer sizes are not supported, so just read the
	 * first entry.
	 */
	adapter->cur_rx_buf_sz = be64_to_cpu(size_array[0]);

	num_tx_pools = be32_to_cpu(adapter->login_rsp_buf->num_txsubm_subcrqs);
	num_rx_pools = be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);

	tx_handle_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
				  be32_to_cpu(adapter->login_rsp_buf->off_txsubm_subcrqs));
	rx_handle_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
				  be32_to_cpu(adapter->login_rsp_buf->off_rxadd_subcrqs));

	for (i = 0; i < num_tx_pools; i++)
		adapter->tx_scrq[i]->handle = tx_handle_array[i];

	for (i = 0; i < num_rx_pools; i++)
		adapter->rx_scrq[i]->handle = rx_handle_array[i];

	adapter->num_active_tx_scrqs = num_tx_pools;
	adapter->num_active_rx_scrqs = num_rx_pools;
	release_login_rsp_buffer(adapter);
	release_login_buffer(adapter);
	complete(&adapter->init_done);

	return 0;
}

static void handle_request_unmap_rsp(union ibmvnic_crq *crq,
				     struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->request_unmap_rsp.rc.code;
	if (rc)
		dev_err(dev, "Error %ld in REQUEST_UNMAP_RSP\n", rc);
}

static void handle_query_map_rsp(union ibmvnic_crq *crq,
				 struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->query_map_rsp.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in QUERY_MAP_RSP\n", rc);
		return;
	}
	netdev_dbg(netdev, "page_size = %d\ntot_pages = %u\nfree_pages = %u\n",
		   crq->query_map_rsp.page_size,
		   __be32_to_cpu(crq->query_map_rsp.tot_pages),
		   __be32_to_cpu(crq->query_map_rsp.free_pages));
}

static void handle_query_cap_rsp(union ibmvnic_crq *crq,
				 struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	atomic_dec(&adapter->running_cap_crqs);
	netdev_dbg(netdev, "Outstanding queries: %d\n",
		   atomic_read(&adapter->running_cap_crqs));
	rc = crq->query_capability.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in QUERY_CAP_RSP\n", rc);
		goto out;
	}

	switch (be16_to_cpu(crq->query_capability.capability)) {
	case MIN_TX_QUEUES:
		adapter->min_tx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_tx_queues = %lld\n",
			   adapter->min_tx_queues);
		break;
	case MIN_RX_QUEUES:
		adapter->min_rx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_queues = %lld\n",
			   adapter->min_rx_queues);
		break;
	case MIN_RX_ADD_QUEUES:
		adapter->min_rx_add_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_add_queues = %lld\n",
			   adapter->min_rx_add_queues);
		break;
	case MAX_TX_QUEUES:
		adapter->max_tx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_queues = %lld\n",
			   adapter->max_tx_queues);
		break;
	case MAX_RX_QUEUES:
		adapter->max_rx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_queues = %lld\n",
			   adapter->max_rx_queues);
		break;
	case MAX_RX_ADD_QUEUES:
		adapter->max_rx_add_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_add_queues = %lld\n",
			   adapter->max_rx_add_queues);
		break;
	case MIN_TX_ENTRIES_PER_SUBCRQ:
		adapter->min_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_tx_entries_per_subcrq = %lld\n",
			   adapter->min_tx_entries_per_subcrq);
		break;
	case MIN_RX_ADD_ENTRIES_PER_SUBCRQ:
		adapter->min_rx_add_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_add_entrs_per_subcrq = %lld\n",
			   adapter->min_rx_add_entries_per_subcrq);
		break;
	case MAX_TX_ENTRIES_PER_SUBCRQ:
		adapter->max_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_entries_per_subcrq = %lld\n",
			   adapter->max_tx_entries_per_subcrq);
		break;
	case MAX_RX_ADD_ENTRIES_PER_SUBCRQ:
		adapter->max_rx_add_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_add_entrs_per_subcrq = %lld\n",
			   adapter->max_rx_add_entries_per_subcrq);
		break;
	case TCP_IP_OFFLOAD:
		adapter->tcp_ip_offload =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "tcp_ip_offload = %lld\n",
			   adapter->tcp_ip_offload);
		break;
	case PROMISC_SUPPORTED:
		adapter->promisc_supported =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "promisc_supported = %lld\n",
			   adapter->promisc_supported);
		break;
	case MIN_MTU:
		adapter->min_mtu = be64_to_cpu(crq->query_capability.number);
		netdev->min_mtu = adapter->min_mtu - ETH_HLEN;
		netdev_dbg(netdev, "min_mtu = %lld\n", adapter->min_mtu);
		break;
	case MAX_MTU:
		adapter->max_mtu = be64_to_cpu(crq->query_capability.number);
		netdev->max_mtu = adapter->max_mtu - ETH_HLEN;
		netdev_dbg(netdev, "max_mtu = %lld\n", adapter->max_mtu);
		break;
	case MAX_MULTICAST_FILTERS:
		adapter->max_multicast_filters =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_multicast_filters = %lld\n",
			   adapter->max_multicast_filters);
		break;
	case VLAN_HEADER_INSERTION:
		adapter->vlan_header_insertion =
		    be64_to_cpu(crq->query_capability.number);
		if (adapter->vlan_header_insertion)
			netdev->features |= NETIF_F_HW_VLAN_STAG_TX;
		netdev_dbg(netdev, "vlan_header_insertion = %lld\n",
			   adapter->vlan_header_insertion);
		break;
	case RX_VLAN_HEADER_INSERTION:
		adapter->rx_vlan_header_insertion =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "rx_vlan_header_insertion = %lld\n",
			   adapter->rx_vlan_header_insertion);
		break;
	case MAX_TX_SG_ENTRIES:
		adapter->max_tx_sg_entries =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_sg_entries = %lld\n",
			   adapter->max_tx_sg_entries);
		break;
	case RX_SG_SUPPORTED:
		adapter->rx_sg_supported =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "rx_sg_supported = %lld\n",
			   adapter->rx_sg_supported);
		break;
	case OPT_TX_COMP_SUB_QUEUES:
		adapter->opt_tx_comp_sub_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_tx_comp_sub_queues = %lld\n",
			   adapter->opt_tx_comp_sub_queues);
		break;
	case OPT_RX_COMP_QUEUES:
		adapter->opt_rx_comp_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rx_comp_queues = %lld\n",
			   adapter->opt_rx_comp_queues);
		break;
	case OPT_RX_BUFADD_Q_PER_RX_COMP_Q:
		adapter->opt_rx_bufadd_q_per_rx_comp_q =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rx_bufadd_q_per_rx_comp_q = %lld\n",
			   adapter->opt_rx_bufadd_q_per_rx_comp_q);
		break;
	case OPT_TX_ENTRIES_PER_SUBCRQ:
		adapter->opt_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_tx_entries_per_subcrq = %lld\n",
			   adapter->opt_tx_entries_per_subcrq);
		break;
	case OPT_RXBA_ENTRIES_PER_SUBCRQ:
		adapter->opt_rxba_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rxba_entries_per_subcrq = %lld\n",
			   adapter->opt_rxba_entries_per_subcrq);
		break;
	case TX_RX_DESC_REQ:
		adapter->tx_rx_desc_req = crq->query_capability.number;
		netdev_dbg(netdev, "tx_rx_desc_req = %llx\n",
			   adapter->tx_rx_desc_req);
		break;

	default:
		netdev_err(netdev, "Got invalid cap rsp %d\n",
			   crq->query_capability.capability);
	}

out:
	if (atomic_read(&adapter->running_cap_crqs) == 0)
		send_request_cap(adapter, 0);
}

static int send_query_phys_parms(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;
	int rc;

	memset(&crq, 0, sizeof(crq));
	crq.query_phys_parms.first = IBMVNIC_CRQ_CMD;
	crq.query_phys_parms.cmd = QUERY_PHYS_PARMS;

	mutex_lock(&adapter->fw_lock);
	adapter->fw_done_rc = 0;
	reinit_completion(&adapter->fw_done);

	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}

	rc = ibmvnic_wait_for_completion(adapter, &adapter->fw_done, 10000);
	if (rc) {
		mutex_unlock(&adapter->fw_lock);
		return rc;
	}

	mutex_unlock(&adapter->fw_lock);
	return adapter->fw_done_rc ? -EIO : 0;
}

static int handle_query_phys_parms_rsp(union ibmvnic_crq *crq,
				       struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int rc;
	__be32 rspeed = cpu_to_be32(crq->query_phys_parms_rsp.speed);

	rc = crq->query_phys_parms_rsp.rc.code;
	if (rc) {
		netdev_err(netdev, "Error %d in QUERY_PHYS_PARMS\n", rc);
		return rc;
	}
	switch (rspeed) {
	case IBMVNIC_10MBPS:
		adapter->speed = SPEED_10;
		break;
	case IBMVNIC_100MBPS:
		adapter->speed = SPEED_100;
		break;
	case IBMVNIC_1GBPS:
		adapter->speed = SPEED_1000;
		break;
	case IBMVNIC_10GBPS:
		adapter->speed = SPEED_10000;
		break;
	case IBMVNIC_25GBPS:
		adapter->speed = SPEED_25000;
		break;
	case IBMVNIC_40GBPS:
		adapter->speed = SPEED_40000;
		break;
	case IBMVNIC_50GBPS:
		adapter->speed = SPEED_50000;
		break;
	case IBMVNIC_100GBPS:
		adapter->speed = SPEED_100000;
		break;
	case IBMVNIC_200GBPS:
		adapter->speed = SPEED_200000;
		break;
	default:
		if (netif_carrier_ok(netdev))
			netdev_warn(netdev, "Unknown speed 0x%08x\n", rspeed);
		adapter->speed = SPEED_UNKNOWN;
	}
	if (crq->query_phys_parms_rsp.flags1 & IBMVNIC_FULL_DUPLEX)
		adapter->duplex = DUPLEX_FULL;
	else if (crq->query_phys_parms_rsp.flags1 & IBMVNIC_HALF_DUPLEX)
		adapter->duplex = DUPLEX_HALF;
	else
		adapter->duplex = DUPLEX_UNKNOWN;

	return rc;
}

static void ibmvnic_handle_crq(union ibmvnic_crq *crq,
			       struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_generic_crq *gen_crq = &crq->generic;
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	u64 *u64_crq = (u64 *)crq;
	long rc;

	netdev_dbg(netdev, "Handling CRQ: %016lx %016lx\n",
		   (unsigned long)cpu_to_be64(u64_crq[0]),
		   (unsigned long)cpu_to_be64(u64_crq[1]));
	switch (gen_crq->first) {
	case IBMVNIC_CRQ_INIT_RSP:
		switch (gen_crq->cmd) {
		case IBMVNIC_CRQ_INIT:
			dev_info(dev, "Partner initialized\n");
			adapter->from_passive_init = true;
			/* Discard any stale login responses from prev reset.
			 * CHECK: should we clear even on INIT_COMPLETE?
			 */
			adapter->login_pending = false;

			if (adapter->state == VNIC_DOWN)
				rc = ibmvnic_reset(adapter, VNIC_RESET_PASSIVE_INIT);
			else
				rc = ibmvnic_reset(adapter, VNIC_RESET_FAILOVER);

			if (rc && rc != -EBUSY) {
				/* We were unable to schedule the failover
				 * reset either because the adapter was still
				 * probing (eg: during kexec) or we could not
				 * allocate memory. Clear the failover_pending
				 * flag since no one else will. We ignore
				 * EBUSY because it means either FAILOVER reset
				 * is already scheduled or the adapter is
				 * being removed.
				 */
				netdev_err(netdev,
					   "Error %ld scheduling failover reset\n",
					   rc);
				adapter->failover_pending = false;
			}

			if (!completion_done(&adapter->init_done)) {
				if (!adapter->init_done_rc)
					adapter->init_done_rc = -EAGAIN;
				complete(&adapter->init_done);
			}

			break;
		case IBMVNIC_CRQ_INIT_COMPLETE:
			dev_info(dev, "Partner initialization complete\n");
			adapter->crq.active = true;
			send_version_xchg(adapter);
			break;
		default:
			dev_err(dev, "Unknown crq cmd: %d\n", gen_crq->cmd);
		}
		return;
	case IBMVNIC_CRQ_XPORT_EVENT:
		netif_carrier_off(netdev);
		adapter->crq.active = false;
		/* terminate any thread waiting for a response
		 * from the device
		 */
		if (!completion_done(&adapter->fw_done)) {
			adapter->fw_done_rc = -EIO;
			complete(&adapter->fw_done);
		}

		/* if we got here during crq-init, retry crq-init */
		if (!completion_done(&adapter->init_done)) {
			adapter->init_done_rc = -EAGAIN;
			complete(&adapter->init_done);
		}

		if (!completion_done(&adapter->stats_done))
			complete(&adapter->stats_done);
		if (test_bit(0, &adapter->resetting))
			adapter->force_reset_recovery = true;
		if (gen_crq->cmd == IBMVNIC_PARTITION_MIGRATED) {
			dev_info(dev, "Migrated, re-enabling adapter\n");
			ibmvnic_reset(adapter, VNIC_RESET_MOBILITY);
		} else if (gen_crq->cmd == IBMVNIC_DEVICE_FAILOVER) {
			dev_info(dev, "Backing device failover detected\n");
			adapter->failover_pending = true;
		} else {
			/* The adapter lost the connection */
			dev_err(dev, "Virtual Adapter failed (rc=%d)\n",
				gen_crq->cmd);
			ibmvnic_reset(adapter, VNIC_RESET_FATAL);
		}
		return;
	case IBMVNIC_CRQ_CMD_RSP:
		break;
	default:
		dev_err(dev, "Got an invalid msg type 0x%02x\n",
			gen_crq->first);
		return;
	}

	switch (gen_crq->cmd) {
	case VERSION_EXCHANGE_RSP:
		rc = crq->version_exchange_rsp.rc.code;
		if (rc) {
			dev_err(dev, "Error %ld in VERSION_EXCHG_RSP\n", rc);
			break;
		}
		ibmvnic_version =
			    be16_to_cpu(crq->version_exchange_rsp.version);
		dev_info(dev, "Partner protocol version is %d\n",
			 ibmvnic_version);
		send_query_cap(adapter);
		break;
	case QUERY_CAPABILITY_RSP:
		handle_query_cap_rsp(crq, adapter);
		break;
	case QUERY_MAP_RSP:
		handle_query_map_rsp(crq, adapter);
		break;
	case REQUEST_MAP_RSP:
		adapter->fw_done_rc = crq->request_map_rsp.rc.code;
		complete(&adapter->fw_done);
		break;
	case REQUEST_UNMAP_RSP:
		handle_request_unmap_rsp(crq, adapter);
		break;
	case REQUEST_CAPABILITY_RSP:
		handle_request_cap_rsp(crq, adapter);
		break;
	case LOGIN_RSP:
		netdev_dbg(netdev, "Got Login Response\n");
		handle_login_rsp(crq, adapter);
		break;
	case LOGICAL_LINK_STATE_RSP:
		netdev_dbg(netdev,
			   "Got Logical Link State Response, state: %d rc: %d\n",
			   crq->logical_link_state_rsp.link_state,
			   crq->logical_link_state_rsp.rc.code);
		adapter->logical_link_state =
		    crq->logical_link_state_rsp.link_state;
		adapter->init_done_rc = crq->logical_link_state_rsp.rc.code;
		complete(&adapter->init_done);
		break;
	case LINK_STATE_INDICATION:
		netdev_dbg(netdev, "Got Logical Link State Indication\n");
		adapter->phys_link_state =
		    crq->link_state_indication.phys_link_state;
		adapter->logical_link_state =
		    crq->link_state_indication.logical_link_state;
		if (adapter->phys_link_state && adapter->logical_link_state)
			netif_carrier_on(netdev);
		else
			netif_carrier_off(netdev);
		break;
	case CHANGE_MAC_ADDR_RSP:
		netdev_dbg(netdev, "Got MAC address change Response\n");
		adapter->fw_done_rc = handle_change_mac_rsp(crq, adapter);
		break;
	case ERROR_INDICATION:
		netdev_dbg(netdev, "Got Error Indication\n");
		handle_error_indication(crq, adapter);
		break;
	case REQUEST_STATISTICS_RSP:
		netdev_dbg(netdev, "Got Statistics Response\n");
		complete(&adapter->stats_done);
		break;
	case QUERY_IP_OFFLOAD_RSP:
		netdev_dbg(netdev, "Got Query IP offload Response\n");
		handle_query_ip_offload_rsp(adapter);
		break;
	case MULTICAST_CTRL_RSP:
		netdev_dbg(netdev, "Got multicast control Response\n");
		break;
	case CONTROL_IP_OFFLOAD_RSP:
		netdev_dbg(netdev, "Got Control IP offload Response\n");
		dma_unmap_single(dev, adapter->ip_offload_ctrl_tok,
				 sizeof(adapter->ip_offload_ctrl),
				 DMA_TO_DEVICE);
		complete(&adapter->init_done);
		break;
	case COLLECT_FW_TRACE_RSP:
		netdev_dbg(netdev, "Got Collect firmware trace Response\n");
		complete(&adapter->fw_done);
		break;
	case GET_VPD_SIZE_RSP:
		handle_vpd_size_rsp(crq, adapter);
		break;
	case GET_VPD_RSP:
		handle_vpd_rsp(crq, adapter);
		break;
	case QUERY_PHYS_PARMS_RSP:
		adapter->fw_done_rc = handle_query_phys_parms_rsp(crq, adapter);
		complete(&adapter->fw_done);
		break;
	default:
		netdev_err(netdev, "Got an invalid cmd type 0x%02x\n",
			   gen_crq->cmd);
	}
}

static irqreturn_t ibmvnic_interrupt(int irq, void *instance)
{
	struct ibmvnic_adapter *adapter = instance;

	tasklet_schedule(&adapter->tasklet);
	return IRQ_HANDLED;
}

static void ibmvnic_tasklet(struct tasklet_struct *t)
{
	struct ibmvnic_adapter *adapter = from_tasklet(adapter, t, tasklet);
	struct ibmvnic_crq_queue *queue = &adapter->crq;
	union ibmvnic_crq *crq;
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);

	/* Pull all the valid messages off the CRQ */
	while ((crq = ibmvnic_next_crq(adapter)) != NULL) {
		/* This barrier makes sure ibmvnic_next_crq()'s
		 * crq->generic.first & IBMVNIC_CRQ_CMD_RSP is loaded
		 * before ibmvnic_handle_crq()'s
		 * switch(gen_crq->first) and switch(gen_crq->cmd).
		 */
		dma_rmb();
		ibmvnic_handle_crq(crq, adapter);
		crq->generic.first = 0;
	}

	spin_unlock_irqrestore(&queue->lock, flags);
}

static int ibmvnic_reenable_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct vio_dev *vdev = adapter->vdev;
	int rc;

	do {
		rc = plpar_hcall_norets(H_ENABLE_CRQ, vdev->unit_address);
	} while (rc == H_IN_PROGRESS || rc == H_BUSY || H_IS_LONG_BUSY(rc));

	if (rc)
		dev_err(&vdev->dev, "Error enabling adapter (rc=%d)\n", rc);

	return rc;
}

static int ibmvnic_reset_crq(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct device *dev = &adapter->vdev->dev;
	struct vio_dev *vdev = adapter->vdev;
	int rc;

	/* Close the CRQ */
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

	/* Clean out the queue */
	if (!crq->msgs)
		return -EINVAL;

	memset(crq->msgs, 0, PAGE_SIZE);
	crq->cur = 0;
	crq->active = false;

	/* And re-open it again */
	rc = plpar_hcall_norets(H_REG_CRQ, vdev->unit_address,
				crq->msg_token, PAGE_SIZE);

	if (rc == H_CLOSED)
		/* Adapter is good, but other end is not ready */
		dev_warn(dev, "Partner adapter not ready\n");
	else if (rc != 0)
		dev_warn(dev, "Couldn't register crq (rc=%d)\n", rc);

	return rc;
}

static void release_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct vio_dev *vdev = adapter->vdev;
	long rc;

	if (!crq->msgs)
		return;

	netdev_dbg(adapter->netdev, "Releasing CRQ\n");
	free_irq(vdev->irq, adapter);
	tasklet_kill(&adapter->tasklet);
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

	dma_unmap_single(&vdev->dev, crq->msg_token, PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
	free_page((unsigned long)crq->msgs);
	crq->msgs = NULL;
	crq->active = false;
}

static int init_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct device *dev = &adapter->vdev->dev;
	struct vio_dev *vdev = adapter->vdev;
	int rc, retrc = -ENOMEM;

	if (crq->msgs)
		return 0;

	crq->msgs = (union ibmvnic_crq *)get_zeroed_page(GFP_KERNEL);
	/* Should we allocate more than one page? */

	if (!crq->msgs)
		return -ENOMEM;

	crq->size = PAGE_SIZE / sizeof(*crq->msgs);
	crq->msg_token = dma_map_single(dev, crq->msgs, PAGE_SIZE,
					DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, crq->msg_token))
		goto map_failed;

	rc = plpar_hcall_norets(H_REG_CRQ, vdev->unit_address,
				crq->msg_token, PAGE_SIZE);

	if (rc == H_RESOURCE)
		/* maybe kexecing and resource is busy. try a reset */
		rc = ibmvnic_reset_crq(adapter);
	retrc = rc;

	if (rc == H_CLOSED) {
		dev_warn(dev, "Partner adapter not ready\n");
	} else if (rc) {
		dev_warn(dev, "Error %d opening adapter\n", rc);
		goto reg_crq_failed;
	}

	retrc = 0;

	tasklet_setup(&adapter->tasklet, (void *)ibmvnic_tasklet);

	netdev_dbg(adapter->netdev, "registering irq 0x%x\n", vdev->irq);
	snprintf(crq->name, sizeof(crq->name), "ibmvnic-%x",
		 adapter->vdev->unit_address);
	rc = request_irq(vdev->irq, ibmvnic_interrupt, 0, crq->name, adapter);
	if (rc) {
		dev_err(dev, "Couldn't register irq 0x%x. rc=%d\n",
			vdev->irq, rc);
		goto req_irq_failed;
	}

	rc = vio_enable_interrupts(vdev);
	if (rc) {
		dev_err(dev, "Error %d enabling interrupts\n", rc);
		goto req_irq_failed;
	}

	crq->cur = 0;
	spin_lock_init(&crq->lock);

	/* process any CRQs that were queued before we enabled interrupts */
	tasklet_schedule(&adapter->tasklet);

	return retrc;

req_irq_failed:
	tasklet_kill(&adapter->tasklet);
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));
reg_crq_failed:
	dma_unmap_single(dev, crq->msg_token, PAGE_SIZE, DMA_BIDIRECTIONAL);
map_failed:
	free_page((unsigned long)crq->msgs);
	crq->msgs = NULL;
	return retrc;
}

static int ibmvnic_reset_init(struct ibmvnic_adapter *adapter, bool reset)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long timeout = msecs_to_jiffies(20000);
	u64 old_num_rx_queues = adapter->req_rx_queues;
	u64 old_num_tx_queues = adapter->req_tx_queues;
	int rc;

	adapter->from_passive_init = false;

	rc = ibmvnic_send_crq_init(adapter);
	if (rc) {
		dev_err(dev, "Send crq init failed with error %d\n", rc);
		return rc;
	}

	if (!wait_for_completion_timeout(&adapter->init_done, timeout)) {
		dev_err(dev, "Initialization sequence timed out\n");
		return -ETIMEDOUT;
	}

	if (adapter->init_done_rc) {
		release_crq_queue(adapter);
		dev_err(dev, "CRQ-init failed, %d\n", adapter->init_done_rc);
		return adapter->init_done_rc;
	}

	if (adapter->from_passive_init) {
		adapter->state = VNIC_OPEN;
		adapter->from_passive_init = false;
		dev_err(dev, "CRQ-init failed, passive-init\n");
		return -EINVAL;
	}

	if (reset &&
	    test_bit(0, &adapter->resetting) && !adapter->wait_for_reset &&
	    adapter->reset_reason != VNIC_RESET_MOBILITY) {
		if (adapter->req_rx_queues != old_num_rx_queues ||
		    adapter->req_tx_queues != old_num_tx_queues) {
			release_sub_crqs(adapter, 0);
			rc = init_sub_crqs(adapter);
		} else {
			/* no need to reinitialize completely, but we do
			 * need to clean up transmits that were in flight
			 * when we processed the reset.  Failure to do so
			 * will confound the upper layer, usually TCP, by
			 * creating the illusion of transmits that are
			 * awaiting completion.
			 */
			clean_tx_pools(adapter);

			rc = reset_sub_crq_queues(adapter);
		}
	} else {
		rc = init_sub_crqs(adapter);
	}

	if (rc) {
		dev_err(dev, "Initialization of sub crqs failed\n");
		release_crq_queue(adapter);
		return rc;
	}

	rc = init_sub_crq_irqs(adapter);
	if (rc) {
		dev_err(dev, "Failed to initialize sub crq irqs\n");
		release_crq_queue(adapter);
	}

	return rc;
}

static struct device_attribute dev_attr_failover;

static int ibmvnic_probe(struct vio_dev *dev, const struct vio_device_id *id)
{
	struct ibmvnic_adapter *adapter;
	struct net_device *netdev;
	unsigned char *mac_addr_p;
	unsigned long flags;
	bool init_success;
	int rc;

	dev_dbg(&dev->dev, "entering ibmvnic_probe for UA 0x%x\n",
		dev->unit_address);

	mac_addr_p = (unsigned char *)vio_get_attribute(dev,
							VETH_MAC_ADDR, NULL);
	if (!mac_addr_p) {
		dev_err(&dev->dev,
			"(%s:%3.3d) ERROR: Can't find MAC_ADDR attribute\n",
			__FILE__, __LINE__);
		return 0;
	}

	netdev = alloc_etherdev_mq(sizeof(struct ibmvnic_adapter),
				   IBMVNIC_MAX_QUEUES);
	if (!netdev)
		return -ENOMEM;

	adapter = netdev_priv(netdev);
	adapter->state = VNIC_PROBING;
	dev_set_drvdata(&dev->dev, netdev);
	adapter->vdev = dev;
	adapter->netdev = netdev;
	adapter->login_pending = false;
	memset(&adapter->map_ids, 0, sizeof(adapter->map_ids));
	/* map_ids start at 1, so ensure map_id 0 is always "in-use" */
	bitmap_set(adapter->map_ids, 0, 1);

	ether_addr_copy(adapter->mac_addr, mac_addr_p);
	eth_hw_addr_set(netdev, adapter->mac_addr);
	netdev->irq = dev->irq;
	netdev->netdev_ops = &ibmvnic_netdev_ops;
	netdev->ethtool_ops = &ibmvnic_ethtool_ops;
	SET_NETDEV_DEV(netdev, &dev->dev);

	INIT_WORK(&adapter->ibmvnic_reset, __ibmvnic_reset);
	INIT_DELAYED_WORK(&adapter->ibmvnic_delayed_reset,
			  __ibmvnic_delayed_reset);
	INIT_LIST_HEAD(&adapter->rwi_list);
	spin_lock_init(&adapter->rwi_lock);
	spin_lock_init(&adapter->state_lock);
	mutex_init(&adapter->fw_lock);
	init_completion(&adapter->probe_done);
	init_completion(&adapter->init_done);
	init_completion(&adapter->fw_done);
	init_completion(&adapter->reset_done);
	init_completion(&adapter->stats_done);
	clear_bit(0, &adapter->resetting);
	adapter->prev_rx_buf_sz = 0;
	adapter->prev_mtu = 0;

	init_success = false;
	do {
		reinit_init_done(adapter);

		/* clear any failovers we got in the previous pass
		 * since we are reinitializing the CRQ
		 */
		adapter->failover_pending = false;

		/* If we had already initialized CRQ, we may have one or
		 * more resets queued already. Discard those and release
		 * the CRQ before initializing the CRQ again.
		 */
		release_crq_queue(adapter);

		/* Since we are still in PROBING state, __ibmvnic_reset()
		 * will not access the ->rwi_list and since we released CRQ,
		 * we won't get _new_ transport events. But there maybe an
		 * ongoing ibmvnic_reset() call. So serialize access to
		 * rwi_list. If we win the race, ibvmnic_reset() could add
		 * a reset after we purged but thats ok - we just may end
		 * up with an extra reset (i.e similar to having two or more
		 * resets in the queue at once).
		 * CHECK.
		 */
		spin_lock_irqsave(&adapter->rwi_lock, flags);
		flush_reset_queue(adapter);
		spin_unlock_irqrestore(&adapter->rwi_lock, flags);

		rc = init_crq_queue(adapter);
		if (rc) {
			dev_err(&dev->dev, "Couldn't initialize crq. rc=%d\n",
				rc);
			goto ibmvnic_init_fail;
		}

		rc = ibmvnic_reset_init(adapter, false);
	} while (rc == -EAGAIN);

	/* We are ignoring the error from ibmvnic_reset_init() assuming that the
	 * partner is not ready. CRQ is not active. When the partner becomes
	 * ready, we will do the passive init reset.
	 */

	if (!rc)
		init_success = true;

	rc = init_stats_buffers(adapter);
	if (rc)
		goto ibmvnic_init_fail;

	rc = init_stats_token(adapter);
	if (rc)
		goto ibmvnic_stats_fail;

	rc = device_create_file(&dev->dev, &dev_attr_failover);
	if (rc)
		goto ibmvnic_dev_file_err;

	netif_carrier_off(netdev);

	if (init_success) {
		adapter->state = VNIC_PROBED;
		netdev->mtu = adapter->req_mtu - ETH_HLEN;
		netdev->min_mtu = adapter->min_mtu - ETH_HLEN;
		netdev->max_mtu = adapter->max_mtu - ETH_HLEN;
	} else {
		adapter->state = VNIC_DOWN;
	}

	adapter->wait_for_reset = false;
	adapter->last_reset_time = jiffies;

	rc = register_netdev(netdev);
	if (rc) {
		dev_err(&dev->dev, "failed to register netdev rc=%d\n", rc);
		goto ibmvnic_register_fail;
	}
	dev_info(&dev->dev, "ibmvnic registered\n");

	rc = ibmvnic_cpu_notif_add(adapter);
	if (rc) {
		netdev_err(netdev, "Registering cpu notifier failed\n");
		goto cpu_notif_add_failed;
	}

	complete(&adapter->probe_done);

	return 0;

cpu_notif_add_failed:
	unregister_netdev(netdev);

ibmvnic_register_fail:
	device_remove_file(&dev->dev, &dev_attr_failover);

ibmvnic_dev_file_err:
	release_stats_token(adapter);

ibmvnic_stats_fail:
	release_stats_buffers(adapter);

ibmvnic_init_fail:
	release_sub_crqs(adapter, 1);
	release_crq_queue(adapter);

	/* cleanup worker thread after releasing CRQ so we don't get
	 * transport events (i.e new work items for the worker thread).
	 */
	adapter->state = VNIC_REMOVING;
	complete(&adapter->probe_done);
	flush_work(&adapter->ibmvnic_reset);
	flush_delayed_work(&adapter->ibmvnic_delayed_reset);

	flush_reset_queue(adapter);

	mutex_destroy(&adapter->fw_lock);
	free_netdev(netdev);

	return rc;
}

static void ibmvnic_remove(struct vio_dev *dev)
{
	struct net_device *netdev = dev_get_drvdata(&dev->dev);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	unsigned long flags;

	spin_lock_irqsave(&adapter->state_lock, flags);

	/* If ibmvnic_reset() is scheduling a reset, wait for it to
	 * finish. Then, set the state to REMOVING to prevent it from
	 * scheduling any more work and to have reset functions ignore
	 * any resets that have already been scheduled. Drop the lock
	 * after setting state, so __ibmvnic_reset() which is called
	 * from the flush_work() below, can make progress.
	 */
	spin_lock(&adapter->rwi_lock);
	adapter->state = VNIC_REMOVING;
	spin_unlock(&adapter->rwi_lock);

	spin_unlock_irqrestore(&adapter->state_lock, flags);

	ibmvnic_cpu_notif_remove(adapter);

	flush_work(&adapter->ibmvnic_reset);
	flush_delayed_work(&adapter->ibmvnic_delayed_reset);

	rtnl_lock();
	unregister_netdevice(netdev);

	release_resources(adapter);
	release_rx_pools(adapter);
	release_tx_pools(adapter);
	release_sub_crqs(adapter, 1);
	release_crq_queue(adapter);

	release_stats_token(adapter);
	release_stats_buffers(adapter);

	adapter->state = VNIC_REMOVED;

	rtnl_unlock();
	mutex_destroy(&adapter->fw_lock);
	device_remove_file(&dev->dev, &dev_attr_failover);
	free_netdev(netdev);
	dev_set_drvdata(&dev->dev, NULL);
}

static ssize_t failover_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	__be64 session_token;
	long rc;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	rc = plpar_hcall(H_VIOCTL, retbuf, adapter->vdev->unit_address,
			 H_GET_SESSION_TOKEN, 0, 0, 0);
	if (rc) {
		netdev_err(netdev, "Couldn't retrieve session token, rc %ld\n",
			   rc);
		goto last_resort;
	}

	session_token = (__be64)retbuf[0];
	netdev_dbg(netdev, "Initiating client failover, session id %llx\n",
		   be64_to_cpu(session_token));
	rc = plpar_hcall_norets(H_VIOCTL, adapter->vdev->unit_address,
				H_SESSION_ERR_DETECTED, session_token, 0, 0);
	if (rc) {
		netdev_err(netdev,
			   "H_VIOCTL initiated failover failed, rc %ld\n",
			   rc);
		goto last_resort;
	}

	return count;

last_resort:
	netdev_dbg(netdev, "Trying to send CRQ_CMD, the last resort\n");
	ibmvnic_reset(adapter, VNIC_RESET_FAILOVER);

	return count;
}
static DEVICE_ATTR_WO(failover);

static unsigned long ibmvnic_get_desired_dma(struct vio_dev *vdev)
{
	struct net_device *netdev = dev_get_drvdata(&vdev->dev);
	struct ibmvnic_adapter *adapter;
	struct iommu_table *tbl;
	unsigned long ret = 0;
	int i;

	tbl = get_iommu_table_base(&vdev->dev);

	/* netdev inits at probe time along with the structures we need below*/
	if (!netdev)
		return IOMMU_PAGE_ALIGN(IBMVNIC_IO_ENTITLEMENT_DEFAULT, tbl);

	adapter = netdev_priv(netdev);

	ret += PAGE_SIZE; /* the crq message queue */
	ret += IOMMU_PAGE_ALIGN(sizeof(struct ibmvnic_statistics), tbl);

	for (i = 0; i < adapter->req_tx_queues + adapter->req_rx_queues; i++)
		ret += 4 * PAGE_SIZE; /* the scrq message queue */

	for (i = 0; i < adapter->num_active_rx_pools; i++)
		ret += adapter->rx_pool[i].size *
		    IOMMU_PAGE_ALIGN(adapter->rx_pool[i].buff_size, tbl);

	return ret;
}

static int ibmvnic_resume(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	if (adapter->state != VNIC_OPEN)
		return 0;

	tasklet_schedule(&adapter->tasklet);

	return 0;
}

static const struct vio_device_id ibmvnic_device_table[] = {
	{"network", "IBM,vnic"},
	{"", "" }
};
MODULE_DEVICE_TABLE(vio, ibmvnic_device_table);

static const struct dev_pm_ops ibmvnic_pm_ops = {
	.resume = ibmvnic_resume
};

static struct vio_driver ibmvnic_driver = {
	.id_table       = ibmvnic_device_table,
	.probe          = ibmvnic_probe,
	.remove         = ibmvnic_remove,
	.get_desired_dma = ibmvnic_get_desired_dma,
	.name		= ibmvnic_driver_name,
	.pm		= &ibmvnic_pm_ops,
};

/* module functions */
static int __init ibmvnic_module_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "net/ibmvnic:online",
				      ibmvnic_cpu_online,
				      ibmvnic_cpu_down_prep);
	if (ret < 0)
		goto out;
	ibmvnic_online = ret;
	ret = cpuhp_setup_state_multi(CPUHP_IBMVNIC_DEAD, "net/ibmvnic:dead",
				      NULL, ibmvnic_cpu_dead);
	if (ret)
		goto err_dead;

	ret = vio_register_driver(&ibmvnic_driver);
	if (ret)
		goto err_vio_register;

	pr_info("%s: %s %s\n", ibmvnic_driver_name, ibmvnic_driver_string,
		IBMVNIC_DRIVER_VERSION);

	return 0;
err_vio_register:
	cpuhp_remove_multi_state(CPUHP_IBMVNIC_DEAD);
err_dead:
	cpuhp_remove_multi_state(ibmvnic_online);
out:
	return ret;
}

static void __exit ibmvnic_module_exit(void)
{
	vio_unregister_driver(&ibmvnic_driver);
	cpuhp_remove_multi_state(CPUHP_IBMVNIC_DEAD);
	cpuhp_remove_multi_state(ibmvnic_online);
}

module_init(ibmvnic_module_init);
module_exit(ibmvnic_module_exit);
