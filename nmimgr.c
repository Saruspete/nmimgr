/**
 * NMI Manager
 *
 * This module allows you to Panic or Ignore specific NMI events.
 *
 * Adrien Mahieux <adrien.mahieux@gmail.com>
 * See also: https://fr.slideshare.net/Saruspete/kernel-crashdump-53496836
 * And tools: https://github.com/saruspete/kdumptools
 *
 *
 * Manage NMI events in a more fine-grained manner than "uknown_nmi_panic".
 * When a production host is unresponsive, we'd like to take a Kernel Dump.
 * If kdump is correctly setup, we need to crash the system for it to start
 *
 * But if no handler registers the vendor-specific NMI event to trigger a
 * crash, the kernel logs a "Dazed and confused, but trying to continue"
 * message and server is still unresponsive.
 *
 *
 * Usage as a module: 
 *  insmod nmimgr.ko events_panic=0,1,2,5-12,13,255 events_ignore=99
 * Usage as Kernel builtin:
 *   nmimgr.events_panic=0,1,2,5-12,13,255 nmimgr.events_ignore=99
 *
 * Parameters:
 * - events_panic=LIST  Events to make the kernel Panic
 * - events_ignore=LIST Events to drop, so no other handler can process them
 *
 *
 * over. Usually, this is done by generating a NMI:
 * - openipmi chassis power diag
 * - vboxmanage debugvm "VMName" injectnmi
 * - virsh inject-nmi "VMName"
 *
 * Usual NMI events (in decimal, to be used as module parameters):
 * - HP Ilo : 32,48
 * - Dell IDRAC: 32,33,48,49
 * - IBM : 44,60
 * - VirtualBox: 0,16,32,48
 *
 * If you see in your system logs messages like :
 * "hhuh. NMI received for unknown reason <xx>" and believe it should have
 * panic'd the system, translate the "xx" hex number to decimal and use this
 * module to panic the system.
 *
 */


/*
 * Kernel Revision history:
 * 2.6.32: Using notifier_block structs
 * 3.2   : Moved NMI descriptions to an enum: LOCAL, UNKNOWN, MAX 
 *         https://lwn.net/Articles/461215/
 *         https://lkml.org/lkml/2012/3/8/386
 * 3.5   : Moved "register_nmi_handler" to a macro + static struct nmiaction fn##_na
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/nmi.h>

/* Compatibility management */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,2,0)
#include <linux/notifier.h> 
#include <linux/kdebug.h>    /* For NMI_DIE and NMI_DIE_IPI */

/* HANDLED=1, OK=1, DONE=0  */
#define NMI_HANDLED NOTIFY_OK
#define NMI_DONE    NOTIFY_DONE
#endif


#define NMIMGR_VERSION  "0.2"
#define NMIMGR_NAME     "nmimgr"
#define NMIMGR_NBMAX	256

static int events_panic_list[NMIMGR_NBMAX];
static int events_ignore_list[NMIMGR_NBMAX];
static char *events_panic;
static char *events_ignore;



#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
/* Borrowed from nmi.c */
/*
#define nmi_to_desc(type) (&nmi_desc[type])
struct nmi_desc {
	spinlock_t lock;
	struct list_head head;
};
static struct nmi_desc *nmi_desc = 0xFFFFFFFF819556A0;

static void nmimgr_showhandlers(unsigned int type)
{
	struct nmi_desc *desc = nmi_to_desc(type);
	struct nmiaction *n, *m = NULL;
	unsigned long flags;

	pr_notice("== Current handlers for type %u\n", type);

	spin_lock_irqsave(&desc->lock, flags);
	list_for_each_entry_rcu(n, &desc->head, list) {
		if (m == n)
			break;

		pr_notice("%p: %s (%lu) prev:%p next:%p\n", n->handler, n->name, n->flags, n->list.prev, n->list.next);

		if (!m)
			m = n;
	}
	spin_unlock_irqrestore(&desc->lock, flags);

}
*/
/*
 Silly Debug. Use it with : 
for(j=NMI_LOCAL+1;j<NMI_MAX;j++) nmimgr_showhandlers(j);
*/
#endif

/**
 * Handler
 */
static int __nmimgr_handle(unsigned int type, unsigned char reason)
{

	int i;

	pr_notice(NMIMGR_NAME": Handling new NMI type:%u event:0x%02x (%d)\n", type, reason, reason);

	/* Check for discarded NMI */
	for(i=1; i<NMIMGR_NBMAX; i++) {
		int j = events_ignore_list[i];

		pr_debug(NMIMGR_NAME": Ignore: Checking 0x%02x(%d) against 0x%02x (%d)\n", reason, reason, j, j);

		if (reason == j) {
			pr_notice(NMIMGR_NAME": Ignore: dropping NMI event 0x%02x (%d)\n", reason, reason);
			return NMI_HANDLED;
		}
	}

	/* Check for Panic NMI */
	for(i=1; i<NMIMGR_NBMAX; i++) {
		int j = events_panic_list[i];

		pr_debug(NMIMGR_NAME": Panic: Checking 0x%02x(%d) against 0x%02x (%d)\n", reason, reason, j, j);

		if (reason == j) {
			pr_emerg(NMIMGR_NAME": Panic: Event 0x%02x(%d) triggered panic\n", reason, reason);
/*
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
			nmi_panic(regs, "NMIMGR: Hit explicit panic reason");
#else
*/
			panic("NMIMGR: Hit explicit panic reason");
/*
#endif
*/
		}
	}

	pr_notice(NMIMGR_NAME": Unmanaged NMI 0x%02x(%d), let other handlers process it\n", reason, reason);
		
	/* Still there: unmanaged NMI Code. Send to other handlers */
	return NMI_DONE;
}




/***** Kernel < 3.2 ***********************************************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,2,0)

static int nmimgr_handle(struct notifier_block *nb, unsigned long reason, void *data)
{
	return __nmimgr_handle(1, (unsigned char)reason);
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
	int ret;
	ret = register_die_notifier(&nmimgr_notifier);
	if (ret) {
		pr_warning(NMIMGR_NAME": Unable to register NMI handler\n");
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
	return __nmimgr_handle(type, x86_platform.get_nmi_reason());
}



/**
 * handler registration
 */
static int nmimgr_register(void)
{
	int ret = 0;
	int i;

/* This wont work because of Macro rewriting of register_nmi_handler (https://lkml.org/lkml/2012/3/8/386)
	for (i=NMI_LOCAL+1; i<NMI_MAX; i++) {
		pr_notice(NMIMGR_NAME": registering type %d\n", i);
for(j=NMI_LOCAL+1;j<NMI_MAX;j++) nmimgr_showhandlers(j);

		ret = register_nmi_handler(i, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
		pr_notice(NMIMGR_NAME": Registered type %d: %d\n", i, ret);
		if (ret) {
			pr_warning(NMIMGR_NAME ": Unable to register NMI class %d\n", i);
			goto err;
		}
for(j=NMI_LOCAL+1;j<NMI_MAX;j++) nmimgr_showhandlers(j);
	}
*/


	/* We register our handler first, as we only manage a specific list */
	ret = register_nmi_handler(NMI_UNKNOWN, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warning(NMIMGR_NAME ": Unable to register NMI_UNKNOWN\n");
		i = NMI_UNKNOWN-1;
		goto err;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
	ret = register_nmi_handler(NMI_SERR, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warning(NMIMGR_NAME ": Unable to register NMI_SERR\n");
		i = NMI_SERR-1;
		goto err;
	}
	ret = register_nmi_handler(NMI_IO_CHECK, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
	if (ret) {
		pr_warning(NMIMGR_NAME ": Unable to register NMI_IO_CHECK\n");
		i = NMI_IO_CHECK-1;
		goto err;
	}
#endif


	pr_notice(NMIMGR_NAME ": Registered NMIs\n");
	return 0;

err:
	for(; i>0; i--) {
		unregister_nmi_handler(i, NMIMGR_NAME);
	}

	return ret;
}

/**
 * Handler unregistration
 */
static void nmimgr_unregister(void)
{
	int i;

	for (i=NMI_MAX-1; i>NMI_LOCAL; i--) {
		pr_notice(NMIMGR_NAME": Unregistering type %d\n", i);
		unregister_nmi_handler(i, NMIMGR_NAME);
	}
}

#endif

/******************************************************************************/



/**
 * Parse the input string
 */
static int __init nmimgr_setup_panic(char *str)
{
	char *ret = 0;
	
	/* from lib/cmdline.c : Extract the int list from str into events_panic_list[] */
	pr_info(NMIMGR_NAME ": Setting panic from '%s'\n", events_panic);
	ret = get_options(str, ARRAY_SIZE(events_panic_list), events_panic_list);
	if (ret && *ret != 0) {
		pr_err(NMIMGR_NAME ": Error, invalid or too many events_panic= values, return %s", ret);
		return 0;
	}
	return 1;
}
__setup("nmimgr.events_panic=", nmimgr_setup_panic);


/**
 *
 */
static int __init nmimgr_setup_ignore(char *str)
{
	char *ret = 0;

	pr_info(NMIMGR_NAME ": Setting ignore from '%s'\n", events_ignore);
	ret = get_options(str, ARRAY_SIZE(events_ignore_list), events_ignore_list);
	if (ret && *ret != 0) {
		pr_err(NMIMGR_NAME ": Error, invalid or too many events_ignore= values, return %s\n", ret);
		return 0;
	}
	return 1;
}
__setup("nmimgr.events_ignore=", nmimgr_setup_ignore);



/**
 * Module initialization 
 */
int __init init_module(void)
{
	int err;

	pr_notice(NMIMGR_NAME ": Loaded module v%s\n", NMIMGR_VERSION);

	nmimgr_setup_panic(events_panic);
	nmimgr_setup_ignore(events_ignore);

	err = nmimgr_register();
	if (err) {
		pr_warning(NMIMGR_NAME ": The NMI Management is not available");
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
	pr_notice(NMIMGR_NAME": unloaded module");
}

module_exit(clean_module);




MODULE_AUTHOR("Adrien Mahieux <adrien.mahieux@gmail.com");
MODULE_DESCRIPTION("Remap specified NMI codes to generate a Panic (then kdump)\n"
					"Also reads kernel parameter events_panic= upon loading");
MODULE_LICENSE("GPL");
MODULE_VERSION(NMIMGR_VERSION);

/* Parameters */
module_param(events_panic, charp, 0444);
MODULE_PARM_DESC(events_panic, "List of NMIs to panic upon receiving");

module_param(events_ignore, charp, 0444);
MODULE_PARM_DESC(events_ignore, "List of NMIs to block and ignore upon receiving");
