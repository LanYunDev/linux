/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  pm_wakeup.h - Power management wakeup interface
 *
 *  Copyright (C) 2008 Alan Stern
 *  Copyright (C) 2010 Rafael J. Wysocki, Novell Inc.
 */

#ifndef _LINUX_PM_WAKEUP_H
#define _LINUX_PM_WAKEUP_H

#ifndef _DEVICE_H_
# error "Please do not include this file directly."
#endif

#include <linux/types.h>

struct wake_irq;

/**
 * struct wakeup_source - Representation of wakeup sources
 *
 * @name: Name of the wakeup source
 * @id: Wakeup source id
 * @entry: Wakeup source list entry
 * @lock: Wakeup source lock
 * @wakeirq: Optional device specific wakeirq
 * @timer: Wakeup timer list
 * @timer_expires: Wakeup timer expiration
 * @total_time: Total time this wakeup source has been active.
 * @max_time: Maximum time this wakeup source has been continuously active.
 * @last_time: Monotonic clock when the wakeup source's was touched last time.
 * @prevent_sleep_time: Total time this source has been preventing autosleep.
 * @event_count: Number of signaled wakeup events.
 * @active_count: Number of times the wakeup source was activated.
 * @relax_count: Number of times the wakeup source was deactivated.
 * @expire_count: Number of times the wakeup source's timeout has expired.
 * @wakeup_count: Number of times the wakeup source might abort suspend.
 * @dev: Struct device for sysfs statistics about the wakeup source.
 * @active: Status of the wakeup source.
 * @autosleep_enabled: Autosleep is active, so update @prevent_sleep_time.
 */
struct wakeup_source {
	const char 		*name;
	int			id;
	struct list_head	entry;
	spinlock_t		lock;
	struct wake_irq		*wakeirq;
	struct timer_list	timer;
	unsigned long		timer_expires;
	ktime_t total_time;
	ktime_t max_time;
	ktime_t last_time;
	ktime_t start_prevent_time;
	ktime_t prevent_sleep_time;
	unsigned long		event_count;
	unsigned long		active_count;
	unsigned long		relax_count;
	unsigned long		expire_count;
	unsigned long		wakeup_count;
	struct device		*dev;
	bool			active:1;
	bool			autosleep_enabled:1;
};

#define for_each_wakeup_source(ws) \
	for ((ws) = wakeup_sources_walk_start();	\
	     (ws);					\
	     (ws) = wakeup_sources_walk_next((ws)))

#ifdef CONFIG_PM_SLEEP

/*
 * Changes to device_may_wakeup take effect on the next pm state change.
 */

static inline bool device_can_wakeup(struct device *dev)
{
	return dev->power.can_wakeup;
}

static inline bool device_may_wakeup(struct device *dev)
{
	return dev->power.can_wakeup && !!dev->power.wakeup;
}

static inline bool device_wakeup_path(struct device *dev)
{
	return dev->power.wakeup_path;
}

static inline void device_set_wakeup_path(struct device *dev)
{
	dev->power.wakeup_path = true;
}

/* drivers/base/power/wakeup.c */
extern struct wakeup_source *wakeup_source_register(struct device *dev,
						    const char *name);
extern void wakeup_source_unregister(struct wakeup_source *ws);
extern int wakeup_sources_read_lock(void);
extern void wakeup_sources_read_unlock(int idx);
extern struct wakeup_source *wakeup_sources_walk_start(void);
extern struct wakeup_source *wakeup_sources_walk_next(struct wakeup_source *ws);
extern int device_wakeup_enable(struct device *dev);
extern void device_wakeup_disable(struct device *dev);
extern void device_set_wakeup_capable(struct device *dev, bool capable);
extern int device_set_wakeup_enable(struct device *dev, bool enable);
extern void __pm_stay_awake(struct wakeup_source *ws);
extern void pm_stay_awake(struct device *dev);
extern void __pm_relax(struct wakeup_source *ws);
extern void pm_relax(struct device *dev);
extern void pm_wakeup_ws_event(struct wakeup_source *ws, unsigned int msec, bool hard);
extern void pm_wakeup_dev_event(struct device *dev, unsigned int msec, bool hard);

#else /* !CONFIG_PM_SLEEP */

static inline void device_set_wakeup_capable(struct device *dev, bool capable)
{
	dev->power.can_wakeup = capable;
}

static inline bool device_can_wakeup(struct device *dev)
{
	return dev->power.can_wakeup;
}

static inline struct wakeup_source *wakeup_source_register(struct device *dev,
							   const char *name)
{
	return NULL;
}

static inline void wakeup_source_unregister(struct wakeup_source *ws) {}

static inline int device_wakeup_enable(struct device *dev)
{
	dev->power.should_wakeup = true;
	return 0;
}

static inline void device_wakeup_disable(struct device *dev)
{
	dev->power.should_wakeup = false;
}

static inline int device_set_wakeup_enable(struct device *dev, bool enable)
{
	dev->power.should_wakeup = enable;
	return 0;
}

static inline bool device_may_wakeup(struct device *dev)
{
	return dev->power.can_wakeup && dev->power.should_wakeup;
}

static inline bool device_wakeup_path(struct device *dev)
{
	return false;
}

static inline void device_set_wakeup_path(struct device *dev) {}

static inline void __pm_stay_awake(struct wakeup_source *ws) {}

static inline void pm_stay_awake(struct device *dev) {}

static inline void __pm_relax(struct wakeup_source *ws) {}

static inline void pm_relax(struct device *dev) {}

static inline void pm_wakeup_ws_event(struct wakeup_source *ws,
				      unsigned int msec, bool hard) {}

static inline void pm_wakeup_dev_event(struct device *dev, unsigned int msec,
				       bool hard) {}

#endif /* !CONFIG_PM_SLEEP */

static inline bool device_awake_path(struct device *dev)
{
	return device_wakeup_path(dev);
}

static inline void device_set_awake_path(struct device *dev)
{
	device_set_wakeup_path(dev);
}

static inline void __pm_wakeup_event(struct wakeup_source *ws, unsigned int msec)
{
	pm_wakeup_ws_event(ws, msec, false);
}

static inline void pm_wakeup_event(struct device *dev, unsigned int msec)
{
	pm_wakeup_dev_event(dev, msec, false);
}

static inline void pm_wakeup_hard_event(struct device *dev)
{
	pm_wakeup_dev_event(dev, 0, true);
}

/**
 * device_init_wakeup - Device wakeup initialization.
 * @dev: Device to handle.
 * @enable: Whether or not to enable @dev as a wakeup device.
 *
 * By default, most devices should leave wakeup disabled.  The exceptions are
 * devices that everyone expects to be wakeup sources: keyboards, power buttons,
 * possibly network interfaces, etc.  Also, devices that don't generate their
 * own wakeup requests but merely forward requests from one bus to another
 * (like PCI bridges) should have wakeup enabled by default.
 */
static inline int device_init_wakeup(struct device *dev, bool enable)
{
	if (enable) {
		device_set_wakeup_capable(dev, true);
		return device_wakeup_enable(dev);
	}
	device_wakeup_disable(dev);
	device_set_wakeup_capable(dev, false);
	return 0;
}

static void device_disable_wakeup(void *dev)
{
	device_init_wakeup(dev, false);
}

/**
 * devm_device_init_wakeup - Resource managed device wakeup initialization.
 * @dev: Device to handle.
 *
 * This function is the devm managed version of device_init_wakeup(dev, true).
 */
static inline int devm_device_init_wakeup(struct device *dev)
{
	device_init_wakeup(dev, true);
	return devm_add_action_or_reset(dev, device_disable_wakeup, dev);
}

#endif /* _LINUX_PM_WAKEUP_H */
