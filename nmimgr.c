/**
 * NMI Manager
 *
 * This module allows you to Panic or Ignore specific NMI events.
 *
 * Adrien Mahieux <adrien.mahieux@gmail.com>
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
 * Details of the codes:
 * - HP   https://www.kernel.org/doc/Documentation/watchdog/hpwdt.txt
 *    No source found                00h
 *    Uncorrectable Memory Error     01h
 *    ASR NMI                        1Bh
 *    PCI Parity Error               20h
 *    NMI Button Press               27h
 *    SB_BUS_NMI                     28h
 *    ILO Doorbell NMI               29h
 *    ILO IOP NMI                    2Ah
 *    ILO Watchdog NMI               2Bh
 *    Proc Throt NMI                 2Ch
 *    Front Side Bus NMI             2Dh
 *    PCI Express Error              2Fh
 *    DMA controller NMI             30h
 *    Hypertransport/CSI Error       31h
 *
 * If you see in your system logs messages like :
 * "hhuh. NMI received for unknown reason <xx>" and believe it should have
 * panic'd the system, translate the "xx" hex number to decimal and use this
 * module to panic the system.
 *
 */


#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/nmi.h>

#define NMIMGR_VERSION  "0.1"
#define NMIMGR_NAME     "nmimgr"
#define NMIMGR_NBMAX	256
static int events_panic_list[NMIMGR_NBMAX];
static int events_ignore_list[NMIMGR_NBMAX];
static char *events_panic;
static char *events_ignore;



/**
 * Handler
 */

static int nmimgr_handle(unsigned int type, struct pt_regs *regs)
{

	int i;
	unsigned char reason = x86_platform.get_nmi_reason();

	pr_notice(NMIMGR_NAME ": Handling new NMI type:%u event:0x%02x (%d)\n", type, reason, reason);

	/* Check for discarded NMI */
	for(i=1; i<=NMIMGR_NBMAX; i++) {
		int j = events_ignore_list[i];

		pr_debug(NMIMGR_NAME": Ignore: Checking 0x%02x(%d) against 0x%02x (%d)\n", reason, reason, j, j);
		if (j == 0)
			break;

		if (reason == j) {
			pr_notice(NMIMGR_NAME": Ignore: dropping NMI event 0x%02x (%d)\n", reason, reason);
			return 1;
		}
	}

	/* Check for Panic NMI */
	for(i=1; i<NMIMGR_NBMAX; i++) {
		/* unsigned char j = (unsigned char)events_panic_list[i]; */
		int j = events_panic_list[i];

		pr_debug(NMIMGR_NAME ": Panic: Checking 0x%02x(%d) against 0x%02x (%d)\n", reason, reason, j, j);
		if (j == 0)
			break;

		if (reason == j) {
			pr_emerg(NMIMGR_NAME": Panic: Event 0x%02x(%d) triggered panic\n", reason, reason);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
			nmi_panic(regs, "NMIMGR: Hit explicit panic reason");
#else
			panic("NMIMGR: Hit explicit panic reason");
#endif
		}
	}

	pr_notice(NMIMGR_NAME": Unmanaged NMI 0x%02x(%d), let other handlers process it\n", reason, reason);
		
	/* Still there: unmanaged NMI Code. Send to other handlers */
	return NMI_DONE;
}



/**
 * handler registration
 */
static int nmimgr_register(void)
{
	int ret = 0;
	int i;

	for (i=1; i<NMI_MAX; i++) {
		/* We do register our handler first, as we only manage a specific list */
		ret = register_nmi_handler(i, nmimgr_handle, NMI_FLAG_FIRST, NMIMGR_NAME);
		if (ret) {
			pr_warning(NMIMGR_NAME ": Unable to register NMI class %d\n", i);
			goto err;
		}
	}
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
	unregister_nmi_handler(NMI_UNKNOWN, NMIMGR_NAME);
	unregister_nmi_handler(NMI_SERR, NMIMGR_NAME);
	unregister_nmi_handler(NMI_IO_CHECK, NMIMGR_NAME);
}



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
	if (err)
		pr_warning(NMIMGR_NAME ": The NMI Management is not available");

	return 0;
}
/* module_init(init_module); */

/**
 * Module unloading
 */
void __exit clean_module(void)
{

	nmimgr_unregister();

	/* pr_info("Released NMI codes: %s\n", nmi_list_); */
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
