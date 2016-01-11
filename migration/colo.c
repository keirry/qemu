/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include <unistd.h>
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "migration/colo.h"
#include "trace.h"
#include "qemu/error-report.h"
#include "migration/failover.h"
#include "qapi-event.h"
#include "net/filter.h"
#include "net/net.h"
#include "block/block_int.h"

static bool vmstate_loading;

/* colo buffer */
#define COLO_BUFFER_BASE_SIZE (4 * 1024 * 1024)

bool colo_supported(void)
{
    return true;
}

bool migration_in_colo_state(void)
{
    MigrationState *s = migrate_get_current();

    return (s->state == MIGRATION_STATUS_COLO);
}

bool migration_incoming_in_colo_state(void)
{
    MigrationIncomingState *mis = migration_incoming_get_current();

    return mis && (mis->state == MIGRATION_STATUS_COLO);
}

static bool colo_runstate_is_stopped(void)
{
    return runstate_check(RUN_STATE_COLO) || !runstate_is_running();
}

static void secondary_vm_do_failover(void)
{
    int old_state;
    MigrationIncomingState *mis = migration_incoming_get_current();
    Error *local_err = NULL;

    /* Can not do failover during the process of VM's loading VMstate, Or
      * it will break the secondary VM.
      */
    if (vmstate_loading) {
        old_state = failover_set_state(FAILOVER_STATUS_HANDLING,
                                       FAILOVER_STATUS_RELAUNCH);
        if (old_state != FAILOVER_STATUS_HANDLING) {
            error_report("Unknown error while do failover for secondary VM,"
                         "old_state: %d", old_state);
        }
        return;
    }

    migrate_set_state(&mis->state, MIGRATION_STATUS_COLO,
                      MIGRATION_STATUS_COMPLETED);

    bdrv_stop_replication_all(true, &local_err);
    if (local_err) {
        error_report_err(local_err);
    }
    trace_colo_stop_block_replication("failover");

    if (!autostart) {
        error_report("\"-S\" qemu option will be ignored in secondary side");
        /* recover runstate to normal migration finish state */
        autostart = true;
    }
    /*
    * Make sure colo incoming thread not block in recv or send,
    * If mis->from_src_file and mis->to_src_file use the same fd,
    * The second shutdown() will return -1, we ignore this value,
    * it is harmless.
    */
    if (mis->from_src_file) {
        qemu_file_shutdown(mis->from_src_file);
    }
    if (mis->to_src_file) {
        qemu_file_shutdown(mis->to_src_file);
    }

    old_state = failover_set_state(FAILOVER_STATUS_HANDLING,
                                   FAILOVER_STATUS_COMPLETED);
    if (old_state != FAILOVER_STATUS_HANDLING) {
        error_report("Incorrect state (%d) while doing failover for "
                     "secondary VM", old_state);
        return;
    }
    /* Notify COLO incoming thread that failover work is finished */
    qemu_sem_post(&mis->colo_incoming_sem);
    /* For Secondary VM, jump to incoming co */
    if (mis->migration_incoming_co) {
        qemu_coroutine_enter(mis->migration_incoming_co, NULL);
    }
}

static void primary_vm_do_failover(void)
{
    MigrationState *s = migrate_get_current();
    int old_state;
    Error *local_err = NULL;

    migrate_set_state(&s->state, MIGRATION_STATUS_COLO,
                      MIGRATION_STATUS_COMPLETED);

    /*
    * Make sure colo thread no block in recv or send,
    * The s->rp_state.from_dst_file and s->to_dst_file may use the
    * same fd, but we still shutdown the fd for twice, it is harmless.
    */
    if (s->to_dst_file) {
        qemu_file_shutdown(s->to_dst_file);
    }
    if (s->rp_state.from_dst_file) {
        qemu_file_shutdown(s->rp_state.from_dst_file);
    }

    old_state = failover_set_state(FAILOVER_STATUS_HANDLING,
                                   FAILOVER_STATUS_COMPLETED);
    if (old_state != FAILOVER_STATUS_HANDLING) {
        error_report("Incorrect state (%d) while doing failover for Primary VM",
                     old_state);
        return;
    }
    /* Don't buffer any packets while exited COLO */
    qemu_set_default_filters_status(false);
    /* Flush the residuary buffered packts */
    qemu_release_default_filters_packets();

    bdrv_stop_replication_all(true, &local_err);
    if (local_err) {
        error_report_err(local_err);
    }
    trace_colo_stop_block_replication("failover");

    /* Notify COLO thread that failover work is finished */
    qemu_sem_post(&s->colo_sem);
}

void colo_do_failover(MigrationState *s)
{
    /* Make sure vm stopped while failover */
    if (!colo_runstate_is_stopped()) {
        vm_stop_force_state(RUN_STATE_COLO);
    }

    if (get_colo_mode() == COLO_MODE_PRIMARY) {
        primary_vm_do_failover();
    } else {
        secondary_vm_do_failover();
    }
}

static void colo_put_cmd(QEMUFile *f, COLOCommand cmd,
                         Error **errp)
{
    int ret;

    if (cmd >= COLO_COMMAND__MAX) {
        error_setg(errp, "%s: Invalid cmd", __func__);
        return;
    }
    qemu_put_be32(f, cmd);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Can't put COLO command");
    }
    trace_colo_put_cmd(COLOCommand_lookup[cmd]);
}

static void colo_put_cmd_value(QEMUFile *f, COLOCommand cmd,
                               uint64_t value, Error **errp)
{
    Error *local_err = NULL;
    int ret;

    colo_put_cmd(f, cmd, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qemu_put_be64(f, value);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Failed to send value for command:%s",
                         COLOCommand_lookup[cmd]);
    }
}

static COLOCommand colo_get_cmd(QEMUFile *f, Error **errp)
{
    COLOCommand cmd;
    int ret;

    cmd = qemu_get_be32(f);
    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Can't get COLO command");
        return cmd;
    }
    if (cmd >= COLO_COMMAND__MAX) {
        error_setg(errp, "%s: Invalid cmd", __func__);
        return cmd;
    }
    trace_colo_get_cmd(COLOCommand_lookup[cmd]);
    return cmd;
}

static void colo_get_check_cmd(QEMUFile *f, COLOCommand expect_cmd,
                               Error **errp)
{
    COLOCommand cmd;
    Error *local_err = NULL;

    cmd = colo_get_cmd(f, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (cmd != expect_cmd) {
        error_setg(errp, "Unexpected COLO command %d, expected %d",
                          expect_cmd, cmd);
    }
}

static uint64_t colo_get_cmd_value(QEMUFile *f, uint32_t expect_cmd,
                                   Error **errp)
{
    Error *local_err = NULL;
    uint64_t value;
    int ret;

    colo_get_check_cmd(f, expect_cmd, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return 0;
    }

    value = qemu_get_be64(f);
    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Failed to get value for COlO commnd: %s",
                         COLOCommand_lookup[expect_cmd]);
    }
    return value;
}

static int colo_do_checkpoint_transaction(MigrationState *s,
                                          QEMUSizedBuffer *buffer)
{
    int colo_shutdown;
    QEMUFile *trans = NULL;
    size_t size;
    Error *local_err = NULL;
    int ret = -1;

    colo_put_cmd(s->to_dst_file, COLO_COMMAND_CHECKPOINT_REQUEST,
                 &local_err);
    if (local_err) {
        goto out;
    }

    /* Reset colo buffer and open it for write */
    qsb_set_length(buffer, 0);
    trans = qemu_bufopen("w", buffer);
    if (!trans) {
        error_report("Open colo buffer for write failed");
        goto out;
    }

    qemu_mutex_lock_iothread();
    if (failover_request_is_active()) {
        qemu_mutex_unlock_iothread();
        goto out;
    }
    colo_shutdown = colo_shutdown_requested;
    vm_stop_force_state(RUN_STATE_COLO);
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("run", "stop");
    /*
     * failover request bh could be called after
     * vm_stop_force_state so we check failover_request_is_active() again.
     */
    if (failover_request_is_active()) {
        goto out;
    }

    /* we call this api although this may do nothing on primary side */
    qemu_mutex_lock_iothread();
    bdrv_do_checkpoint_all(&local_err);
    qemu_mutex_unlock_iothread();
    if (local_err) {
        goto out;
    }

    colo_put_cmd(s->to_dst_file, COLO_COMMAND_VMSTATE_SEND, &local_err);
    if (local_err) {
        goto out;
    }

    qemu_mutex_lock_iothread();
    /* Only save VM's live state, which not including device state */
    qemu_savevm_live_state(s->to_dst_file);
    /* Note: device state is saved into buffer */
    ret = qemu_save_device_state(trans);
    if (ret < 0) {
        error_report("save device state error\n");
        qemu_mutex_unlock_iothread();
        goto out;
    }
    qemu_fflush(trans);
    qemu_mutex_unlock_iothread();

    /* we send the total size of the vmstate first */
    size = qsb_get_length(buffer);
    colo_put_cmd_value(s->to_dst_file, COLO_COMMAND_VMSTATE_SIZE,
                       size, &local_err);
    if (local_err) {
        goto out;
    }

    qsb_put_buffer(s->to_dst_file, buffer, size);
    qemu_fflush(s->to_dst_file);
    ret = qemu_file_get_error(s->to_dst_file);
    if (ret < 0) {
        goto out;
    }

    colo_get_check_cmd(s->rp_state.from_dst_file,
                       COLO_COMMAND_VMSTATE_RECEIVED, &local_err);
    if (local_err) {
        goto out;
    }

    colo_get_check_cmd(s->rp_state.from_dst_file,
                       COLO_COMMAND_VMSTATE_LOADED, &local_err);
    if (local_err) {
        goto out;
    }

    qemu_release_default_filters_packets();

    if (colo_shutdown) {
        qemu_mutex_lock_iothread();
        bdrv_stop_replication_all(false, NULL);
        trace_colo_stop_block_replication("shutdown");
        qemu_mutex_unlock_iothread();
        colo_put_cmd(s->to_dst_file, COLO_COMMAND_GUEST_SHUTDOWN, &local_err);
        if (local_err) {
            goto out;
        }
        qemu_fflush(s->to_dst_file);
        colo_shutdown_requested = 0;
        qemu_system_shutdown_request_core();
        /* Fix me: Just let the colo thread exit ? */
        qemu_thread_exit(0);
    }

    ret = 0;
    /* Resume primary guest */
    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

out:
    if (local_err) {
        error_report_err(local_err);
    }
    if (trans) {
        qemu_fclose(trans);
    }
    return ret;
}

static int colo_prepare_before_save(MigrationState *s)
{
    int ret;

    /* Disable block migration */
    s->params.blk = 0;
    s->params.shared = 0;
    qemu_savevm_state_begin(s->to_dst_file, &s->params);
    ret = qemu_file_get_error(s->to_dst_file);
    if (ret < 0) {
        error_report("save vm state begin error");
    }
    return ret;
}

static int colo_init_buffer_filters(void)
{
    if (!qemu_netdev_support_netfilter()) {
        return -EPERM;
    }
    /* Begin to buffer packets that sent by VM */
    qemu_set_default_filters_status(true);

    return 0;
}

static void colo_process_checkpoint(MigrationState *s)
{
    QEMUSizedBuffer *buffer = NULL;
    int64_t current_time, checkpoint_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    Error *local_err = NULL;
    int ret;

    failover_init_state();
    ret = colo_init_buffer_filters();
    if (ret < 0) {
        goto out;
    }
    s->rp_state.from_dst_file = qemu_file_get_return_path(s->to_dst_file);
    if (!s->rp_state.from_dst_file) {
        error_report("Open QEMUFile from_dst_file failed");
        goto out;
    }

    ret = colo_prepare_before_save(s);
    if (ret < 0) {
        goto out;
    }

    /*
     * Wait for Secondary finish loading vm states and enter COLO
     * restore.
     */
    colo_get_check_cmd(s->rp_state.from_dst_file,
                       COLO_COMMAND_CHECKPOINT_READY, &local_err);
    if (local_err) {
        goto out;
    }

    buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    qemu_mutex_lock_iothread();
    /* start block replication */
    bdrv_start_replication_all(REPLICATION_MODE_PRIMARY, &local_err);
    if (local_err) {
        qemu_mutex_unlock_iothread();
        goto out;
    }
    trace_colo_start_block_replication();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

    ret = global_state_store();
    if (ret < 0) {
        goto out;
    }

    while (s->state == MIGRATION_STATUS_COLO) {
        if (failover_request_is_active()) {
            error_report("failover request");
            goto out;
        }

        current_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
        if ((current_time - checkpoint_time <
            s->parameters[MIGRATION_PARAMETER_X_CHECKPOINT_DELAY]) &&
            !colo_shutdown_requested) {
            int64_t delay_ms;

            delay_ms = s->parameters[MIGRATION_PARAMETER_X_CHECKPOINT_DELAY] -
                       (current_time - checkpoint_time);
            g_usleep(delay_ms * 1000);
        }
        /* start a colo checkpoint */
        ret = colo_do_checkpoint_transaction(s, buffer);
        if (ret < 0) {
            goto out;
        }
        checkpoint_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    }

out:
    /* Throw the unreported error message after exited from loop */
    if (local_err) {
        error_report_err(local_err);
    }
    /*
    * There are only two reasons we can go here, something error happened,
    * Or users triggered failover.
    */
    if (!failover_request_is_active()) {
        qapi_event_send_colo_exit(COLO_MODE_PRIMARY,
                                  COLO_EXIT_REASON_ERROR, NULL);
    } else {
        qapi_event_send_colo_exit(COLO_MODE_PRIMARY,
                                  COLO_EXIT_REASON_REQUEST, NULL);
    }

    qsb_free(buffer);
    buffer = NULL;

    /* Hope this not to be too long to wait here */
    qemu_sem_wait(&s->colo_sem);
    qemu_sem_destroy(&s->colo_sem);
    /*
    * Must be called after failover BH is completed,
    * Or the failover BH may shutdown the wrong fd, that
    * re-used by other thread after we release here.
    */
    if (s->rp_state.from_dst_file) {
        qemu_fclose(s->rp_state.from_dst_file);
    }
}

void migrate_start_colo_process(MigrationState *s)
{
    qemu_mutex_unlock_iothread();
    qemu_sem_init(&s->colo_sem, 0);
    migrate_set_state(&s->state, MIGRATION_STATUS_ACTIVE,
                      MIGRATION_STATUS_COLO);
    colo_process_checkpoint(s);
    qemu_mutex_lock_iothread();
}

static void colo_wait_handle_cmd(QEMUFile *f, int *checkpoint_request,
                                 Error **errp)
{
    COLOCommand cmd;
    Error *local_err = NULL;

    cmd = colo_get_cmd(f, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    switch (cmd) {
    case COLO_COMMAND_CHECKPOINT_REQUEST:
        *checkpoint_request = 1;
        break;
    case COLO_COMMAND_GUEST_SHUTDOWN:
        qemu_mutex_lock_iothread();
        vm_stop_force_state(RUN_STATE_COLO);
        bdrv_stop_replication_all(false, NULL);
        trace_colo_stop_block_replication("shutdown");
        qemu_system_shutdown_request_core();
        qemu_mutex_unlock_iothread();
        /* the main thread will exit and terminate the whole
        * process, do we need some cleanup?
        */
        qemu_thread_exit(0);
    default:
        *checkpoint_request = 0;
        error_setg(errp, "Got unknown COLO command: %d", cmd);
        break;
    }
}

static int colo_prepare_before_load(QEMUFile *f)
{
    int ret;

    ret = qemu_loadvm_state_begin(f);
    if (ret < 0) {
        error_report("load vm state begin error, ret=%d", ret);
    }
    return ret;
}

void *colo_process_incoming_thread(void *opaque)
{
    MigrationIncomingState *mis = opaque;
    QEMUFile *fb = NULL;
    QEMUSizedBuffer *buffer = NULL; /* Cache incoming device state */
    uint64_t total_size;
    uint64_t value;
    Error *local_err = NULL;
    int ret;

    qemu_sem_init(&mis->colo_incoming_sem, 0);

    migrate_set_state(&mis->state, MIGRATION_STATUS_ACTIVE,
                      MIGRATION_STATUS_COLO);

    failover_init_state();

    mis->to_src_file = qemu_file_get_return_path(mis->from_src_file);
    if (!mis->to_src_file) {
        error_report("colo incoming thread: Open QEMUFile to_src_file failed");
        goto out;
    }
    /* Note: We set the fd to unblocked in migration incoming coroutine,
    *  But here we are in the colo incoming thread, so it is ok to set the
    *  fd back to blocked.
    */
    qemu_file_set_blocking(mis->from_src_file, true);

    ret = colo_init_ram_cache();
    if (ret < 0) {
        error_report("Failed to initialize ram cache");
        goto out;
    }

    buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    ret = colo_prepare_before_load(mis->from_src_file);
    if (ret < 0) {
        goto out;
    }

    qemu_mutex_lock_iothread();
    /* start block replication */
    bdrv_start_replication_all(REPLICATION_MODE_SECONDARY, &local_err);
    qemu_mutex_unlock_iothread();
    if (local_err) {
        goto out;
    }
    trace_colo_start_block_replication();

    colo_put_cmd(mis->to_src_file, COLO_COMMAND_CHECKPOINT_READY,
                 &local_err);
    if (local_err) {
        goto out;
    }

    while (mis->state == MIGRATION_STATUS_COLO) {
        int request;

        colo_wait_handle_cmd(mis->from_src_file, &request, &local_err);
        if (local_err) {
            goto out;
        }
        assert(request);
        if (failover_request_is_active()) {
            error_report("failover request");
            goto out;
        }

        qemu_mutex_lock_iothread();
        vm_stop_force_state(RUN_STATE_COLO);
        trace_colo_vm_state_change("run", "stop");
        qemu_mutex_unlock_iothread();

        colo_get_check_cmd(mis->from_src_file,
                           COLO_COMMAND_VMSTATE_SEND, &local_err);
        if (local_err) {
            goto out;
        }

        ret = qemu_load_ram_state(mis->from_src_file);
        if (ret < 0) {
            error_report("load ram state error");
            goto out;
        }
        /* read the VM state total size first */
        value = colo_get_cmd_value(mis->from_src_file,
                                 COLO_COMMAND_VMSTATE_SIZE, &local_err);
        if (local_err) {
            goto out;
        }

        /* read vm device state into colo buffer */
        total_size = qsb_fill_buffer(buffer, mis->from_src_file, value);
        if (total_size != value) {
            error_report("Got %lu VMState data, less than expected %lu",
                         total_size, value);
            ret = -EINVAL;
            goto out;
        }

        colo_put_cmd(mis->to_src_file, COLO_COMMAND_VMSTATE_RECEIVED,
                     &local_err);
        if (local_err) {
            goto out;
        }

        /* open colo buffer for read */
        fb = qemu_bufopen("r", buffer);
        if (!fb) {
            error_report("Can't open colo buffer for read");
            goto out;
        }

        qemu_mutex_lock_iothread();
        qemu_system_reset(VMRESET_SILENT);
        vmstate_loading = true;
        colo_flush_ram_cache();
        ret = qemu_load_device_state(fb);
        if (ret < 0) {
            error_report("COLO: load device state failed\n");
            vmstate_loading = false;
            qemu_mutex_unlock_iothread();
            goto out;
        }
        /* discard colo disk buffer */
        bdrv_do_checkpoint_all(&local_err);
        if (local_err) {
            vmstate_loading = false;
            qemu_mutex_unlock_iothread();
            goto out;
        }

        vmstate_loading = false;
        qemu_mutex_unlock_iothread();

        if (failover_get_state() == FAILOVER_STATUS_RELAUNCH) {
            failover_set_state(FAILOVER_STATUS_RELAUNCH, FAILOVER_STATUS_NONE);
            failover_request_active(NULL);
            goto out;
        }

        colo_put_cmd(mis->to_src_file, COLO_COMMAND_VMSTATE_LOADED,
                     &local_err);
        if (local_err) {
            goto out;
        }

        qemu_mutex_lock_iothread();
        vm_start();
        trace_colo_vm_state_change("stop", "run");
        qemu_mutex_unlock_iothread();
        qemu_fclose(fb);
        fb = NULL;
    }

out:
    /* Throw the unreported error message after exited from loop */
    if (local_err) {
        error_report_err(local_err);
    }
    if (!failover_request_is_active()) {
        qapi_event_send_colo_exit(COLO_MODE_SECONDARY,
                                  COLO_EXIT_REASON_ERROR, NULL);
    } else {
        qapi_event_send_colo_exit(COLO_MODE_SECONDARY,
                                  COLO_EXIT_REASON_REQUEST, NULL);
    }

    if (fb) {
        qemu_fclose(fb);
    }
    qsb_free(buffer);
    /* Here, we can ensure BH is hold the global lock, and will join colo
    * incoming thread, so here it is not necessary to lock here again,
    * or there will be a deadlock error.
    */
    colo_release_ram_cache();

    /* Hope this not to be too long to loop here */
    qemu_sem_wait(&mis->colo_incoming_sem);
    qemu_sem_destroy(&mis->colo_incoming_sem);
    /* Must be called after failover BH is completed */
    if (mis->to_src_file) {
        qemu_fclose(mis->to_src_file);
    }
    migration_incoming_exit_colo();

    return NULL;
}

bool colo_shutdown(void)
{
    /*
    * if in colo mode, we need do some significant work before respond
    * to the shutdown request.
    */
    if (migration_incoming_in_colo_state()) {
        return true; /* primary's responsibility */
    }
    if (migration_in_colo_state()) {
        colo_shutdown_requested = 1;
        return true;
    }
    return false;
}
