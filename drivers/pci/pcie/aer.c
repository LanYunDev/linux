// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the AER root port service driver. The driver registers an IRQ
 * handler. When a root port triggers an AER interrupt, the IRQ handler
 * collects Root Port status and schedules work.
 *
 * Copyright (C) 2006 Intel Corp.
 *	Tom Long Nguyen (tom.l.nguyen@intel.com)
 *	Zhang Yanmin (yanmin.zhang@intel.com)
 *
 * (C) Copyright 2009 Hewlett-Packard Development Company, L.P.
 *    Andrew Patterson <andrew.patterson@hp.com>
 */

#define pr_fmt(fmt) "AER: " fmt
#define dev_fmt pr_fmt

#include <linux/bitops.h>
#include <linux/cper.h>
#include <linux/dev_printk.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <acpi/apei.h>
#include <acpi/ghes.h>
#include <ras/ras_event.h>

#include "../pci.h"
#include "portdrv.h"

#define aer_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)

#define AER_ERROR_SOURCES_MAX		128

#define AER_MAX_TYPEOF_COR_ERRS		16	/* as per PCI_ERR_COR_STATUS */
#define AER_MAX_TYPEOF_UNCOR_ERRS	27	/* as per PCI_ERR_UNCOR_STATUS*/

struct aer_err_source {
	u32 status;			/* PCI_ERR_ROOT_STATUS */
	u32 id;				/* PCI_ERR_ROOT_ERR_SRC */
};

struct aer_rpc {
	struct pci_dev *rpd;		/* Root Port device */
	DECLARE_KFIFO(aer_fifo, struct aer_err_source, AER_ERROR_SOURCES_MAX);
};

/* AER info for the device */
struct aer_info {

	/*
	 * Fields for all AER capable devices. They indicate the errors
	 * "as seen by this device". Note that this may mean that if an
	 * Endpoint is causing problems, the AER counters may increment
	 * at its link partner (e.g. Root Port) because the errors will be
	 * "seen" by the link partner and not the problematic Endpoint
	 * itself (which may report all counters as 0 as it never saw any
	 * problems).
	 */
	/* Counters for different type of correctable errors */
	u64 dev_cor_errs[AER_MAX_TYPEOF_COR_ERRS];
	/* Counters for different type of fatal uncorrectable errors */
	u64 dev_fatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Counters for different type of nonfatal uncorrectable errors */
	u64 dev_nonfatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Total number of ERR_COR sent by this device */
	u64 dev_total_cor_errs;
	/* Total number of ERR_FATAL sent by this device */
	u64 dev_total_fatal_errs;
	/* Total number of ERR_NONFATAL sent by this device */
	u64 dev_total_nonfatal_errs;

	/*
	 * Fields for Root Ports & Root Complex Event Collectors only; these
	 * indicate the total number of ERR_COR, ERR_FATAL, and ERR_NONFATAL
	 * messages received by the Root Port / Event Collector, INCLUDING the
	 * ones that are generated internally (by the Root Port itself)
	 */
	u64 rootport_total_cor_errs;
	u64 rootport_total_fatal_errs;
	u64 rootport_total_nonfatal_errs;

	/* Ratelimits for errors */
	struct ratelimit_state correctable_ratelimit;
	struct ratelimit_state nonfatal_ratelimit;
};

#define AER_LOG_TLP_MASKS		(PCI_ERR_UNC_POISON_TLP|	\
					PCI_ERR_UNC_ECRC|		\
					PCI_ERR_UNC_UNSUP|		\
					PCI_ERR_UNC_COMP_ABORT|		\
					PCI_ERR_UNC_UNX_COMP|		\
					PCI_ERR_UNC_MALF_TLP)

#define SYSTEM_ERROR_INTR_ON_MESG_MASK	(PCI_EXP_RTCTL_SECEE|	\
					PCI_EXP_RTCTL_SENFEE|	\
					PCI_EXP_RTCTL_SEFEE)
#define ROOT_PORT_INTR_ON_MESG_MASK	(PCI_ERR_ROOT_CMD_COR_EN|	\
					PCI_ERR_ROOT_CMD_NONFATAL_EN|	\
					PCI_ERR_ROOT_CMD_FATAL_EN)
#define ERR_COR_ID(d)			(d & 0xffff)
#define ERR_UNCOR_ID(d)			(d >> 16)

#define AER_ERR_STATUS_MASK		(PCI_ERR_ROOT_UNCOR_RCV |	\
					PCI_ERR_ROOT_COR_RCV |		\
					PCI_ERR_ROOT_MULTI_COR_RCV |	\
					PCI_ERR_ROOT_MULTI_UNCOR_RCV)

static bool pcie_aer_disable;
static pci_ers_result_t aer_root_reset(struct pci_dev *dev);

void pci_no_aer(void)
{
	pcie_aer_disable = true;
}

bool pci_aer_available(void)
{
	return !pcie_aer_disable && pci_msi_enabled();
}

#ifdef CONFIG_PCIE_ECRC

#define ECRC_POLICY_DEFAULT 0		/* ECRC set by BIOS */
#define ECRC_POLICY_OFF     1		/* ECRC off for performance */
#define ECRC_POLICY_ON      2		/* ECRC on for data integrity */

static int ecrc_policy = ECRC_POLICY_DEFAULT;

static const char * const ecrc_policy_str[] = {
	[ECRC_POLICY_DEFAULT] = "bios",
	[ECRC_POLICY_OFF] = "off",
	[ECRC_POLICY_ON] = "on"
};

/**
 * enable_ecrc_checking - enable PCIe ECRC checking for a device
 * @dev: the PCI device
 *
 * Return: 0 on success, or negative on failure.
 */
static int enable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	if (reg32 & PCI_ERR_CAP_ECRC_GENC)
		reg32 |= PCI_ERR_CAP_ECRC_GENE;
	if (reg32 & PCI_ERR_CAP_ECRC_CHKC)
		reg32 |= PCI_ERR_CAP_ECRC_CHKE;
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
}

/**
 * disable_ecrc_checking - disable PCIe ECRC checking for a device
 * @dev: the PCI device
 *
 * Return: 0 on success, or negative on failure.
 */
static int disable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	reg32 &= ~(PCI_ERR_CAP_ECRC_GENE | PCI_ERR_CAP_ECRC_CHKE);
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
}

/**
 * pcie_set_ecrc_checking - set/unset PCIe ECRC checking for a device based
 * on global policy
 * @dev: the PCI device
 */
void pcie_set_ecrc_checking(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev))
		return;

	switch (ecrc_policy) {
	case ECRC_POLICY_DEFAULT:
		return;
	case ECRC_POLICY_OFF:
		disable_ecrc_checking(dev);
		break;
	case ECRC_POLICY_ON:
		enable_ecrc_checking(dev);
		break;
	default:
		return;
	}
}

/**
 * pcie_ecrc_get_policy - parse kernel command-line ecrc option
 * @str: ECRC policy from kernel command line to use
 */
void pcie_ecrc_get_policy(char *str)
{
	int i;

	i = match_string(ecrc_policy_str, ARRAY_SIZE(ecrc_policy_str), str);
	if (i < 0)
		return;

	ecrc_policy = i;
}
#endif	/* CONFIG_PCIE_ECRC */

#define	PCI_EXP_AER_FLAGS	(PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE | \
				 PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE)

int pcie_aer_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	if (!dev->aer_cap)
		return 0;

	return pcie_ports_native || host->native_aer;
}
EXPORT_SYMBOL_NS_GPL(pcie_aer_is_native, "CXL");

static int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
	int rc;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
	return pcibios_err_to_errno(rc);
}

int pci_aer_clear_nonfatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* Clear status bits for ERR_NONFATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= ~sev;
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_aer_clear_nonfatal_status);

void pci_aer_clear_fatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return;

	/* Clear status bits for ERR_FATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= sev;
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);
}

/**
 * pci_aer_raw_clear_status - Clear AER error registers.
 * @dev: the PCI device
 *
 * Clear AER error status registers unconditionally, regardless of
 * whether they're owned by firmware or the OS.
 *
 * Return: 0 on success, or negative on failure.
 */
int pci_aer_raw_clear_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 status;
	int port_type;

	if (!aer)
		return -EIO;

	port_type = pci_pcie_type(dev);
	if (port_type == PCI_EXP_TYPE_ROOT_PORT ||
	    port_type == PCI_EXP_TYPE_RC_EC) {
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, &status);
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, status);
	}

	pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, status);

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}

int pci_aer_clear_status(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev))
		return -EIO;

	return pci_aer_raw_clear_status(dev);
}

void pci_save_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!aer)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, cap++);
	if (pcie_cap_has_rtctl(dev))
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, cap++);
}

void pci_restore_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!aer)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, *cap++);
	if (pcie_cap_has_rtctl(dev))
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, *cap++);
}

void pci_aer_init(struct pci_dev *dev)
{
	int n;

	dev->aer_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	if (!dev->aer_cap)
		return;

	dev->aer_info = kzalloc(sizeof(*dev->aer_info), GFP_KERNEL);

	ratelimit_state_init(&dev->aer_info->correctable_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	ratelimit_state_init(&dev->aer_info->nonfatal_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

	/*
	 * We save/restore PCI_ERR_UNCOR_MASK, PCI_ERR_UNCOR_SEVER,
	 * PCI_ERR_COR_MASK, and PCI_ERR_CAP.  Root and Root Complex Event
	 * Collectors also implement PCI_ERR_ROOT_COMMAND (PCIe r6.0, sec
	 * 7.8.4.9).
	 */
	n = pcie_cap_has_rtctl(dev) ? 5 : 4;
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_ERR, sizeof(u32) * n);

	pci_aer_clear_status(dev);

	if (pci_aer_available())
		pci_enable_pcie_error_reporting(dev);

	pcie_set_ecrc_checking(dev);
}

void pci_aer_exit(struct pci_dev *dev)
{
	kfree(dev->aer_info);
	dev->aer_info = NULL;
}

#define AER_AGENT_RECEIVER		0
#define AER_AGENT_REQUESTER		1
#define AER_AGENT_COMPLETER		2
#define AER_AGENT_TRANSMITTER		3

#define AER_AGENT_REQUESTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : (PCI_ERR_UNC_COMP_TIME|PCI_ERR_UNC_UNSUP))
#define AER_AGENT_COMPLETER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : PCI_ERR_UNC_COMP_ABORT)
#define AER_AGENT_TRANSMITTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_REP_ROLL|PCI_ERR_COR_REP_TIMER) : 0)

#define AER_GET_AGENT(t, e)						\
	((e & AER_AGENT_COMPLETER_MASK(t)) ? AER_AGENT_COMPLETER :	\
	(e & AER_AGENT_REQUESTER_MASK(t)) ? AER_AGENT_REQUESTER :	\
	(e & AER_AGENT_TRANSMITTER_MASK(t)) ? AER_AGENT_TRANSMITTER :	\
	AER_AGENT_RECEIVER)

#define AER_PHYSICAL_LAYER_ERROR	0
#define AER_DATA_LINK_LAYER_ERROR	1
#define AER_TRANSACTION_LAYER_ERROR	2

#define AER_PHYSICAL_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	PCI_ERR_COR_RCVR : 0)
#define AER_DATA_LINK_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_BAD_TLP|						\
	PCI_ERR_COR_BAD_DLLP|						\
	PCI_ERR_COR_REP_ROLL|						\
	PCI_ERR_COR_REP_TIMER) : PCI_ERR_UNC_DLP)

#define AER_GET_LAYER_ERROR(t, e)					\
	((e & AER_PHYSICAL_LAYER_ERROR_MASK(t)) ? AER_PHYSICAL_LAYER_ERROR : \
	(e & AER_DATA_LINK_LAYER_ERROR_MASK(t)) ? AER_DATA_LINK_LAYER_ERROR : \
	AER_TRANSACTION_LAYER_ERROR)

/*
 * AER error strings
 */
static const char * const aer_error_severity_string[] = {
	"Uncorrectable (Non-Fatal)",
	"Uncorrectable (Fatal)",
	"Correctable"
};

static const char *aer_error_layer[] = {
	"Physical Layer",
	"Data Link Layer",
	"Transaction Layer"
};

static const char *aer_correctable_error_string[] = {
	"RxErr",			/* Bit Position 0	*/
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"BadTLP",			/* Bit Position 6	*/
	"BadDLLP",			/* Bit Position 7	*/
	"Rollover",			/* Bit Position 8	*/
	NULL,
	NULL,
	NULL,
	"Timeout",			/* Bit Position 12	*/
	"NonFatalErr",			/* Bit Position 13	*/
	"CorrIntErr",			/* Bit Position 14	*/
	"HeaderOF",			/* Bit Position 15	*/
	NULL,				/* Bit Position 16	*/
	NULL,				/* Bit Position 17	*/
	NULL,				/* Bit Position 18	*/
	NULL,				/* Bit Position 19	*/
	NULL,				/* Bit Position 20	*/
	NULL,				/* Bit Position 21	*/
	NULL,				/* Bit Position 22	*/
	NULL,				/* Bit Position 23	*/
	NULL,				/* Bit Position 24	*/
	NULL,				/* Bit Position 25	*/
	NULL,				/* Bit Position 26	*/
	NULL,				/* Bit Position 27	*/
	NULL,				/* Bit Position 28	*/
	NULL,				/* Bit Position 29	*/
	NULL,				/* Bit Position 30	*/
	NULL,				/* Bit Position 31	*/
};

static const char *aer_uncorrectable_error_string[] = {
	"Undefined",			/* Bit Position 0	*/
	NULL,
	NULL,
	NULL,
	"DLP",				/* Bit Position 4	*/
	"SDES",				/* Bit Position 5	*/
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"TLP",				/* Bit Position 12	*/
	"FCP",				/* Bit Position 13	*/
	"CmpltTO",			/* Bit Position 14	*/
	"CmpltAbrt",			/* Bit Position 15	*/
	"UnxCmplt",			/* Bit Position 16	*/
	"RxOF",				/* Bit Position 17	*/
	"MalfTLP",			/* Bit Position 18	*/
	"ECRC",				/* Bit Position 19	*/
	"UnsupReq",			/* Bit Position 20	*/
	"ACSViol",			/* Bit Position 21	*/
	"UncorrIntErr",			/* Bit Position 22	*/
	"BlockedTLP",			/* Bit Position 23	*/
	"AtomicOpBlocked",		/* Bit Position 24	*/
	"TLPBlockedErr",		/* Bit Position 25	*/
	"PoisonTLPBlocked",		/* Bit Position 26	*/
	NULL,				/* Bit Position 27	*/
	NULL,				/* Bit Position 28	*/
	NULL,				/* Bit Position 29	*/
	NULL,				/* Bit Position 30	*/
	NULL,				/* Bit Position 31	*/
};

static const char *aer_agent_string[] = {
	"Receiver ID",
	"Requester ID",
	"Completer ID",
	"Transmitter ID"
};

#define aer_stats_dev_attr(name, stats_array, strings_array,		\
			   total_string, total_field)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		     char *buf)						\
{									\
	unsigned int i;							\
	struct pci_dev *pdev = to_pci_dev(dev);				\
	u64 *stats = pdev->aer_info->stats_array;			\
	size_t len = 0;							\
									\
	for (i = 0; i < ARRAY_SIZE(pdev->aer_info->stats_array); i++) {	\
		if (strings_array[i])					\
			len += sysfs_emit_at(buf, len, "%s %llu\n",	\
					     strings_array[i],		\
					     stats[i]);			\
		else if (stats[i])					\
			len += sysfs_emit_at(buf, len,			\
					     #stats_array "_bit[%d] %llu\n",\
					     i, stats[i]);		\
	}								\
	len += sysfs_emit_at(buf, len, "TOTAL_%s %llu\n", total_string,	\
			     pdev->aer_info->total_field);		\
	return len;							\
}									\
static DEVICE_ATTR_RO(name)

aer_stats_dev_attr(aer_dev_correctable, dev_cor_errs,
		   aer_correctable_error_string, "ERR_COR",
		   dev_total_cor_errs);
aer_stats_dev_attr(aer_dev_fatal, dev_fatal_errs,
		   aer_uncorrectable_error_string, "ERR_FATAL",
		   dev_total_fatal_errs);
aer_stats_dev_attr(aer_dev_nonfatal, dev_nonfatal_errs,
		   aer_uncorrectable_error_string, "ERR_NONFATAL",
		   dev_total_nonfatal_errs);

#define aer_stats_rootport_attr(name, field)				\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		     char *buf)						\
{									\
	struct pci_dev *pdev = to_pci_dev(dev);				\
	return sysfs_emit(buf, "%llu\n", pdev->aer_info->field);	\
}									\
static DEVICE_ATTR_RO(name)

aer_stats_rootport_attr(aer_rootport_total_err_cor,
			 rootport_total_cor_errs);
aer_stats_rootport_attr(aer_rootport_total_err_fatal,
			 rootport_total_fatal_errs);
aer_stats_rootport_attr(aer_rootport_total_err_nonfatal,
			 rootport_total_nonfatal_errs);

static struct attribute *aer_stats_attrs[] __ro_after_init = {
	&dev_attr_aer_dev_correctable.attr,
	&dev_attr_aer_dev_fatal.attr,
	&dev_attr_aer_dev_nonfatal.attr,
	&dev_attr_aer_rootport_total_err_cor.attr,
	&dev_attr_aer_rootport_total_err_fatal.attr,
	&dev_attr_aer_rootport_total_err_nonfatal.attr,
	NULL
};

static umode_t aer_stats_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_info)
		return 0;

	if ((a == &dev_attr_aer_rootport_total_err_cor.attr ||
	     a == &dev_attr_aer_rootport_total_err_fatal.attr ||
	     a == &dev_attr_aer_rootport_total_err_nonfatal.attr) &&
	    ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) &&
	     (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_EC)))
		return 0;

	return a->mode;
}

const struct attribute_group aer_stats_attr_group = {
	.attrs  = aer_stats_attrs,
	.is_visible = aer_stats_attrs_are_visible,
};

/*
 * Ratelimit interval
 * <=0: disabled with ratelimit.interval = 0
 * >0: enabled with ratelimit.interval in ms
 */
#define aer_ratelimit_interval_attr(name, ratelimit)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
					 char *buf)			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.interval);	\
	}								\
									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr, \
		     const char *buf, size_t count) 			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int interval;						\
									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
									\
		if (kstrtoint(buf, 0, &interval) < 0)			\
			return -EINVAL;					\
									\
		if (interval <= 0)					\
			interval = 0;					\
		else							\
			interval = msecs_to_jiffies(interval); 		\
									\
		pdev->aer_info->ratelimit.interval = interval;		\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name);

#define aer_ratelimit_burst_attr(name, ratelimit)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		    char *buf)						\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.burst);	\
	}								\
									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr,	\
		     const char *buf, size_t count)			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int burst;						\
									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
									\
		if (kstrtoint(buf, 0, &burst) < 0)			\
			return -EINVAL;					\
									\
		pdev->aer_info->ratelimit.burst = burst;		\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name);

#define aer_ratelimit_attrs(name)					\
	aer_ratelimit_interval_attr(name##_ratelimit_interval_ms,	\
				    name##_ratelimit)			\
	aer_ratelimit_burst_attr(name##_ratelimit_burst,		\
				 name##_ratelimit)

aer_ratelimit_attrs(correctable)
aer_ratelimit_attrs(nonfatal)

static struct attribute *aer_attrs[] = {
	&dev_attr_correctable_ratelimit_interval_ms.attr,
	&dev_attr_correctable_ratelimit_burst.attr,
	&dev_attr_nonfatal_ratelimit_interval_ms.attr,
	&dev_attr_nonfatal_ratelimit_burst.attr,
	NULL
};

static umode_t aer_attrs_are_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_info)
		return 0;

	return a->mode;
}

const struct attribute_group aer_attr_group = {
	.name = "aer",
	.attrs = aer_attrs,
	.is_visible = aer_attrs_are_visible,
};

static void pci_dev_aer_stats_incr(struct pci_dev *pdev,
				   struct aer_err_info *info)
{
	unsigned long status = info->status & ~info->mask;
	int i, max = -1;
	u64 *counter = NULL;
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	switch (info->severity) {
	case AER_CORRECTABLE:
		aer_info->dev_total_cor_errs++;
		counter = &aer_info->dev_cor_errs[0];
		max = AER_MAX_TYPEOF_COR_ERRS;
		break;
	case AER_NONFATAL:
		aer_info->dev_total_nonfatal_errs++;
		counter = &aer_info->dev_nonfatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	case AER_FATAL:
		aer_info->dev_total_fatal_errs++;
		counter = &aer_info->dev_fatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	}

	for_each_set_bit(i, &status, max)
		counter[i]++;
}

static void pci_rootport_aer_stats_incr(struct pci_dev *pdev,
				 struct aer_err_source *e_src)
{
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	if (e_src->status & PCI_ERR_ROOT_COR_RCV)
		aer_info->rootport_total_cor_errs++;

	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) {
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV)
			aer_info->rootport_total_fatal_errs++;
		else
			aer_info->rootport_total_nonfatal_errs++;
	}
}

static int aer_ratelimit(struct pci_dev *dev, unsigned int severity)
{
	switch (severity) {
	case AER_NONFATAL:
		return __ratelimit(&dev->aer_info->nonfatal_ratelimit);
	case AER_CORRECTABLE:
		return __ratelimit(&dev->aer_info->correctable_ratelimit);
	default:
		return 1;	/* Don't ratelimit fatal errors */
	}
}

static void __aer_print_error(struct pci_dev *dev, struct aer_err_info *info)
{
	const char **strings;
	unsigned long status = info->status & ~info->mask;
	const char *level = info->level;
	const char *errmsg;
	int i;

	if (info->severity == AER_CORRECTABLE)
		strings = aer_correctable_error_string;
	else
		strings = aer_uncorrectable_error_string;

	for_each_set_bit(i, &status, 32) {
		errmsg = strings[i];
		if (!errmsg)
			errmsg = "Unknown Error Bit";

		aer_printk(level, dev, "   [%2d] %-22s%s\n", i, errmsg,
				info->first_error == i ? " (First)" : "");
	}
}

static void aer_print_source(struct pci_dev *dev, struct aer_err_info *info,
			     bool found)
{
	u16 source = info->id;

	pci_info(dev, "%s%s error message received from %04x:%02x:%02x.%d%s\n",
		 info->multi_error_valid ? "Multiple " : "",
		 aer_error_severity_string[info->severity],
		 pci_domain_nr(dev->bus), PCI_BUS_NUM(source),
		 PCI_SLOT(source), PCI_FUNC(source),
		 found ? "" : " (no details found");
}

void aer_print_error(struct aer_err_info *info, int i)
{
	struct pci_dev *dev;
	int layer, agent, id;
	const char *level = info->level;

	if (WARN_ON_ONCE(i >= AER_MAX_MULTI_ERR_DEVICES))
		return;

	dev = info->dev[i];
	id = pci_dev_id(dev);

	pci_dev_aer_stats_incr(dev, info);
	trace_aer_event(pci_name(dev), (info->status & ~info->mask),
			info->severity, info->tlp_header_valid, &info->tlp);

	if (!info->ratelimit_print[i])
		return;

	if (!info->status) {
		pci_err(dev, "PCIe Bus Error: severity=%s, type=Inaccessible, (Unregistered Agent ID)\n",
			aer_error_severity_string[info->severity]);
		goto out;
	}

	layer = AER_GET_LAYER_ERROR(info->severity, info->status);
	agent = AER_GET_AGENT(info->severity, info->status);

	aer_printk(level, dev, "PCIe Bus Error: severity=%s, type=%s, (%s)\n",
		   aer_error_severity_string[info->severity],
		   aer_error_layer[layer], aer_agent_string[agent]);

	aer_printk(level, dev, "  device [%04x:%04x] error status/mask=%08x/%08x\n",
		   dev->vendor, dev->device, info->status, info->mask);

	__aer_print_error(dev, info);

	if (info->tlp_header_valid)
		pcie_print_tlp_log(dev, &info->tlp, level, dev_fmt("  "));

out:
	if (info->id && info->error_dev_num > 1 && info->id == id)
		pci_err(dev, "  Error of this Agent is reported first\n");
}

#ifdef CONFIG_ACPI_APEI_PCIEAER
int cper_severity_to_aer(int cper_severity)
{
	switch (cper_severity) {
	case CPER_SEV_RECOVERABLE:
		return AER_NONFATAL;
	case CPER_SEV_FATAL:
		return AER_FATAL;
	default:
		return AER_CORRECTABLE;
	}
}
EXPORT_SYMBOL_GPL(cper_severity_to_aer);
#endif

void pci_print_aer(struct pci_dev *dev, int aer_severity,
		   struct aer_capability_regs *aer)
{
	int layer, agent, tlp_header_valid = 0;
	u32 status, mask;
	struct aer_err_info info = {
		.severity = aer_severity,
		.first_error = PCI_ERR_CAP_FEP(aer->cap_control),
	};

	if (aer_severity == AER_CORRECTABLE) {
		status = aer->cor_status;
		mask = aer->cor_mask;
		info.level = KERN_WARNING;
	} else {
		status = aer->uncor_status;
		mask = aer->uncor_mask;
		info.level = KERN_ERR;
		tlp_header_valid = status & AER_LOG_TLP_MASKS;
	}

	info.status = status;
	info.mask = mask;

	pci_dev_aer_stats_incr(dev, &info);
	trace_aer_event(pci_name(dev), (status & ~mask),
			aer_severity, tlp_header_valid, &aer->header_log);

	if (!aer_ratelimit(dev, info.severity))
		return;

	layer = AER_GET_LAYER_ERROR(aer_severity, status);
	agent = AER_GET_AGENT(aer_severity, status);

	aer_printk(info.level, dev, "aer_status: 0x%08x, aer_mask: 0x%08x\n",
		   status, mask);
	__aer_print_error(dev, &info);
	aer_printk(info.level, dev, "aer_layer=%s, aer_agent=%s\n",
		   aer_error_layer[layer], aer_agent_string[agent]);

	if (aer_severity != AER_CORRECTABLE)
		aer_printk(info.level, dev, "aer_uncor_severity: 0x%08x\n",
			   aer->uncor_severity);

	if (tlp_header_valid)
		pcie_print_tlp_log(dev, &aer->header_log, info.level,
				   dev_fmt("  "));
}
EXPORT_SYMBOL_NS_GPL(pci_print_aer, "CXL");

/**
 * add_error_device - list device to be handled
 * @e_info: pointer to error info
 * @dev: pointer to pci_dev to be added
 */
static int add_error_device(struct aer_err_info *e_info, struct pci_dev *dev)
{
	int i = e_info->error_dev_num;

	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return -ENOSPC;

	e_info->dev[i] = pci_dev_get(dev);
	e_info->error_dev_num++;

	/*
	 * Ratelimit AER log messages.  "dev" is either the source
	 * identified by the root's Error Source ID or it has an unmasked
	 * error logged in its own AER Capability.  Messages are emitted
	 * when "ratelimit_print[i]" is non-zero.  If we will print detail
	 * for a downstream device, make sure we print the Error Source ID
	 * from the root as well.
	 */
	if (aer_ratelimit(dev, e_info->severity)) {
		e_info->ratelimit_print[i] = 1;
		e_info->root_ratelimit_print = 1;
	}
	return 0;
}

/**
 * is_error_source - check whether the device is source of reported error
 * @dev: pointer to pci_dev to be checked
 * @e_info: pointer to reported error info
 */
static bool is_error_source(struct pci_dev *dev, struct aer_err_info *e_info)
{
	int aer = dev->aer_cap;
	u32 status, mask;
	u16 reg16;

	/*
	 * When bus ID is equal to 0, it might be a bad ID
	 * reported by Root Port.
	 */
	if ((PCI_BUS_NUM(e_info->id) != 0) &&
	    !(dev->bus->bus_flags & PCI_BUS_FLAGS_NO_AERSID)) {
		/* Device ID match? */
		if (e_info->id == pci_dev_id(dev))
			return true;

		/* Continue ID comparing if there is no multiple error */
		if (!e_info->multi_error_valid)
			return false;
	}

	/*
	 * When either
	 *      1) bus ID is equal to 0. Some ports might lose the bus
	 *              ID of error source id;
	 *      2) bus flag PCI_BUS_FLAGS_NO_AERSID is set
	 *      3) There are multiple errors and prior ID comparing fails;
	 * We check AER status registers to find possible reporter.
	 */
	if (atomic_read(&dev->enable_cnt) == 0)
		return false;

	/* Check if AER is enabled */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &reg16);
	if (!(reg16 & PCI_EXP_AER_FLAGS))
		return false;

	if (!aer)
		return false;

	/* Check if error is recorded */
	if (e_info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	} else {
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	}
	if (status & ~mask)
		return true;

	return false;
}

static int find_device_iter(struct pci_dev *dev, void *data)
{
	struct aer_err_info *e_info = (struct aer_err_info *)data;

	if (is_error_source(dev, e_info)) {
		/* List this device */
		if (add_error_device(e_info, dev)) {
			/* We cannot handle more... Stop iteration */
			pci_err(dev, "Exceeded max supported (%d) devices with errors logged\n",
				AER_MAX_MULTI_ERR_DEVICES);
			return 1;
		}

		/* If there is only a single error, stop iteration */
		if (!e_info->multi_error_valid)
			return 1;
	}
	return 0;
}

/**
 * find_source_device - search through device hierarchy for source device
 * @parent: pointer to Root Port pci_dev data structure
 * @e_info: including detailed error information such as ID
 *
 * Return: true if found.
 *
 * Invoked by DPC when error is detected at the Root Port.
 * Caller of this function must set id, severity, and multi_error_valid of
 * struct aer_err_info pointed by @e_info properly.  This function must fill
 * e_info->error_dev_num and e_info->dev[], based on the given information.
 */
static bool find_source_device(struct pci_dev *parent,
			       struct aer_err_info *e_info)
{
	struct pci_dev *dev = parent;
	int result;

	/* Must reset in this function */
	e_info->error_dev_num = 0;

	/* Is Root Port an agent that sends error message? */
	result = find_device_iter(dev, e_info);
	if (result)
		return true;

	if (pci_pcie_type(parent) == PCI_EXP_TYPE_RC_EC)
		pcie_walk_rcec(parent, find_device_iter, e_info);
	else
		pci_walk_bus(parent->subordinate, find_device_iter, e_info);

	if (!e_info->error_dev_num)
		return false;
	return true;
}

#ifdef CONFIG_PCIEAER_CXL

/**
 * pci_aer_unmask_internal_errors - unmask internal errors
 * @dev: pointer to the pci_dev data structure
 *
 * Unmask internal errors in the Uncorrectable and Correctable Error
 * Mask registers.
 *
 * Note: AER must be enabled and supported by the device which must be
 * checked in advance, e.g. with pcie_aer_is_native().
 */
static void pci_aer_unmask_internal_errors(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 mask;

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	mask &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask);

	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	mask &= ~PCI_ERR_COR_INTERNAL;
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask);
}

static bool is_cxl_mem_dev(struct pci_dev *dev)
{
	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	if (dev->devfn != PCI_DEVFN(0, 0))
		return false;

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	if ((dev->class >> 8) != PCI_CLASS_MEMORY_CXL)
		return false;

	return true;
}

static bool cxl_error_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	return (pcie_ports_native || host->native_aer);
}

static bool is_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE)
		return info->status & PCI_ERR_COR_INTERNAL;

	return info->status & PCI_ERR_UNC_INTN;
}

static int cxl_rch_handle_error_iter(struct pci_dev *dev, void *data)
{
	struct aer_err_info *info = (struct aer_err_info *)data;
	const struct pci_error_handlers *err_handler;

	if (!is_cxl_mem_dev(dev) || !cxl_error_is_native(dev))
		return 0;

	/* Protect dev->driver */
	device_lock(&dev->dev);

	err_handler = dev->driver ? dev->driver->err_handler : NULL;
	if (!err_handler)
		goto out;

	if (info->severity == AER_CORRECTABLE) {
		if (err_handler->cor_error_detected)
			err_handler->cor_error_detected(dev);
	} else if (err_handler->error_detected) {
		if (info->severity == AER_NONFATAL)
			err_handler->error_detected(dev, pci_channel_io_normal);
		else if (info->severity == AER_FATAL)
			err_handler->error_detected(dev, pci_channel_io_frozen);
	}
out:
	device_unlock(&dev->dev);
	return 0;
}

static void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info)
{
	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC &&
	    is_internal_error(info))
		pcie_walk_rcec(dev, cxl_rch_handle_error_iter, info);
}

static int handles_cxl_error_iter(struct pci_dev *dev, void *data)
{
	bool *handles_cxl = data;

	if (!*handles_cxl)
		*handles_cxl = is_cxl_mem_dev(dev) && cxl_error_is_native(dev);

	/* Non-zero terminates iteration */
	return *handles_cxl;
}

static bool handles_cxl_errors(struct pci_dev *rcec)
{
	bool handles_cxl = false;

	if (pci_pcie_type(rcec) == PCI_EXP_TYPE_RC_EC &&
	    pcie_aer_is_native(rcec))
		pcie_walk_rcec(rcec, handles_cxl_error_iter, &handles_cxl);

	return handles_cxl;
}

static void cxl_rch_enable_rcec(struct pci_dev *rcec)
{
	if (!handles_cxl_errors(rcec))
		return;

	pci_aer_unmask_internal_errors(rcec);
	pci_info(rcec, "CXL: Internal errors unmasked");
}

#else
static inline void cxl_rch_enable_rcec(struct pci_dev *dev) { }
static inline void cxl_rch_handle_error(struct pci_dev *dev,
					struct aer_err_info *info) { }
#endif

/**
 * pci_aer_handle_error - handle logging error into an event log
 * @dev: pointer to pci_dev data structure of error source device
 * @info: comprehensive error information
 *
 * Invoked when an error being detected by Root Port.
 */
static void pci_aer_handle_error(struct pci_dev *dev, struct aer_err_info *info)
{
	int aer = dev->aer_cap;

	if (info->severity == AER_CORRECTABLE) {
		/*
		 * Correctable error does not need software intervention.
		 * No need to go through error recovery process.
		 */
		if (aer)
			pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS,
					info->status);
		if (pcie_aer_is_native(dev)) {
			struct pci_driver *pdrv = dev->driver;

			if (pdrv && pdrv->err_handler &&
			    pdrv->err_handler->cor_error_detected)
				pdrv->err_handler->cor_error_detected(dev);
			pcie_clear_device_status(dev);
		}
	} else if (info->severity == AER_NONFATAL)
		pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset);
	else if (info->severity == AER_FATAL)
		pcie_do_recovery(dev, pci_channel_io_frozen, aer_root_reset);
}

static void handle_error_source(struct pci_dev *dev, struct aer_err_info *info)
{
	cxl_rch_handle_error(dev, info);
	pci_aer_handle_error(dev, info);
	pci_dev_put(dev);
}

#ifdef CONFIG_ACPI_APEI_PCIEAER

#define AER_RECOVER_RING_SIZE		16

struct aer_recover_entry {
	u8	bus;
	u8	devfn;
	u16	domain;
	int	severity;
	struct aer_capability_regs *regs;
};

static DEFINE_KFIFO(aer_recover_ring, struct aer_recover_entry,
		    AER_RECOVER_RING_SIZE);

static void aer_recover_work_func(struct work_struct *work)
{
	struct aer_recover_entry entry;
	struct pci_dev *pdev;

	while (kfifo_get(&aer_recover_ring, &entry)) {
		pdev = pci_get_domain_bus_and_slot(entry.domain, entry.bus,
						   entry.devfn);
		if (!pdev) {
			pr_err_ratelimited("%04x:%02x:%02x.%x: no pci_dev found\n",
					   entry.domain, entry.bus,
					   PCI_SLOT(entry.devfn),
					   PCI_FUNC(entry.devfn));
			continue;
		}
		pci_print_aer(pdev, entry.severity, entry.regs);

		/*
		 * Memory for aer_capability_regs(entry.regs) is being
		 * allocated from the ghes_estatus_pool to protect it from
		 * overwriting when multiple sections are present in the
		 * error status. Thus free the same after processing the
		 * data.
		 */
		ghes_estatus_pool_region_free((unsigned long)entry.regs,
					    sizeof(struct aer_capability_regs));

		if (entry.severity == AER_NONFATAL)
			pcie_do_recovery(pdev, pci_channel_io_normal,
					 aer_root_reset);
		else if (entry.severity == AER_FATAL)
			pcie_do_recovery(pdev, pci_channel_io_frozen,
					 aer_root_reset);
		pci_dev_put(pdev);
	}
}

/*
 * Mutual exclusion for writers of aer_recover_ring, reader side don't
 * need lock, because there is only one reader and lock is not needed
 * between reader and writer.
 */
static DEFINE_SPINLOCK(aer_recover_ring_lock);
static DECLARE_WORK(aer_recover_work, aer_recover_work_func);

void aer_recover_queue(int domain, unsigned int bus, unsigned int devfn,
		       int severity, struct aer_capability_regs *aer_regs)
{
	struct aer_recover_entry entry = {
		.bus		= bus,
		.devfn		= devfn,
		.domain		= domain,
		.severity	= severity,
		.regs		= aer_regs,
	};

	if (kfifo_in_spinlocked(&aer_recover_ring, &entry, 1,
				 &aer_recover_ring_lock))
		schedule_work(&aer_recover_work);
	else
		pr_err("buffer overflow in recovery for %04x:%02x:%02x.%x\n",
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
}
EXPORT_SYMBOL_GPL(aer_recover_queue);
#endif

/**
 * aer_get_device_error_info - read error status from dev and store it to info
 * @info: pointer to structure to store the error record
 * @i: index into info->dev[]
 *
 * Return: 1 on success, 0 on error.
 *
 * Note that @info is reused among all error devices. Clear fields properly.
 */
int aer_get_device_error_info(struct aer_err_info *info, int i)
{
	struct pci_dev *dev;
	int type, aer;
	u32 aercc;

	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return 0;

	dev = info->dev[i];
	aer = dev->aer_cap;
	type = pci_pcie_type(dev);

	/* Must reset in this function */
	info->status = 0;
	info->tlp_header_valid = 0;

	/* The device might not support AER */
	if (!aer)
		return 0;

	if (info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK,
			&info->mask);
		if (!(info->status & ~info->mask))
			return 0;
	} else if (type == PCI_EXP_TYPE_ROOT_PORT ||
		   type == PCI_EXP_TYPE_RC_EC ||
		   type == PCI_EXP_TYPE_DOWNSTREAM ||
		   info->severity == AER_NONFATAL) {

		/* Link is still healthy for IO reads */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK,
			&info->mask);
		if (!(info->status & ~info->mask))
			return 0;

		/* Get First Error Pointer */
		pci_read_config_dword(dev, aer + PCI_ERR_CAP, &aercc);
		info->first_error = PCI_ERR_CAP_FEP(aercc);

		if (info->status & AER_LOG_TLP_MASKS) {
			info->tlp_header_valid = 1;
			pcie_read_tlp_log(dev, aer + PCI_ERR_HEADER_LOG,
					  aer + PCI_ERR_PREFIX_LOG,
					  aer_tlp_log_len(dev, aercc),
					  aercc & PCI_ERR_CAP_TLP_LOG_FLIT,
					  &info->tlp);
		}
	}

	return 1;
}

static inline void aer_process_err_devices(struct aer_err_info *e_info)
{
	int i;

	/* Report all before handling them, to not lose records by reset etc. */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		if (aer_get_device_error_info(e_info, i))
			aer_print_error(e_info, i);
	}
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		if (aer_get_device_error_info(e_info, i))
			handle_error_source(e_info->dev[i], e_info);
	}
}

/**
 * aer_isr_one_error_type - consume a Correctable or Uncorrectable Error
 *			    detected by Root Port or RCEC
 * @root: pointer to Root Port or RCEC that signaled AER interrupt
 * @info: pointer to AER error info
 */
static void aer_isr_one_error_type(struct pci_dev *root,
				   struct aer_err_info *info)
{
	bool found;

	found = find_source_device(root, info);

	/*
	 * If we're going to log error messages, we've already set
	 * "info->root_ratelimit_print" and "info->ratelimit_print[i]" to
	 * non-zero (which enables printing) because this is either an
	 * ERR_FATAL or we found a device with an error logged in its AER
	 * Capability.
	 *
	 * If we didn't find the Error Source device, at least log the
	 * Requester ID from the ERR_* Message received by the Root Port or
	 * RCEC, ratelimited by the RP or RCEC.
	 */
	if (info->root_ratelimit_print ||
	    (!found && aer_ratelimit(root, info->severity)))
		aer_print_source(root, info, found);

	if (found)
		aer_process_err_devices(info);
}

/**
 * aer_isr_one_error - consume error(s) signaled by an AER interrupt from
 *		       Root Port or RCEC
 * @root: pointer to Root Port or RCEC that signaled AER interrupt
 * @e_src: pointer to an error source
 */
static void aer_isr_one_error(struct pci_dev *root,
			      struct aer_err_source *e_src)
{
	u32 status = e_src->status;

	pci_rootport_aer_stats_incr(root, e_src);

	/*
	 * There is a possibility that both correctable error and
	 * uncorrectable error being logged. Report correctable error first.
	 */
	if (status & PCI_ERR_ROOT_COR_RCV) {
		int multi = status & PCI_ERR_ROOT_MULTI_COR_RCV;
		struct aer_err_info e_info = {
			.id = ERR_COR_ID(e_src->id),
			.severity = AER_CORRECTABLE,
			.level = KERN_WARNING,
			.multi_error_valid = multi ? 1 : 0,
		};

		aer_isr_one_error_type(root, &e_info);
	}

	if (status & PCI_ERR_ROOT_UNCOR_RCV) {
		int fatal = status & PCI_ERR_ROOT_FATAL_RCV;
		int multi = status & PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		struct aer_err_info e_info = {
			.id = ERR_UNCOR_ID(e_src->id),
			.severity = fatal ? AER_FATAL : AER_NONFATAL,
			.level = KERN_ERR,
			.multi_error_valid = multi ? 1 : 0,
		};

		aer_isr_one_error_type(root, &e_info);
	}
}

/**
 * aer_isr - consume errors detected by Root Port
 * @irq: IRQ assigned to Root Port
 * @context: pointer to Root Port data structure
 *
 * Invoked, as DPC, when Root Port records new detected error
 */
static irqreturn_t aer_isr(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	struct aer_rpc *rpc = get_service_data(dev);
	struct aer_err_source e_src;

	if (kfifo_is_empty(&rpc->aer_fifo))
		return IRQ_NONE;

	while (kfifo_get(&rpc->aer_fifo, &e_src))
		aer_isr_one_error(rpc->rpd, &e_src);
	return IRQ_HANDLED;
}

/**
 * aer_irq - Root Port's ISR
 * @irq: IRQ assigned to Root Port
 * @context: pointer to Root Port data structure
 *
 * Invoked when Root Port detects AER messages.
 */
static irqreturn_t aer_irq(int irq, void *context)
{
	struct pcie_device *pdev = (struct pcie_device *)context;
	struct aer_rpc *rpc = get_service_data(pdev);
	struct pci_dev *rp = rpc->rpd;
	int aer = rp->aer_cap;
	struct aer_err_source e_src = {};

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, &e_src.status);
	if (!(e_src.status & AER_ERR_STATUS_MASK))
		return IRQ_NONE;

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_ERR_SRC, &e_src.id);
	pci_write_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, e_src.status);

	if (!kfifo_put(&rpc->aer_fifo, e_src))
		return IRQ_HANDLED;

	return IRQ_WAKE_THREAD;
}

static void aer_enable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Enable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 |= ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

static void aer_disable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Disable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 &= ~ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

/**
 * aer_enable_rootport - enable Root Port's interrupts when receiving messages
 * @rpc: pointer to a Root Port data structure
 *
 * Invoked when PCIe bus loads AER service driver.
 */
static void aer_enable_rootport(struct aer_rpc *rpc)
{
	struct pci_dev *pdev = rpc->rpd;
	int aer = pdev->aer_cap;
	u16 reg16;
	u32 reg32;

	/* Clear PCIe Capability's Device Status */
	pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &reg16);
	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, reg16);

	/* Disable system error generation in response to error messages */
	pcie_capability_clear_word(pdev, PCI_EXP_RTCTL,
				   SYSTEM_ERROR_INTR_ON_MESG_MASK);

	/* Clear error status */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);
	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, reg32);
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, reg32);

	aer_enable_irq(pdev);
}

/**
 * aer_disable_rootport - disable Root Port's interrupts when receiving messages
 * @rpc: pointer to a Root Port data structure
 *
 * Invoked when PCIe bus unloads AER service driver.
 */
static void aer_disable_rootport(struct aer_rpc *rpc)
{
	struct pci_dev *pdev = rpc->rpd;
	int aer = pdev->aer_cap;
	u32 reg32;

	aer_disable_irq(pdev);

	/* Clear Root's error status reg */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);
}

/**
 * aer_remove - clean up resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus unloads or AER probe fails.
 */
static void aer_remove(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
}

/**
 * aer_probe - initialize resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus loads AER service driver.
 */
static int aer_probe(struct pcie_device *dev)
{
	int status;
	struct aer_rpc *rpc;
	struct device *device = &dev->device;
	struct pci_dev *port = dev->port;

	BUILD_BUG_ON(ARRAY_SIZE(aer_correctable_error_string) <
		     AER_MAX_TYPEOF_COR_ERRS);
	BUILD_BUG_ON(ARRAY_SIZE(aer_uncorrectable_error_string) <
		     AER_MAX_TYPEOF_UNCOR_ERRS);

	/* Limit to Root Ports or Root Complex Event Collectors */
	if ((pci_pcie_type(port) != PCI_EXP_TYPE_RC_EC) &&
	    (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT))
		return -ENODEV;

	rpc = devm_kzalloc(device, sizeof(struct aer_rpc), GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;

	rpc->rpd = port;
	INIT_KFIFO(rpc->aer_fifo);
	set_service_data(dev, rpc);

	status = devm_request_threaded_irq(device, dev->irq, aer_irq, aer_isr,
					   IRQF_SHARED, "aerdrv", dev);
	if (status) {
		pci_err(port, "request AER IRQ %d failed\n", dev->irq);
		return status;
	}

	cxl_rch_enable_rcec(port);
	aer_enable_rootport(rpc);
	pci_info(port, "enabled with IRQ %d\n", dev->irq);
	return 0;
}

static int aer_suspend(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
	return 0;
}

static int aer_resume(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev);

	aer_enable_rootport(rpc);
	return 0;
}

/**
 * aer_root_reset - reset Root Port hierarchy, RCEC, or RCiEP
 * @dev: pointer to Root Port, RCEC, or RCiEP
 *
 * Invoked by Port Bus driver when performing reset.
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);
	struct pci_dev *root;
	int aer;
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	u32 reg32;
	int rc;

	/*
	 * Only Root Ports and RCECs have AER Root Command and Root Status
	 * registers.  If "dev" is an RCiEP, the relevant registers are in
	 * the RCEC.
	 */
	if (type == PCI_EXP_TYPE_RC_END)
		root = dev->rcec;
	else
		root = pcie_find_root_port(dev);

	/*
	 * If the platform retained control of AER, an RCiEP may not have
	 * an RCEC visible to us, so dev->rcec ("root") may be NULL.  In
	 * that case, firmware is responsible for these registers.
	 */
	aer = root ? root->aer_cap : 0;

	if ((host->native_aer || pcie_ports_native) && aer)
		aer_disable_irq(root);

	if (type == PCI_EXP_TYPE_RC_EC || type == PCI_EXP_TYPE_RC_END) {
		rc = pcie_reset_flr(dev, PCI_RESET_DO_RESET);
		if (!rc)
			pci_info(dev, "has been reset\n");
		else
			pci_info(dev, "not reset (no FLR support: %d)\n", rc);
	} else {
		rc = pci_bus_error_reset(dev);
		pci_info(dev, "%s Port link has been reset (%d)\n",
			pci_is_root_bus(dev->bus) ? "Root" : "Downstream", rc);
	}

	if ((host->native_aer || pcie_ports_native) && aer) {
		/* Clear Root Error Status */
		pci_read_config_dword(root, aer + PCI_ERR_ROOT_STATUS, &reg32);
		pci_write_config_dword(root, aer + PCI_ERR_ROOT_STATUS, reg32);

		aer_enable_irq(root);
	}

	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static struct pcie_port_service_driver aerdriver = {
	.name		= "aer",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_AER,

	.probe		= aer_probe,
	.suspend	= aer_suspend,
	.resume		= aer_resume,
	.remove		= aer_remove,
};

/**
 * pcie_aer_init - register AER service driver
 *
 * Invoked when AER service driver is loaded.
 */
int __init pcie_aer_init(void)
{
	if (!pci_aer_available())
		return -ENXIO;
	return pcie_port_service_register(&aerdriver);
}
