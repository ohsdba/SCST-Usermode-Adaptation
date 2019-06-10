/* scst_compat.c
 * Compatibility for SCST running in usermode
 * Copyright 2016-2019 David A. Butterfield
 *
 * This provides the interface between the daemon and the kernel code (running in usermode).
 * The daemon runs on the main (TID == PID) thread, and the kernel threads are created using
 * pthread_create.
 *
 * SCST_init is invoked from a gcc constructor function that runs before main().  SCST_init
 * invokes the init functions declared in various parts of the SCST kernel logic, which starts
 * the various threads implemented by SCST.
 *
 * Daemon ioctl calls to the kernel code are intercepted by SCST_ctldev_ioctl below, which
 * passes it on to ctr_fops.compat_ioctl().
 *
 * The daemon open of the "netlink" is simulated by a socketpair(2) created in SCST_nl_open
 * (below), with one end of the socket being returned to the daemon as its "netlink" endpoint.
 * From the kernel side, our implementation of event_send writes events to the other end of
 * the socketpair, which arrives at the daemon thread as a POLLIN ready on its "netlink".
 *
 * The exit handler (below) begins a clean shutdown:  it closes the kernel end of the socket
 * pair, which induces the daemon to exit its thread.  It also calls the various kernel module
 * exit handlers -- counterparts to the init functions previously called by SCST_init.
 */
#define NAME SCST_COMPAT

#ifndef SCST_COMPAT_H
  #error Makefile failed to force-include scst_compat.h as required
#endif

#include <sys_service.h>
#include <sys_debug.h>

extern const struct file_operations ctr_fops;

extern void UMC_param_create_num_threads(void);
extern void UMC_param_create_scst_vdisk_ID(void);
extern void UMC_param_create_scst_threads(void);
extern void UMC_param_create_scst_max_cmd_mem(void);
extern void UMC_param_create_scst_max_dev_cmd_mem(void);
extern void UMC_param_create_forcibly_close_sessions(void);

extern errno_t UMC_INIT_init_scst(void);			/* scst_main.c */
extern errno_t UMC_INIT_init_scst_vdisk_driver(void);		/* scst_vdisk.c */
extern errno_t UMC_INIT_iscsi_init(void);			/* iscsi.c */

/* Master init() call for all the SCST "kernel modules" */
void
SCST_init(void)
{
    errno_t err;

    /* Establish /proc entries */
    UMC_param_create_num_threads();
    UMC_param_create_scst_vdisk_ID();
    UMC_param_create_scst_threads();
    UMC_param_create_scst_max_cmd_mem();
    UMC_param_create_scst_max_dev_cmd_mem();
    UMC_param_create_forcibly_close_sessions();

    /* Call the various module init functions -- we must know all their names here, which
     * are automatically generated by the module_init and module_exit macros
     */
    err = UMC_INIT_init_scst();			/* scst_main.c */
    verify_noerr(err, "init_scst");

    err = UMC_INIT_init_scst_vdisk_driver();    /* scst_vdisk.c */
    verify_noerr(err, "init_scst_vdisk_driver");

    err = UMC_INIT_iscsi_init();		/* iscsi.c */
    verify_noerr(err, "iscsi_init");
}

extern void UMC_param_remove_num_threads(void);
extern void UMC_param_remove_scst_vdisk_ID(void);
extern void UMC_param_remove_scst_threads(void);
extern void UMC_param_remove_scst_max_cmd_mem(void);
extern void UMC_param_remove_scst_max_dev_cmd_mem(void);
extern void UMC_param_remove_forcibly_close_sessions(void);

extern void UMC_EXIT_exit_scst(void);
extern void UMC_EXIT_exit_scst_vdisk_driver(void);
extern void UMC_EXIT_iscsi_exit(void);

static int SCST_nl_fdwrite = -1;    /* kernel end of nl_fd socket */

void
SCST_exit(void)
{
    /* Close the kernel end of nl_fd, to get daemon thread to exit */
    if (SCST_nl_fdwrite != -1) {
	int rc = close(SCST_nl_fdwrite);
	expect_rc(rc, close, "close(SCST_nl_fdwrite)");
	SCST_nl_fdwrite = -1;
    }

    errno_t err = ctr_fops.release(NULL, NULL);
    verify_eq(err, E_OK);

    UMC_EXIT_iscsi_exit();
    UMC_EXIT_exit_scst_vdisk_driver();
    UMC_EXIT_exit_scst();

    UMC_param_remove_num_threads();
    UMC_param_remove_scst_vdisk_ID();
    UMC_param_remove_scst_threads();
    UMC_param_remove_scst_max_cmd_mem();
    UMC_param_remove_scst_max_dev_cmd_mem();
    UMC_param_remove_forcibly_close_sessions();
}

/******************************************************************************/

/* Called from daemon code to issue an "ioctl" request to the kernel code */
int
SCST_ctldev_ioctl(int fd_arg_ignored, unsigned int cmd, unsigned long arg)
{
    /* The return value isn't necessarily an error; e.g. ADD_TARGET returns tid */
    long ret = ctr_fops.compat_ioctl(NULL, cmd, arg);

    trace("ctr_fops.compat_ioctl cmd=%u/0x%x arg=%"PRIu64"/0x%"PRIx64" returns %"PRId64"/%"PRIx64,
	  cmd, cmd, arg, arg, ret, ret);

    if (ret < 0) {
	/* Translate kernel-style error return to usermode-style */
	errno = (int)-ret;
	ret = -1;
    }
    return (int)ret;
}

/******************************************************************************/

/* Called from daemon code to open an event-notification "nl" socket */
int
SCST_nl_open(void)
{
    int fd[2];
    errno_t err = UMC_socketpair(AF_LOCAL,
		       SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0, fd);
    verify_rc(err, socketpair);
    assert(SCST_nl_fdwrite == -1);
    SCST_nl_fdwrite = fd[1];	    /* kernel code's end of the pipe */
    return fd[0];		    /* daemon's end of the pipe */
}

unsigned long trace_flag;
#include "scst.h"
#include "iscsi.h"
#include <linux/netlink.h>

/* Called from kernel code to issue a notification to the daemon through the "nl" socket */
// XXX Does the data need to be copied?  I don't think so, but should check
errno_t
event_send(u32 tid, u64 sid, u32 cid, u32 cookie,
	   enum iscsi_kern_event_code code, const char * param1, const char * param2)
{
    struct nlmsghdr hdr = {
	.nlmsg_len   = IGNORED,	    /* Length of message including header */
	.nlmsg_type  = IGNORED,     /* Message content */
	.nlmsg_flags = IGNORED,     /* Additional flags */
	.nlmsg_seq   = IGNORED,     /* Sequence number */
	.nlmsg_pid   = IGNORED,     /* Sending process port ID */
    };

    struct iscsi_kern_event event = {
	.tid = tid,
	.sid = sid,
	.cid = cid,
	.code = code,
	.cookie = cookie,
	.param1_size = param1 ? (u32)strlen(param1) : 0,
	.param2_size = param2 ? (u32)strlen(param2) : 0,
    };

    /* Assumes writev(2) is OK with zero-length iov_len elements (XXXX check) */
    struct iovec iov[] = {
	[0] = { .iov_base = &hdr,		 .iov_len = sizeof(hdr)       },
	[1] = { .iov_base = &event,		 .iov_len = sizeof(event)     },
	[2] = { .iov_base = _unconstify(param1), .iov_len = event.param1_size },   //XXX
	[3] = { .iov_base = _unconstify(param2), .iov_len = event.param2_size },   //XXX
    };

    return (errno_t)UMC_kernelize64(writev(SCST_nl_fdwrite, iov, ARRAY_SIZE(iov)));
}

/******************************************************************************/

extern __thread char sys_pthread_name[16];

/* Callback when an AIO thread is created to set up a "current" pointer for it --
 * the AIO thread calls back into "kernel" code which expects this
 */
extern void aios_thread_init(void * unused);	//XXX
void
aios_thread_init(void * unused)
{
    expect_eq(unused, NULL);
    UMC_current_set(
	    UMC_current_init(
		    UMC_current_alloc(),
		    sys_thread_current(),
		    (void *)aios_thread_init,
		    unused,
		    kstrdup(sys_pthread_name, IGNORED)));

    errno_t err = pthread_setname_np(pthread_self(), sys_pthread_name);
    expect_noerr(err, "pthread_setname_np");
}

extern void aios_thread_exit(void * unused);	//XXX
void
aios_thread_exit(void * unused)
{
    expect_eq(unused, NULL);
    assert(current);
    UMC_current_free(current);
    UMC_current_set(NULL);
}

/******************************************************************************/

int event_init(void)	{ return E_OK; }
void event_exit(void)	{ }

extern int scst_event_init(void);   int scst_event_init(void)	{ return E_OK; }
extern void scst_event_exit(void);  void scst_event_exit(void)	{ }

struct scst_cmd;
struct scst_tgt;
struct scst_mgmt_cmd;
struct scst_event_entry;
struct scst_tgt_template;
struct scst_dev_type;

extern void scst_event_queue(uint32_t event_code, const char *issuer_name, struct scst_event_entry *e);

extern int scst_event_queue_lun_not_found(const struct scst_cmd *cmd);
extern int scst_event_queue_negative_luns_inquiry(const struct scst_tgt *tgt, const char *initiator_name);
extern int scst_event_queue_tm_fn_received(struct scst_mgmt_cmd *mcmd);

void scst_event_queue(uint32_t event_code, const char *issuer_name, struct scst_event_entry *e) \
	    { UMC_STUB(scst_event_queue); }
int scst_event_queue_lun_not_found(const struct scst_cmd *cmd) \
	    { sys_warning("SKIP queuing event scst_event_queue_lun_not_found"); return E_OK; }
int scst_event_queue_negative_luns_inquiry(const struct scst_tgt *tgt, const char *initiator_name) \
	    { sys_warning("SKIP queuing event scst_event_queue_negative_luns_inquiry"); return E_OK; }
int scst_event_queue_tm_fn_received(struct scst_mgmt_cmd *mcmd) \
	    { sys_warning("SKIP queuing event scst_event_queue_tm_fn_received"); return E_OK; }

int scsi_reset_provider(struct scsi_device * sdev, int flags) { UMC_STUB(scsi_reset_provider); return E_OK; }
