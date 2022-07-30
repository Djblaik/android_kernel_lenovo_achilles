#ifndef _LINUX_HALL_H
#define _LINUX_HALL_H

#include <linux/kgdb.h>


#include <linux/fs.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/list.h>
#include <linux/backlight.h>
#include <linux/slab.h>
#include <asm/io.h>

#define HALL_EVENT_REPORT                  0xf

/*	NEAR BY */ 
#define HALL_EVENT_NEAR_BY		0x00
/*FAR AWAY*/
#define HALL_EVENT_FAR_AWAY		0x01

struct hall_event {
	void *data;
};

extern int hall_register_client(struct notifier_block *nb);
extern int hall_unregister_client(struct notifier_block *nb);
extern int hall_notifier_call_chain(unsigned long val, void *v);

#endif /* _LINUX_HALL_H */
