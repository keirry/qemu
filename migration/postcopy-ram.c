/*
 * Postcopy migration for RAM
 *
 * Copyright 2013-2015 Red Hat, Inc. and/or its affiliates
 *
 * Authors:
 *  Dave Gilbert  <dgilbert@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/*
 * Postcopy is a migration technique where the execution flips from the
 * source to the destination before all the data has been copied.
 */

#include <glib.h>
#include <stdio.h>
#include <unistd.h>

#include "qemu-common.h"
#include "migration/migration.h"
#include "migration/postcopy-ram.h"
#include "sysemu/sysemu.h"
#include "qemu/error-report.h"
#include "trace.h"

#define MAX_DISCARDS_PER_COMMAND 12

struct PostcopyDiscardState {
    const char *name;
    uint64_t offset; /* Bitmap entry for the 1st bit of this RAMBlock */
    uint16_t cur_entry;
    /*
     * Start and end address of a discard range; end_list points to the byte
     * after the end of the range.
     */
    uint64_t start_list[MAX_DISCARDS_PER_COMMAND];
    uint64_t   end_list[MAX_DISCARDS_PER_COMMAND];
    unsigned int nsentwords;
    unsigned int nsentcmds;
};

/* Postcopy needs to detect accesses to pages that haven't yet been copied
 * across, and efficiently map new pages in, the techniques for doing this
 * are target OS specific.
 */
#if defined(__linux__)

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <asm/types.h> /* for __u64 */
#endif

#if defined(__linux__) && defined(__NR_userfaultfd)
#include <linux/userfaultfd.h>

static bool ufd_version_check(int ufd)
{
    struct uffdio_api api_struct;
    uint64_t feature_mask;

    api_struct.api = UFFD_API;
    if (ioctl(ufd, UFFDIO_API, &api_struct)) {
        error_report("postcopy_ram_supported_by_host: UFFDIO_API failed: %s",
                     strerror(errno));
        return false;
    }

    feature_mask = (__u64)1 << _UFFDIO_REGISTER |
                   (__u64)1 << _UFFDIO_UNREGISTER;
    if ((api_struct.ioctls & feature_mask) != feature_mask) {
        error_report("Missing userfault features: %" PRIx64,
                     (uint64_t)(~api_struct.ioctls & feature_mask));
        return false;
    }

    return true;
}

/*
 * Note: This has the side effect of munlock'ing all of RAM, that's
 * normally fine since if the postcopy succeeds it gets turned back on at the
 * end.
 */
bool postcopy_ram_supported_by_host(void)
{
    long pagesize = getpagesize();
    int ufd = -1;
    bool ret = false; /* Error unless we change it */
    void *testarea = NULL;
    struct uffdio_register reg_struct;
    struct uffdio_range range_struct;
    uint64_t feature_mask;

    if ((1ul << qemu_target_page_bits()) > pagesize) {
        error_report("Target page size bigger than host page size");
        goto out;
    }

    ufd = syscall(__NR_userfaultfd, O_CLOEXEC);
    if (ufd == -1) {
        error_report("%s: userfaultfd not available: %s", __func__,
                     strerror(errno));
        goto out;
    }

    /* Version and features check */
    if (!ufd_version_check(ufd)) {
        goto out;
    }

    /*
     * userfault and mlock don't go together; we'll put it back later if
     * it was enabled.
     */
    if (munlockall()) {
        error_report("%s: munlockall: %s", __func__,  strerror(errno));
        return -1;
    }

    /*
     *  We need to check that the ops we need are supported on anon memory
     *  To do that we need to register a chunk and see the flags that
     *  are returned.
     */
    testarea = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE |
                                    MAP_ANONYMOUS, -1, 0);
    if (testarea == MAP_FAILED) {
        error_report("%s: Failed to map test area: %s", __func__,
                     strerror(errno));
        goto out;
    }
    g_assert(((size_t)testarea & (pagesize-1)) == 0);

    reg_struct.range.start = (uintptr_t)testarea;
    reg_struct.range.len = pagesize;
    reg_struct.mode = UFFDIO_REGISTER_MODE_MISSING;

    if (ioctl(ufd, UFFDIO_REGISTER, &reg_struct)) {
        error_report("%s userfault register: %s", __func__, strerror(errno));
        goto out;
    }

    range_struct.start = (uintptr_t)testarea;
    range_struct.len = pagesize;
    if (ioctl(ufd, UFFDIO_UNREGISTER, &range_struct)) {
        error_report("%s userfault unregister: %s", __func__, strerror(errno));
        goto out;
    }

    feature_mask = (__u64)1 << _UFFDIO_WAKE |
                   (__u64)1 << _UFFDIO_COPY |
                   (__u64)1 << _UFFDIO_ZEROPAGE;
    if ((reg_struct.ioctls & feature_mask) != feature_mask) {
        error_report("Missing userfault map features: %" PRIx64,
                     (uint64_t)(~reg_struct.ioctls & feature_mask));
        goto out;
    }

    /* Success! */
    ret = true;
out:
    if (testarea) {
        munmap(testarea, pagesize);
    }
    if (ufd != -1) {
        close(ufd);
    }
    return ret;
}

/*
 * Discard the contents of memory start..end inclusive.
 * We can assume that if we've been called postcopy_ram_hosttest returned true
 */
int postcopy_ram_discard_range(MigrationIncomingState *mis, uint8_t *start,
                               uint8_t *end)
{
    trace_postcopy_ram_discard_range(start, end);
    if (madvise(start, (end-start)+1, MADV_DONTNEED)) {
        error_report("%s MADV_DONTNEED: %s", __func__, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * Setup an area of RAM so that it *can* be used for postcopy later; this
 * must be done right at the start prior to pre-copy.
 * opaque should be the MIS.
 */
static int init_area(const char *block_name, void *host_addr,
                     ram_addr_t offset, ram_addr_t length, void *opaque)
{
    MigrationIncomingState *mis = opaque;

    trace_postcopy_init_area(block_name, host_addr, offset, length);

    /*
     * We need the whole of RAM to be truly empty for postcopy, so things
     * like ROMs and any data tables built during init must be zero'd
     * - we're going to get the copy from the source anyway.
     * (Precopy will just overwrite this data, so doesn't need the discard)
     */
    if (postcopy_ram_discard_range(mis, host_addr, (host_addr + length - 1))) {
        return -1;
    }

    /*
     * We also need the area to be normal 4k pages, not huge pages
     * (otherwise we can't be sure we can atomically place the
     * 4k page in later).  THP might come along and map a 2MB page
     * and when it's partially accessed in precopy it might not break
     * it down, but leave a 2MB zero'd page.
     */
#ifdef MADV_NOHUGEPAGE
    if (madvise(host_addr, length, MADV_NOHUGEPAGE)) {
        error_report("%s: NOHUGEPAGE: %s", __func__, strerror(errno));
        return -1;
    }
#endif

    return 0;
}

/*
 * At the end of migration, undo the effects of init_area
 * opaque should be the MIS.
 */
static int cleanup_area(const char *block_name, void *host_addr,
                        ram_addr_t offset, ram_addr_t length, void *opaque)
{
    MigrationIncomingState *mis = opaque;
    struct uffdio_range range_struct;
    trace_postcopy_cleanup_area(block_name, host_addr, offset, length);

    /*
     * We turned off hugepage for the precopy stage with postcopy enabled
     * we can turn it back on now.
     */
#ifdef MADV_HUGEPAGE
    if (madvise(host_addr, length, MADV_HUGEPAGE)) {
        error_report("%s HUGEPAGE: %s", __func__, strerror(errno));
        return -1;
    }
#endif

    /*
     * We can also turn off userfault now since we should have all the
     * pages.   It can be useful to leave it on to debug postcopy
     * if you're not sure it's always getting every page.
     */
    range_struct.start = (uintptr_t)host_addr;
    range_struct.len = length;

    if (ioctl(mis->userfault_fd, UFFDIO_UNREGISTER, &range_struct)) {
        error_report("%s: userfault unregister %s", __func__, strerror(errno));

        return -1;
    }

    return 0;
}

/*
 * Initialise postcopy-ram, setting the RAM to a state where we can go into
 * postcopy later; must be called prior to any precopy.
 * called from arch_init's similarly named ram_postcopy_incoming_init
 */
int postcopy_ram_incoming_init(MigrationIncomingState *mis, size_t ram_pages)
{
    if (qemu_ram_foreach_block(init_area, mis)) {
        return -1;
    }

    return 0;
}

/*
 * At the end of a migration where postcopy_ram_incoming_init was called.
 */
int postcopy_ram_incoming_cleanup(MigrationIncomingState *mis)
{
    trace_postcopy_ram_incoming_cleanup_entry();

    if (mis->have_fault_thread) {
        uint64_t tmp64;

        if (qemu_ram_foreach_block(cleanup_area, mis)) {
            return -1;
        }
        /*
         * Tell the fault_thread to exit, it's an eventfd that should
         * currently be at 0, we're going to inc it to 1
         */
        tmp64 = 1;
        if (write(mis->userfault_quit_fd, &tmp64, 8) == 8) {
            trace_postcopy_ram_incoming_cleanup_join();
            qemu_thread_join(&mis->fault_thread);
        } else {
            /* Not much we can do here, but may as well report it */
            error_report("%s: incing userfault_quit_fd: %s", __func__,
                         strerror(errno));
        }
        trace_postcopy_ram_incoming_cleanup_closeuf();
        close(mis->userfault_fd);
        close(mis->userfault_quit_fd);
        mis->have_fault_thread = false;
    }

    if (enable_mlock) {
        if (os_mlock() < 0) {
            error_report("mlock: %s", strerror(errno));
            /*
             * It doesn't feel right to fail at this point, we have a valid
             * VM state.
             */
        }
    }

    postcopy_state_set(mis, POSTCOPY_INCOMING_END);
    migrate_send_rp_shut(mis, qemu_file_get_error(mis->file) != 0);

    if (mis->postcopy_tmp_page) {
        munmap(mis->postcopy_tmp_page, getpagesize());
        mis->postcopy_tmp_page = NULL;
    }
    trace_postcopy_ram_incoming_cleanup_exit();
    return 0;
}

/*
 * Mark the given area of RAM as requiring notification to unwritten areas
 * Used as a  callback on qemu_ram_foreach_block.
 *   host_addr: Base of area to mark
 *   offset: Offset in the whole ram arena
 *   length: Length of the section
 *   opaque: MigrationIncomingState pointer
 * Returns 0 on success
 */
static int ram_block_enable_notify(const char *block_name, void *host_addr,
                                   ram_addr_t offset, ram_addr_t length,
                                   void *opaque)
{
    MigrationIncomingState *mis = opaque;
    struct uffdio_register reg_struct;

    reg_struct.range.start = (uintptr_t)host_addr;
    reg_struct.range.len = length;
    reg_struct.mode = UFFDIO_REGISTER_MODE_MISSING;

    /* Now tell our userfault_fd that it's responsible for this area */
    if (ioctl(mis->userfault_fd, UFFDIO_REGISTER, &reg_struct)) {
        error_report("%s userfault register: %s", __func__, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * Handle faults detected by the USERFAULT markings
 */
static void *postcopy_ram_fault_thread(void *opaque)
{
    MigrationIncomingState *mis = (MigrationIncomingState *)opaque;
    uint64_t hostaddr; /* The kernel always gives us 64 bit, not a pointer */
    int ret;
    size_t hostpagesize = getpagesize();
    RAMBlock *rb = NULL;
    RAMBlock *last_rb = NULL; /* last RAMBlock we sent part of */
    uint8_t *local_tmp_page;

    trace_postcopy_ram_fault_thread_entry();
    qemu_sem_post(&mis->fault_thread_sem);

    local_tmp_page = mmap(NULL, getpagesize(),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (!local_tmp_page) {
        error_report("%s mapping local tmp page: %s", __func__,
                     strerror(errno));
        return NULL;
    }
    if (madvise(local_tmp_page, getpagesize(), MADV_DONTFORK)) {
        munmap(local_tmp_page, getpagesize());
        error_report("%s postcopy local page DONTFORK: %s", __func__,
                     strerror(errno));
        return NULL;
    }

    while (true) {
        ram_addr_t rb_offset;
        ram_addr_t in_raspace;
        struct pollfd pfd[2];

        /*
         * We're mainly waiting for the kernel to give us a faulting HVA,
         * however we can be told to quit via userfault_quit_fd which is
         * an eventfd
         */
        pfd[0].fd = mis->userfault_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = mis->userfault_quit_fd;
        pfd[1].events = POLLIN; /* Waiting for eventfd to go positive */
        pfd[1].revents = 0;

        if (poll(pfd, 2, -1 /* Wait forever */) == -1) {
            error_report("%s: userfault poll: %s", __func__, strerror(errno));
            break;
        }

        if (pfd[1].revents) {
            trace_postcopy_ram_fault_thread_quit();
            break;
        }

        ret = read(mis->userfault_fd, &hostaddr, sizeof(hostaddr));
        if (ret != sizeof(hostaddr)) {
            if (errno == EAGAIN) {
                /*
                 * if a wake up happens on the other thread just after
                 * the poll, there is nothing to read.
                 */
                continue;
            }
            if (ret < 0) {
                error_report("%s: Failed to read full userfault hostaddr: %s",
                             __func__, strerror(errno));
                break;
            } else {
                error_report("%s: Read %d bytes from userfaultfd expected %zd",
                             __func__, ret, sizeof(hostaddr));
                break; /* Lost alignment, don't know what we'd read next */
            }
        }

        rb = qemu_ram_block_from_host((void *)(uintptr_t)hostaddr, true,
                                      &in_raspace, &rb_offset);
        if (!rb) {
            error_report("postcopy_ram_fault_thread: Fault outside guest: %"
                         PRIx64, hostaddr);
            break;
        }

        trace_postcopy_ram_fault_thread_request(hostaddr,
                                                qemu_ram_get_idstr(rb),
                                                rb_offset);

        /*
         * Send the request to the source - we want to request one
         * of our host page sizes (which is >= TPS)
         */
        if (rb != last_rb) {
            last_rb = rb;
            migrate_send_rp_req_pages(mis, qemu_ram_get_idstr(rb),
                                     rb_offset, hostpagesize);
        } else {
            /* Save some space */
            migrate_send_rp_req_pages(mis, NULL,
                                     rb_offset, hostpagesize);
        }
    }
    munmap(local_tmp_page, getpagesize());
    trace_postcopy_ram_fault_thread_exit();
    return NULL;
}

int postcopy_ram_enable_notify(MigrationIncomingState *mis)
{
    /* Open the fd for the kernel to give us userfaults */
    mis->userfault_fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (mis->userfault_fd == -1) {
        error_report("%s: Failed to open userfault fd: %s", __func__,
                     strerror(errno));
        return -1;
    }

    /*
     * Although the host check already tested the API, we need to
     * do the check again as an ABI handshake on the new fd.
     */
    if (!ufd_version_check(mis->userfault_fd)) {
        return -1;
    }

    /* Now an eventfd we use to tell the fault-thread to quit */
    mis->userfault_quit_fd = eventfd(0, EFD_CLOEXEC);
    if (mis->userfault_quit_fd == -1) {
        error_report("%s: Opening userfault_quit_fd: %s", __func__,
                     strerror(errno));
        close(mis->userfault_fd);
        return -1;
    }

    qemu_sem_init(&mis->fault_thread_sem, 0);
    qemu_thread_create(&mis->fault_thread, "postcopy/fault",
                       postcopy_ram_fault_thread, mis, QEMU_THREAD_JOINABLE);
    qemu_sem_wait(&mis->fault_thread_sem);
    qemu_sem_destroy(&mis->fault_thread_sem);
    mis->have_fault_thread = true;

    /* Mark so that we get notified of accesses to unwritten areas */
    if (qemu_ram_foreach_block(ram_block_enable_notify, mis)) {
        return -1;
    }

    trace_postcopy_ram_enable_notify();

    return 0;
}

/*
 * Place a host page (from) at (host) atomically
 * all_zero: Hint that the page being placed is 0 throughout
 * returns 0 on success
 */
int postcopy_place_page(MigrationIncomingState *mis, void *host, void *from,
                        bool all_zero)
{
    if (!all_zero) {
        struct uffdio_copy copy_struct;

        copy_struct.dst = (uint64_t)(uintptr_t)host;
        copy_struct.src = (uint64_t)(uintptr_t)from;
        copy_struct.len = getpagesize();
        copy_struct.mode = 0;

        /* copy also acks to the kernel waking the stalled thread up
         * TODO: We can inhibit that ack and only do it if it was requested
         * which would be slightly cheaper, but we'd have to be careful
         * of the order of updating our page state.
         */
        if (ioctl(mis->userfault_fd, UFFDIO_COPY, &copy_struct)) {
            int e = errno;
            error_report("%s: %s copy host: %p from: %p",
                         __func__, strerror(e), host, from);

            return -e;
        }
    } else {
        struct uffdio_zeropage zero_struct;

        zero_struct.range.start = (uint64_t)(uintptr_t)host;
        zero_struct.range.len = getpagesize();
        zero_struct.mode = 0;

        if (ioctl(mis->userfault_fd, UFFDIO_ZEROPAGE, &zero_struct)) {
            int e = errno;
            error_report("%s: %s zero host: %p from: %p",
                         __func__, strerror(e), host, from);

            return -e;
        }
    }

    trace_postcopy_place_page(host, all_zero);
    return 0;
}

/*
 * Returns a target page of memory that can be mapped at a later point in time
 * using postcopy_place_page
 * The same address is used repeatedly, postcopy_place_page just takes the
 * backing page away.
 * Returns: Pointer to allocated page
 *
 */
void *postcopy_get_tmp_page(MigrationIncomingState *mis)
{
    if (!mis->postcopy_tmp_page) {
        mis->postcopy_tmp_page = mmap(NULL, getpagesize(),
                             PROT_READ | PROT_WRITE, MAP_PRIVATE |
                             MAP_ANONYMOUS, -1, 0);
        if (!mis->postcopy_tmp_page) {
            error_report("%s: %s", __func__, strerror(errno));
            return NULL;
        }
    }

    return mis->postcopy_tmp_page;
}

#else
/* No target OS support, stubs just fail */
bool postcopy_ram_supported_by_host(void)
{
    error_report("%s: No OS support", __func__);
    return false;
}

int postcopy_ram_incoming_init(MigrationIncomingState *mis, size_t ram_pages)
{
    error_report("postcopy_ram_incoming_init: No OS support");
    return -1;
}

int postcopy_ram_incoming_cleanup(MigrationIncomingState *mis)
{
    assert(0);
}

int postcopy_ram_discard_range(MigrationIncomingState *mis, uint8_t *start,
                               uint8_t *end)
{
    assert(0);
}

int postcopy_ram_enable_notify(MigrationIncomingState *mis)
{
    assert(0);
}

int postcopy_place_page(MigrationIncomingState *mis, void *host, void *from,
                        bool all_zero)
{
    assert(0);
}

void *postcopy_get_tmp_page(MigrationIncomingState *mis)
{
    assert(0);
}

#endif

/* ------------------------------------------------------------------------- */

/*
 * Called at the start of each RAMBlock by the bitmap code
 * 'offset' is the bitmap offset of the named RAMBlock in the migration
 * bitmap.
 * Returns a new PDS
 */
PostcopyDiscardState *postcopy_discard_send_init(MigrationState *ms,
                                                 unsigned long offset,
                                                 const char *name)
{
    PostcopyDiscardState *res = g_try_malloc(sizeof(PostcopyDiscardState));

    if (res) {
        res->name = name;
        res->cur_entry = 0;
        res->nsentwords = 0;
        res->nsentcmds = 0;
        res->offset = offset;
    }

    return res;
}

/*
 * Called by the bitmap code for each chunk to discard
 * May send a discard message, may just leave it queued to
 * be sent later
 * 'start' and 'end' describe an inclusive range of pages in the
 * migration bitmap in the RAM block passed to postcopy_discard_send_init
 */
void postcopy_discard_send_range(MigrationState *ms, PostcopyDiscardState *pds,
                                unsigned long start, unsigned long end)
{
    size_t tp_bits = qemu_target_page_bits();
    /* Convert to byte offsets within the RAM block */
    pds->start_list[pds->cur_entry] = (start - pds->offset) << tp_bits;
    pds->end_list[pds->cur_entry] = (1 + end - pds->offset) << tp_bits;
    pds->cur_entry++;
    pds->nsentwords++;

    if (pds->cur_entry == MAX_DISCARDS_PER_COMMAND) {
        /* Full set, ship it! */
        qemu_savevm_send_postcopy_ram_discard(ms->file, pds->name,
                                              pds->cur_entry,
                                              pds->start_list, pds->end_list);
        pds->nsentcmds++;
        pds->cur_entry = 0;
    }
}

/*
 * Called at the end of each RAMBlock by the bitmap code
 * Sends any outstanding discard messages, frees the PDS
 */
void postcopy_discard_send_finish(MigrationState *ms, PostcopyDiscardState *pds)
{
    /* Anything unsent? */
    if (pds->cur_entry) {
        qemu_savevm_send_postcopy_ram_discard(ms->file, pds->name,
                                              pds->cur_entry,
                                              pds->start_list, pds->end_list);
        pds->nsentcmds++;
    }

    trace_postcopy_discard_send_finish(pds->name, pds->nsentwords,
                                       pds->nsentcmds);

    g_free(pds);
}
