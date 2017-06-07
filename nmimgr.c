/*
 * Copyright (C) 2017 Adrien Mahieux <adrien.mahieux@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

/*
 * Kernel Revision history:
 * 2.6.32: Using notifier_block structs
 * 3.2   : Moved NMI descriptions to an enum: LOCAL, UNKNOWN, MAX
 *         https://lwn.net/Articles/461215/
 *         https://lkml.org/lkml/2012/3/8/386
 * 3.5   : Moved register_nmi_handler to macro+static struct nmiaction fn##_na
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/nmi.h>

#include <asm/nmi.h>
#include <asm/x86_init.h>

/* Compatibility management */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
#include <linux/notifier.h>
#include <linux/kdebug.h>    /* For NMI_DIE and NMI_DIE_IPI */

#include <asm/kdebug.h>

/* HANDLED=1, OK=1, DONE=0  */
#define NMI_HANDLED NOTIFY_OK
#define NMI_DONE    NOTIFY_DONE
#endif


#define NMIMGR_VERSION  "0.4"
#define NMIMGR_NAME     "nmimgr"
#define NMIMGR_NBMAX    256

enum {
	OP_IGNORE=0,
	OP_DROP,
	OP_DEBUG,
	OP_PANIC
};


static int   events_arr[4][NMIMGR_NBMAX];
static char *events_ignore;
static char *events_debug;
static char *events_drop;
static char *events_panic;



/**
 * Handler
 */
static int __nmimgr_handle(unsigned int type, unsigned char reason,
			struct pt_regs *regs)
{

	int i;

	/* ignored NMI */
	for (i = 1; i < NMIMGR_NBMAX; i++) {
		if (reason == events_arr[OP_IGNORE][i])
			return NMI_DONE;
	}


	pr_notice(NMIMGR_NAME": Handling new NMI type:%u event:0x%02x (%d)\n",
		type, reason, reason);

	/* Debugging NMI */
	for (i = 1; i < NMIMGR_NBMAX; i++ ) {
		if (reason == events_arr[OP_DEBUG][i]) {
			pr_notice(NMIMGR_NAME": Debug NMI");
			dump_stack();

#ifndef MODULE
			/* show_regs is not exported */
			show_regs(regs);
#endif
		}
	}


	/* dropped NMI */
	for (i = 1; i < NMIMGR_NBMAX; i++) {

		if (reason == events_arr[OP_DROP][i]) {
			pr_notice(NMIMGR_NAME": Drop NMI event:0x%02x (%d)\n",
				reason, reason);
			return NMI_HANDLED;
		}
	}

	/* Panic NMI */
	for (i = 1; i < NMIMGR_NBMAX; i++) {

		if (reason == events_arr[OP_PANIC][i]) {
			pr_emerg(NMIMGR_NAME": Panic on Event:0x%02x(%d)\n",
				reason, reason);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
			nmi_panic(regs, NMIMGR_NAME": Hit explicit panic");
#else
			panic(NMIMGR_NAME": Hit explicit panic");
#endif

		}
	}

	/* Still there: unmanaged NMI Code. Send to other handlers */
	pr_notice(NMIMGR_NAME": Unmanaged NMI event:0x%02x (%d), let it pass\n",
		reason, reason);

	return NMI_DONE;
}




/***** Kernel < 3.2 **********************************************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)

static int nmimgr_handle(struct notifier_block *nb, unsigned long val,
			void *data)
{
	struct die_args *args = (struct die_args *)data;
	unsigned char reason = args->err;

	/* Only process NMI cases */
	switch (val) {
	case DIE_NMI:
	case DIE_NMIWATCHDOG:
	case DIE_NMI_IPI:
	case DIE_NMIUNKNOWN:
		return __nmimgr_handle(1, reason, args->regs);

	default:
		break;
	}

	return NOTIFY_OK;
}


static struct notifier_block nmimgr_notifier = {
	.notifier_call = nmimgr_handle,
	.priority = 0x7FFFFFFF
};


/**
 * Handler registration
 */
static int nmimgr_register(void)
{
	int ret = register_die_notifier(&nmimgr_notifier);

	if (ret) {
		pr_warn(NMIMGR_NAME": Unable to register NMI handler\n");
		return ret;
	}

	pr_notice(NMIMGR_NAME": Registered handler\n");

	return 0;
}

/**
 * Handler unregistration
 */
static void nmimgr_unregister(void)
{
	unregister_die_notifier(&nmimgr_notifier);
}

/***** Kernel 3.2+ ***********************************************************/
#else

static int nmimgr_handle(unsigned int type, struct pt_regs *regs)
{
	return __nmimgr_handle(type, x86_platform.get_nmi_reason(), regs);
}



/**
 * handler registration
 */
static int nmimgr_register(void)
{
	int ret = 0;
	int i;

	/* A loop wont work because of Macro rewriting of
	 * register_nmi_handler (https://lkml.org/lkml/2012/3/8/386)
	 */

	/* We register our handler first, as we only manage a specific list */
	ret = register_nmi_handler(
	    NMI_UNKNOWN, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warn(NMIMGR_NAME ": Unable to register NMI_UNKNOWN\n");
		i = NMI_UNKNOWN-1;
		goto err;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	ret = register_nmi_handler(
		NMI_SERR, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warn(NMIMGR_NAME ": Unable to register NMI_SERR\n");
		i = NMI_SERR-1;
		goto err;
	}
	ret = register_nmi_handler(
		NMI_IO_CHECK, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warn(NMIMGR_NAME ": Unable to register NMI_IO_CHECK\n");
		i = NMI_IO_CHECK-1;
		goto err;
	}
#endif


	return 0;

err:
	for (; i > 0; i--)
		unregister_nmi_handler(i, NMIMGR_NAME);

	return ret;
}

/**
 * Handler unregistration
 */
static void nmimgr_unregister(void)
{
	int i;

	for (i = NMI_MAX-1; i > NMI_LOCAL; i--)
		unregister_nmi_handler(i, NMIMGR_NAME);

}

#endif

/*****************************************************************************/

static int __init __nmimgr_setup(int op, char *str)
{
	char *ret = 0;

	if (!str)
		return 1;

	/* lib/cmdline.c: Extract int list from str into events_panic_list[] */
	ret = get_options(str, ARRAY_SIZE(events_arr[op]),
		events_arr[op]);

	if (ret && *ret != 0) {
		pr_err(NMIMGR_NAME": Invalid input '%s', ret:%s\n", str, ret);
		return 0;
	}
	return 1;
}


/**
 * Parse the input string
 */
static int __init nmimgr_setup_panic(char *str)
{
	if (!str)
		return 1;

	pr_info(NMIMGR_NAME ": events_panic: %s\n", str);
	return __nmimgr_setup(OP_PANIC, str);
}
__setup("nmimgr.events_panic=", nmimgr_setup_panic);


/**
 *
 */
static int __init nmimgr_setup_debug(char *str)
{
	if (!str)
		return 1;

	pr_info(NMIMGR_NAME ": events_debug: %s\n", str);
	return __nmimgr_setup(OP_DEBUG, str);
}
__setup("nmimgr.events_debug=", nmimgr_setup_debug);


/**
 *
 */
static int __init nmimgr_setup_ignore(char *str)
{
	if (!str)
		return 1;

	pr_info(NMIMGR_NAME ": events_ignore: %s\n", str);
	return __nmimgr_setup(OP_IGNORE, str);
}
__setup("nmimgr.events_ignore=", nmimgr_setup_ignore);

/**
 *
 */
static int __init nmimgr_setup_drop(char *str)
{
	if (!str)
		return 1;

	pr_info(NMIMGR_NAME ": events_drop: %s\n", str);
	return __nmimgr_setup(OP_DROP, str);
}
__setup("nmimgr.events_drop=", nmimgr_setup_drop);



/**
 * Module initialization
 */
int __init init_module(void)
{
	int err;

	pr_notice(NMIMGR_NAME ": Loaded module v%s\n", NMIMGR_VERSION);

	nmimgr_setup_ignore(events_ignore);
	nmimgr_setup_debug(events_debug);
	nmimgr_setup_panic(events_panic);
	nmimgr_setup_drop(events_drop);

	err = nmimgr_register();
	if (err) {
		pr_warn(NMIMGR_NAME": NMI Management not available\n");
		return err;
	}
	return 0;
}
/* module_init(init_module); */

/**
 * Module unloading
 */
void __exit clean_module(void)
{
	nmimgr_unregister();
	pr_notice(NMIMGR_NAME": unloaded module\n");
}

module_exit(clean_module);




MODULE_AUTHOR("Adrien Mahieux <adrien.mahieux@gmail.com");
MODULE_DESCRIPTION("Remap specified NMI codes to generate a Panic\n"
	"or drops specific events (self-test or while kdump'ing)\n"
	"Also reads kernel parameter events_panic= upon loading");
MODULE_LICENSE("GPL");
MODULE_VERSION(NMIMGR_VERSION);

/* Parameters */
module_param(events_panic, charp, 0444);
MODULE_PARM_DESC(events_panic, "List of NMIs to panic upon receiving");

module_param(events_debug, charp, 0444);
MODULE_PARM_DESC(events_debug, "List of NMIs to show debug upon receiving");

module_param(events_ignore, charp, 0444);
MODULE_PARM_DESC(events_ignore, "List of NMIs to ignore silently");

module_param(events_drop, charp, 0444);
MODULE_PARM_DESC(events_drop, "List of NMIs to hide from other handlers");
