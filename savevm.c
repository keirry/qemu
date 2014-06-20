/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config-host.h"
#include "qemu-common.h"
#include "hw/boards.h"
#include "hw/hw.h"
#include "hw/qdev.h"
#include "net/net.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qemu/timer.h"
#include "audio/audio.h"
#include "migration/migration.h"
#include "migration/postcopy-ram.h"
#include "qemu/sockets.h"
#include "qemu/queue.h"
#include "sysemu/cpus.h"
#include "exec/memory.h"
#include "qmp-commands.h"
#include "trace.h"
#include "qemu/bitops.h"
#include "qemu/iov.h"
#include "block/snapshot.h"
#include "block/qapi.h"

#ifdef DEBUG_SAVEVM
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "savevm@%" PRId64 " " fmt "\n", \
                          qemu_clock_get_ms(QEMU_CLOCK_REALTIME), \
                          ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif


#ifndef ETH_P_RARP
#define ETH_P_RARP 0x8035
#endif
#define ARP_HTYPE_ETH 0x0001
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST_REV 0x3

static int announce_self_create(uint8_t *buf,
                                uint8_t *mac_addr)
{
    /* Ethernet header. */
    memset(buf, 0xff, 6);         /* destination MAC addr */
    memcpy(buf + 6, mac_addr, 6); /* source MAC addr */
    *(uint16_t *)(buf + 12) = htons(ETH_P_RARP); /* ethertype */

    /* RARP header. */
    *(uint16_t *)(buf + 14) = htons(ARP_HTYPE_ETH); /* hardware addr space */
    *(uint16_t *)(buf + 16) = htons(ARP_PTYPE_IP); /* protocol addr space */
    *(buf + 18) = 6; /* hardware addr length (ethernet) */
    *(buf + 19) = 4; /* protocol addr length (IPv4) */
    *(uint16_t *)(buf + 20) = htons(ARP_OP_REQUEST_REV); /* opcode */
    memcpy(buf + 22, mac_addr, 6); /* source hw addr */
    memset(buf + 28, 0x00, 4);     /* source protocol addr */
    memcpy(buf + 32, mac_addr, 6); /* target hw addr */
    memset(buf + 38, 0x00, 4);     /* target protocol addr */

    /* Padding to get up to 60 bytes (ethernet min packet size, minus FCS). */
    memset(buf + 42, 0x00, 18);

    return 60; /* len (FCS will be added by hardware) */
}

static void qemu_announce_self_iter(NICState *nic, void *opaque)
{
    uint8_t buf[60];
    int len;

    trace_qemu_announce_self_iter(qemu_ether_ntoa(&nic->conf->macaddr));
    len = announce_self_create(buf, nic->conf->macaddr.a);

    qemu_send_packet_raw(qemu_get_queue(nic), buf, len);
}


static void qemu_announce_self_once(void *opaque)
{
    static int count = SELF_ANNOUNCE_ROUNDS;
    QEMUTimer *timer = *(QEMUTimer **)opaque;

    qemu_foreach_nic(qemu_announce_self_iter, NULL);

    if (--count) {
        /* delay 50ms, 150ms, 250ms, ... */
        timer_mod(timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  self_announce_delay(count));
    } else {
            timer_del(timer);
            timer_free(timer);
    }
}

void qemu_announce_self(void)
{
    static QEMUTimer *timer;
    timer = timer_new_ms(QEMU_CLOCK_REALTIME, qemu_announce_self_once, &timer);
    qemu_announce_self_once(&timer);
}

/***********************************************************/
/* savevm/loadvm support */

static ssize_t block_writev_buffer(void *opaque, struct iovec *iov, int iovcnt,
                                   int64_t pos)
{
    int ret;
    QEMUIOVector qiov;

    qemu_iovec_init_external(&qiov, iov, iovcnt);
    ret = bdrv_writev_vmstate(opaque, &qiov, pos);
    if (ret < 0) {
        return ret;
    }

    return qiov.size;
}

static int block_put_buffer(void *opaque, const uint8_t *buf,
                           int64_t pos, int size)
{
    bdrv_save_vmstate(opaque, buf, pos, size);
    return size;
}

static int block_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    return bdrv_load_vmstate(opaque, buf, pos, size);
}

static int bdrv_fclose(void *opaque)
{
    return bdrv_flush(opaque);
}

static const QEMUFileOps bdrv_read_ops = {
    .get_buffer = block_get_buffer,
    .close =      bdrv_fclose
};

static const QEMUFileOps bdrv_write_ops = {
    .put_buffer     = block_put_buffer,
    .writev_buffer  = block_writev_buffer,
    .close          = bdrv_fclose
};

static QEMUFile *qemu_fopen_bdrv(BlockDriverState *bs, int is_writable)
{
    if (is_writable) {
        return qemu_fopen_ops(bs, &bdrv_write_ops);
    }
    return qemu_fopen_ops(bs, &bdrv_read_ops);
}


/* QEMUFile timer support.
 * Not in qemu-file.c to not add qemu-timer.c as dependency to qemu-file.c
 */

void timer_put(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = timer_expire_time_ns(ts);
    qemu_put_be64(f, expire_time);
}

void timer_get(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = qemu_get_be64(f);
    if (expire_time != -1) {
        timer_mod_ns(ts, expire_time);
    } else {
        timer_del(ts);
    }
}


/* VMState timer support.
 * Not in vmstate.c to not add qemu-timer.c as dependency to vmstate.c
 */

static int get_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_get(f, v);
    return 0;
}

static void put_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_put(f, v);
}

const VMStateInfo vmstate_info_timer = {
    .name = "timer",
    .get  = get_timer,
    .put  = put_timer,
};


typedef struct CompatEntry {
    char idstr[256];
    int instance_id;
} CompatEntry;

typedef struct SaveStateEntry {
    QTAILQ_ENTRY(SaveStateEntry) entry;
    char idstr[256];
    int instance_id;
    int alias_id;
    int version_id;
    int section_id;
    SaveVMHandlers *ops;
    const VMStateDescription *vmsd;
    void *opaque;
    CompatEntry *compat;
    int is_ram;
} SaveStateEntry;


static QTAILQ_HEAD(savevm_handlers, SaveStateEntry) savevm_handlers =
    QTAILQ_HEAD_INITIALIZER(savevm_handlers);
static int global_section_id;

static void dump_vmstate_vmsd(FILE *out_file,
                              const VMStateDescription *vmsd, int indent,
                              bool is_subsection);

static void dump_vmstate_vmsf(FILE *out_file, const VMStateField *field,
                              int indent)
{
    fprintf(out_file, "%*s{\n", indent, "");
    indent += 2;
    fprintf(out_file, "%*s\"field\": \"%s\",\n", indent, "", field->name);
    fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
            field->version_id);
    fprintf(out_file, "%*s\"field_exists\": %s,\n", indent, "",
            field->field_exists ? "true" : "false");
    fprintf(out_file, "%*s\"size\": %zu", indent, "", field->size);
    if (field->vmsd != NULL) {
        fprintf(out_file, ",\n");
        dump_vmstate_vmsd(out_file, field->vmsd, indent, false);
    }
    fprintf(out_file, "\n%*s}", indent - 2, "");
}

static void dump_vmstate_vmss(FILE *out_file,
                              const VMStateSubsection *subsection,
                              int indent)
{
    if (subsection->vmsd != NULL) {
        dump_vmstate_vmsd(out_file, subsection->vmsd, indent, true);
    }
}

static void dump_vmstate_vmsd(FILE *out_file,
                              const VMStateDescription *vmsd, int indent,
                              bool is_subsection)
{
    if (is_subsection) {
        fprintf(out_file, "%*s{\n", indent, "");
    } else {
        fprintf(out_file, "%*s\"%s\": {\n", indent, "", "Description");
    }
    indent += 2;
    fprintf(out_file, "%*s\"name\": \"%s\",\n", indent, "", vmsd->name);
    fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
            vmsd->version_id);
    fprintf(out_file, "%*s\"minimum_version_id\": %d", indent, "",
            vmsd->minimum_version_id);
    if (vmsd->fields != NULL) {
        const VMStateField *field = vmsd->fields;
        bool first;

        fprintf(out_file, ",\n%*s\"Fields\": [\n", indent, "");
        first = true;
        while (field->name != NULL) {
            if (field->flags & VMS_MUST_EXIST) {
                /* Ignore VMSTATE_VALIDATE bits; these don't get migrated */
                field++;
                continue;
            }
            if (!first) {
                fprintf(out_file, ",\n");
            }
            dump_vmstate_vmsf(out_file, field, indent + 2);
            field++;
            first = false;
        }
        fprintf(out_file, "\n%*s]", indent, "");
    }
    if (vmsd->subsections != NULL) {
        const VMStateSubsection *subsection = vmsd->subsections;
        bool first;

        fprintf(out_file, ",\n%*s\"Subsections\": [\n", indent, "");
        first = true;
        while (subsection->vmsd != NULL) {
            if (!first) {
                fprintf(out_file, ",\n");
            }
            dump_vmstate_vmss(out_file, subsection, indent + 2);
            subsection++;
            first = false;
        }
        fprintf(out_file, "\n%*s]", indent, "");
    }
    fprintf(out_file, "\n%*s}", indent - 2, "");
}

static void dump_machine_type(FILE *out_file)
{
    MachineClass *mc;

    mc = MACHINE_GET_CLASS(current_machine);

    fprintf(out_file, "  \"vmschkmachine\": {\n");
    fprintf(out_file, "    \"Name\": \"%s\"\n", mc->name);
    fprintf(out_file, "  },\n");
}

void dump_vmstate_json_to_file(FILE *out_file)
{
    GSList *list, *elt;
    bool first;

    fprintf(out_file, "{\n");
    dump_machine_type(out_file);

    first = true;
    list = object_class_get_list(TYPE_DEVICE, true);
    for (elt = list; elt; elt = elt->next) {
        DeviceClass *dc = OBJECT_CLASS_CHECK(DeviceClass, elt->data,
                                             TYPE_DEVICE);
        const char *name;
        int indent = 2;

        if (!dc->vmsd) {
            continue;
        }

        if (!first) {
            fprintf(out_file, ",\n");
        }
        name = object_class_get_name(OBJECT_CLASS(dc));
        fprintf(out_file, "%*s\"%s\": {\n", indent, "", name);
        indent += 2;
        fprintf(out_file, "%*s\"Name\": \"%s\",\n", indent, "", name);
        fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
                dc->vmsd->version_id);
        fprintf(out_file, "%*s\"minimum_version_id\": %d,\n", indent, "",
                dc->vmsd->minimum_version_id);

        dump_vmstate_vmsd(out_file, dc->vmsd, indent, false);

        fprintf(out_file, "\n%*s}", indent - 2, "");
        first = false;
    }
    fprintf(out_file, "\n}\n");
    fclose(out_file);
}

static int calculate_new_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (strcmp(idstr, se->idstr) == 0
            && instance_id <= se->instance_id) {
            instance_id = se->instance_id + 1;
        }
    }
    return instance_id;
}

static int calculate_compat_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->compat) {
            continue;
        }

        if (strcmp(idstr, se->compat->idstr) == 0
            && instance_id <= se->compat->instance_id) {
            instance_id = se->compat->instance_id + 1;
        }
    }
    return instance_id;
}

/* TODO: Individual devices generally have very little idea about the rest
   of the system, so instance_id should be removed/replaced.
   Meanwhile pass -1 as instance_id if you do not already have a clearly
   distinguishing id for all instances of your device class. */
int register_savevm_live(DeviceState *dev,
                         const char *idstr,
                         int instance_id,
                         int version_id,
                         SaveVMHandlers *ops,
                         void *opaque)
{
    SaveStateEntry *se;

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = version_id;
    se->section_id = global_section_id++;
    se->ops = ops;
    se->opaque = opaque;
    se->vmsd = NULL;
    /* if this is a live_savem then set is_ram */
    if (ops->save_live_setup != NULL) {
        se->is_ram = 1;
    }

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), idstr);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(idstr) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), idstr);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_handlers, se, entry);
    return 0;
}

int register_savevm(DeviceState *dev,
                    const char *idstr,
                    int instance_id,
                    int version_id,
                    SaveStateHandler *save_state,
                    LoadStateHandler *load_state,
                    void *opaque)
{
    SaveVMHandlers *ops = g_malloc0(sizeof(SaveVMHandlers));
    ops->save_state = save_state;
    ops->load_state = load_state;
    return register_savevm_live(dev, idstr, instance_id, version_id,
                                ops, opaque);
}

void unregister_savevm(DeviceState *dev, const char *idstr, void *opaque)
{
    SaveStateEntry *se, *new_se;
    char id[256] = "";

    if (dev) {
        char *path = qdev_get_dev_path(dev);
        if (path) {
            pstrcpy(id, sizeof(id), path);
            pstrcat(id, sizeof(id), "/");
            g_free(path);
        }
    }
    pstrcat(id, sizeof(id), idstr);

    QTAILQ_FOREACH_SAFE(se, &savevm_handlers, entry, new_se) {
        if (strcmp(se->idstr, id) == 0 && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se->ops);
            g_free(se);
        }
    }
}

int vmstate_register_with_alias_id(DeviceState *dev, int instance_id,
                                   const VMStateDescription *vmsd,
                                   void *opaque, int alias_id,
                                   int required_for_version)
{
    SaveStateEntry *se;

    /* If this triggers, alias support can be dropped for the vmsd. */
    assert(alias_id == -1 || required_for_version >= vmsd->minimum_version_id);

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = vmsd->version_id;
    se->section_id = global_section_id++;
    se->opaque = opaque;
    se->vmsd = vmsd;
    se->alias_id = alias_id;

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), vmsd->name);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(vmsd->name) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), vmsd->name);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_handlers, se, entry);
    return 0;
}

void vmstate_unregister(DeviceState *dev, const VMStateDescription *vmsd,
                        void *opaque)
{
    SaveStateEntry *se, *new_se;

    QTAILQ_FOREACH_SAFE(se, &savevm_handlers, entry, new_se) {
        if (se->vmsd == vmsd && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se);
        }
    }
}

static int vmstate_load(QEMUFile *f, SaveStateEntry *se, int version_id)
{
    trace_vmstate_load(se->idstr, se->vmsd ? se->vmsd->name : "(old)");
    if (!se->vmsd) {         /* Old style */
        return se->ops->load_state(f, se->opaque, version_id);
    }
    return vmstate_load_state(f, se->vmsd, se->opaque, version_id);
}

static void vmstate_save(QEMUFile *f, SaveStateEntry *se)
{
    trace_vmstate_save(se->idstr, se->vmsd ? se->vmsd->name : "(old)");
    if (!se->vmsd) {         /* Old style */
        se->ops->save_state(f, se->opaque);
        return;
    }
    vmstate_save_state(f, se->vmsd, se->opaque);
}


/* Send a 'QEMU_VM_COMMAND' type element with the command
 * and associated data.
 */
void qemu_savevm_command_send(QEMUFile *f,
                              enum qemu_vm_cmd command,
                              uint16_t len,
                              uint8_t *data)
{
    uint32_t tmp = (uint16_t)command;
    qemu_put_byte(f, QEMU_VM_COMMAND);
    qemu_put_be16(f, tmp);
    qemu_put_be16(f, len);
    if (len) {
        qemu_put_buffer(f, data, len);
    }
    qemu_fflush(f);
}

void qemu_savevm_send_reqack(QEMUFile *f, uint32_t value)
{
    uint32_t buf;

    DPRINTF("send_reqack %d", value);
    buf = cpu_to_be32(value);
    qemu_savevm_command_send(f, QEMU_VM_CMD_REQACK, 4, (uint8_t *)&buf);
}

void qemu_savevm_send_openrp(QEMUFile *f)
{
    qemu_savevm_command_send(f, QEMU_VM_CMD_OPENRP, 0, NULL);
}

/* We have a buffer of data to send; we don't want that all to be loaded
 * by the command itself, so the command contains just the length of the
 * extra buffer that we then send straight after it.
 * TODO: Must be a better way to organise that
 */
void qemu_savevm_send_packaged(QEMUFile *f, const QEMUSizedBuffer *qsb)
{
    size_t cur_iov;
    size_t len = qsb_get_length(qsb);
    uint32_t tmp;

    tmp = cpu_to_be32(len);

    DPRINTF("send_packaged");
    qemu_savevm_command_send(f, QEMU_VM_CMD_PACKAGED, 4, (uint8_t *)&tmp);

    /* all the data follows (concatinating the iov's) */
    for (cur_iov = 0; cur_iov < qsb->n_iov; cur_iov++) {
        /* The iov entries are partially filled */
        size_t towrite = (qsb->iov[cur_iov].iov_len > len) ?
                              len :
                              qsb->iov[cur_iov].iov_len;
        len -= towrite;

        if (!towrite) {
            break;
        }

        qemu_put_buffer(f, qsb->iov[cur_iov].iov_base, towrite);
    }
}

/* Send prior to any RAM transfer */
void qemu_savevm_send_postcopy_ram_advise(QEMUFile *f)
{
    DPRINTF("send postcopy-ram-advise");
    qemu_savevm_command_send(f, QEMU_VM_CMD_POSTCOPY_RAM_ADVISE, 0, NULL);
}

/* Prior to running, to cause pages that have been dirtied after precopy
 * started to be discarded on the destination.
 * CMD_POSTCOPY_RAM_DISCARD consist of:
 *  2 byte header (filled in by qemu_savevm_send_postcopy_ram_discard)
 *      byte   version (0)
 *      byte   offset into the 1st data word containing 1st page of RAMBlock
 *      byte   Length of name field
 *  n x byte   RAM block name (NOT 0 terminated)
 *  n x
 *      be64   Page addresses for start of an invalidation range
 *      be64   mask of 64 pages, '1' to discard'
 *
 *  Hopefully this is pretty sparse so we don't get too many entries,
 *  and using the mask should deal with most pagesize differences
 *  just ending up as a single full mask
 *
 *  The mask is always 64bits irrespective of the long size
 *
 *  Note the destination is free to discard *more* than we've asked
 *  (e.g. rounding up to some convenient page size)
 *
 *  name:  RAMBlock name that these entries are part of
 *  len: Number of page entries
 *  pagelist: one 8byte header word (empty) then len*(start,mask) pairs
 *            The caller must have already put these into be64 format
 */
void qemu_savevm_send_postcopy_ram_discard(QEMUFile *f, const char *name,
                                           uint16_t len, uint8_t offset,
                                           uint64_t *pagelist)
{
    uint8_t *buf;
    uint16_t tmplen;

    DPRINTF("send postcopy-ram-discard");
    buf = g_malloc0(len*16 + strlen(name) + 3);
    buf[0] = 0; /* Version */
    buf[1] = offset;
    assert(strlen(name) < 256);
    buf[2] = strlen(name);
    memcpy(buf+3, name, strlen(name));
    tmplen = 3+strlen(name);
    memcpy(buf + tmplen, pagelist, len*16);

    qemu_savevm_command_send(f, QEMU_VM_CMD_POSTCOPY_RAM_DISCARD,
                             tmplen + len*16, buf);
    g_free(buf);
}

/* Get the destination into a state where it can receive page data. */
void qemu_savevm_send_postcopy_ram_listen(QEMUFile *f)
{
    DPRINTF("send postcopy-ram-listen");
    qemu_savevm_command_send(f, QEMU_VM_CMD_POSTCOPY_RAM_LISTEN, 0, NULL);
}

/* Kick the destination into running */
void qemu_savevm_send_postcopy_ram_run(QEMUFile *f)
{
    DPRINTF("send postcopy-ram-run");
    qemu_savevm_command_send(f, QEMU_VM_CMD_POSTCOPY_RAM_RUN, 0, NULL);
}

/* End of postcopy - with a status byte; 0 is good, anything else is a fail */
void qemu_savevm_send_postcopy_ram_end(QEMUFile *f, uint8_t status)
{
    DPRINTF("send postcopy-ram-end");
    qemu_savevm_command_send(f, QEMU_VM_CMD_POSTCOPY_RAM_END, 1, &status);
}

bool qemu_savevm_state_blocked(Error **errp)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (se->vmsd && se->vmsd->unmigratable) {
            error_setg(errp, "State blocked by non-migratable device '%s'",
                       se->idstr);
            return true;
        }
    }
    return false;
}

void qemu_savevm_state_begin(QEMUFile *f,
                             const MigrationParams *params)
{
    SaveStateEntry *se;
    int ret;

    trace_savevm_state_begin();
    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->set_params) {
            continue;
        }
        se->ops->set_params(params, se->opaque);
    }

    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        int len;

        if (!se->ops || !se->ops->save_live_setup) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_START);
        qemu_put_be32(f, se->section_id);

        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        ret = se->ops->save_live_setup(f, se->opaque);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
            break;
        }
    }
}

/*
 * this function has three return values:
 *   negative: there was one error, and we have -errno.
 *   0 : We haven't finished, caller have to go again
 *   1 : We have finished, we can go to complete phase
 */
int qemu_savevm_state_iterate(QEMUFile *f)
{
    SaveStateEntry *se;
    int ret = 1;

    trace_savevm_state_iterate();
    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->save_live_iterate) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        if (qemu_file_rate_limit(f)) {
            return 0;
        }
        trace_savevm_section_start(se->idstr, se->section_id);
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_PART);
        qemu_put_be32(f, se->section_id);

        ret = se->ops->save_live_iterate(f, se->opaque);
        trace_savevm_section_end(se->idstr, se->section_id);

        if (ret < 0) {
            DPRINTF("%s: setting error state after iterate on id=%d/%s",
                    __func__, se->section_id, se->idstr);
            qemu_file_set_error(f, ret);
        }
        if (ret <= 0) {
            /* Do not proceed to the next vmstate before this one reported
               completion of the current stage. This serializes the migration
               and reduces the probability that a faster changing state is
               synchronized over and over again. */
            break;
        }
    }
    return ret;
}

/*
 * Calls the complete routines just for those devices that are postcopiable;
 * causing the last few pages to be sent immediately and doing any associated
 * cleanup.
 * Note postcopy also calls the plain qemu_savevm_state_complete to complete
 * all the other devices, but that happens at the point we switch to postcopy.
 */
void qemu_savevm_state_postcopy_complete(QEMUFile *f)
{
    SaveStateEntry *se;
    int ret;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->save_live_complete ||
            !se->ops->can_postcopy) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        trace_savevm_section_start(se->idstr, se->section_id);
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_END);
        qemu_put_be32(f, se->section_id);

        ret = se->ops->save_live_complete(f, se->opaque);
        trace_savevm_section_end(se->idstr, se->section_id);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
            return;
        }
    }

    qemu_savevm_send_postcopy_ram_end(f, 0 /* Good */);
    qemu_put_byte(f, QEMU_VM_EOF);
    qemu_fflush(f);
}

void qemu_savevm_state_complete(QEMUFile *f)
{
    SaveStateEntry *se;
    int ret;
    bool in_postcopy = migration_postcopy_phase(migrate_get_current());

    trace_savevm_state_complete();

    cpu_synchronize_all_states();

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->save_live_complete) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        if (in_postcopy && se->ops &&  se->ops->can_postcopy &&
            se->ops->can_postcopy(se->opaque)) {
            DPRINTF("%s: Skipping %s in postcopy", __func__, se->idstr);
            continue;
        }
        trace_savevm_section_start(se->idstr, se->section_id);
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_END);
        qemu_put_be32(f, se->section_id);

        ret = se->ops->save_live_complete(f, se->opaque);
        trace_savevm_section_end(se->idstr, se->section_id);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
            return;
        }
    }

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        int len;

        if ((!se->ops || !se->ops->save_state) && !se->vmsd) {
            continue;
        }
        trace_savevm_section_start(se->idstr, se->section_id);
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_FULL);
        qemu_put_be32(f, se->section_id);

        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        vmstate_save(f, se);
        trace_savevm_section_end(se->idstr, se->section_id);
    }

    if (!in_postcopy) {
        /* Postcopy stream will still be going */
        qemu_put_byte(f, QEMU_VM_EOF);
    }

    qemu_fflush(f);
}

/* Give an estimate of the amount left to be transferred,
 * the result is split into the amount for units that can and
 * for units that can't do postcopy.
 */
void qemu_savevm_state_pending(QEMUFile *f, uint64_t max_size,
                               uint64_t *res_non_postcopiable,
                               uint64_t *res_postcopiable)
{
    SaveStateEntry *se;
    uint64_t res_nonpc = 0;
    uint64_t res_pc = 0;
    uint64_t tmp;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->save_live_pending) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        tmp = se->ops->save_live_pending(f, se->opaque, max_size);

        if (se->ops->can_postcopy(se->opaque)) {
            res_pc += tmp;
        } else {
            res_nonpc += tmp;
        }
    }
    *res_non_postcopiable = res_nonpc;
    *res_postcopiable = res_pc;
}

void qemu_savevm_state_cancel(void)
{
    SaveStateEntry *se;

    trace_savevm_state_cancel();
    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (se->ops && se->ops->cancel) {
            se->ops->cancel(se->opaque);
        }
    }
}

static int qemu_savevm_state(QEMUFile *f)
{
    int ret;
    MigrationParams params = {
        .blk = 0,
        .shared = 0
    };
    MigrationState *ms = migrate_init(&params);
    ms->file = f;

    if (qemu_savevm_state_blocked(NULL)) {
        return -EINVAL;
    }

    qemu_mutex_unlock_iothread();
    qemu_savevm_state_begin(f, &params);
    qemu_mutex_lock_iothread();

    while (qemu_file_get_error(f) == 0) {
        if (qemu_savevm_state_iterate(f) > 0) {
            break;
        }
    }

    ret = qemu_file_get_error(f);
    if (ret == 0) {
        qemu_savevm_state_complete(f);
        ret = qemu_file_get_error(f);
    }
    if (ret != 0) {
        qemu_savevm_state_cancel();
    }
    return ret;
}

static int qemu_save_device_state(QEMUFile *f)
{
    SaveStateEntry *se;

    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);

    cpu_synchronize_all_states();

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        int len;

        if (se->is_ram) {
            continue;
        }
        if ((!se->ops || !se->ops->save_state) && !se->vmsd) {
            continue;
        }

        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_FULL);
        qemu_put_be32(f, se->section_id);

        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        vmstate_save(f, se);
    }

    qemu_put_byte(f, QEMU_VM_EOF);

    return qemu_file_get_error(f);
}

static SaveStateEntry *find_se(const char *idstr, int instance_id)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!strcmp(se->idstr, idstr) &&
            (instance_id == se->instance_id ||
             instance_id == se->alias_id))
            return se;
        /* Migrating from an older version? */
        if (strstr(se->idstr, idstr) && se->compat) {
            if (!strcmp(se->compat->idstr, idstr) &&
                (instance_id == se->compat->instance_id ||
                 instance_id == se->alias_id))
                return se;
        }
    }
    return NULL;
}

/* These are ORable flags */
const int LOADVM_EXITCODE_QUITLOOP     =  1;
const int LOADVM_EXITCODE_QUITPARENT   =  2;
const int LOADVM_EXITCODE_KEEPHANDLERS =  4;

typedef struct LoadStateEntry {
    QLIST_ENTRY(LoadStateEntry) entry;
    SaveStateEntry *se;
    int section_id;
    int version_id;
} LoadStateEntry;

typedef QLIST_HEAD(, LoadStateEntry) LoadStateEntry_Head;

static LoadStateEntry_Head loadvm_handlers =
 QLIST_HEAD_INITIALIZER(loadvm_handlers);

static int qemu_loadvm_state_main(QEMUFile *f,
                                  LoadStateEntry_Head *loadvm_handlers);

/* ------ incoming postcopy-ram messages ------ */
/* 'advise' arrives before any RAM transfers just to tell us that a postcopy
 * *might* happen - it might be skipped if precopy transferred everything
 * quickly.
 */
static int loadvm_postcopy_ram_handle_advise(MigrationIncomingState *mis)
{
    DPRINTF("%s", __func__);
    if (mis->postcopy_ram_state != POSTCOPY_RAM_INCOMING_NONE) {
        error_report("CMD_POSTCOPY_RAM_ADVISE in wrong postcopy state (%d)",
                     mis->postcopy_ram_state);
        return -1;
    }

    /* Check this host can do it */
    if (postcopy_ram_hosttest()) {
        return -1;
    }

    if (ram_postcopy_incoming_init(mis)) {
        return -1;
    }

    mis->postcopy_ram_state = POSTCOPY_RAM_INCOMING_ADVISE;

    /*
     * Postcopy will be sending lots of small messages along the return path
     * that it needs quick answers to.
     */
    socket_set_nodelay(qemu_get_fd(mis->return_path));

    return 0;
}

/* After postcopy we will be told to throw some pages away since they're
 * dirty and will have to be demand fetched.  Must happen before CPU is
 * started.
 * There can be 0..many of these messages, each encoding multiple pages.
 * Bits set in the message represent a page in the source VMs bitmap, but
 * since the guest/target page sizes can be different on s/d then we have
 * to convert.
 */
static int loadvm_postcopy_ram_handle_discard(MigrationIncomingState *mis,
                                              uint16_t len)
{
    int tmp;
    const int source_target_page_bits = 12; /* TODO */
    unsigned int first_bit_offset;
    char ramid[256];

    DPRINTF("%s", __func__);

    if (mis->postcopy_ram_state != POSTCOPY_RAM_INCOMING_ADVISE) {
        error_report("CMD_POSTCOPY_RAM_DISCARD in wrong postcopy state (%d)",
                     mis->postcopy_ram_state);
        return -1;
    }
    /* We're expecting a
     *    3 byte header,
     *    a RAM ID string
     *    then at least 1 2x8 byte chunks
    */
    if (len < 19) {
        error_report("CMD_POSTCOPY_RAM_DISCARD invalid length (%d)", len);
        return -1;
    }

    tmp = qemu_get_byte(mis->file);
    if (tmp != 0) {
        error_report("CMD_POSTCOPY_RAM_DISCARD invalid version (%d)", tmp);
        return -1;
    }
    first_bit_offset = qemu_get_byte(mis->file);

    if (qemu_get_counted_string(mis->file, (uint8_t *)ramid)) {
        error_report("CMD_POSTCOPY_RAM_DISCARD Failed to read RAMBlock ID");
        return -1;
    }

    len -= 3+strlen(ramid);
    if (len & 15) {
        error_report("CMD_POSTCOPY_RAM_DISCARD invalid length (%d)", len);
        return -1;
    }
    while (len) {
        uint64_t startaddr, mask;
        /*
         * We now have pairs of address, mask
         *   The address is in multiples of 64bit chunks in the source bitmask
         *     ie multiply by 64 and then source-target-page-size to get bytes
         *     '0' represents the chunk in which the RAMBlock starts for the
         *     source and 'first_bit_offset' (see above) represents which bit in
         *     that first word corresponds to the first page of the RAMBlock
         *   The mask is 64 bits of bitmask starting at that offset into the
         *   RAMBlock.
         *
         *   For example:
         *      an address of 1 with a first_bit_offset of 12 indicates
         *      page 1*64 - 12 = page 52 for bit 0 of the mask
         *      Source guarantees that for address 0, bits <first_bit_offset
         *      shall be 0
         */
        startaddr = qemu_get_be64(mis->file) * 64;
        mask = qemu_get_be64(mis->file);

        len -= 16;

        while (mask) {
            /* mask= .....?10...0 */
            /*             ^fs    */
            int firstset = ctz64(mask);

            /* tmp64=.....?11...1 */
            /*             ^fs    */
            uint64_t tmp64 = mask | ((((uint64_t)1)<<firstset)-1);

            /* mask= .?01..10...0 */
            /*         ^fz ^fs    */
            int firstzero = cto64(tmp64);

            if ((startaddr == 0) && (firstset < first_bit_offset)) {
                error_report("CMD_POSTCOPY_RAM_DISCARD bad data; bit set"
                               " prior to block; block=%s offset=%d"
                               " firstset=%d\n", ramid, first_bit_offset,
                               firstzero);
                return -1;
            }
            /*
             * we know there must be at least 1 bit set due to the loop entry
             * If there is no 0 firstzero will be 64
             */
            /* TODO - ram_discard_range gets added in a later patch
            int ret = ram_discard_range(mis, ramid, source_target_page_bits,
                                startaddr + firstset - first_bit_offset,
                                startaddr + (firstzero - 1) - first_bit_offset);
             */
            ret = -1; /* TODO */
            if (ret) {
                return ret;
            }

            /* mask= .?0000000000 */
            /*         ^fz ^fs    */
            if (firstzero != 64) {
                mask &= (((uint64_t)-1) << firstzero);
            } else {
                mask = 0;
            }
        }
    }
    DPRINTF("%s finished", __func__);

    return 0;
}

/* After this message we must be able to immediately receive page data */
static int loadvm_postcopy_ram_handle_listen(MigrationIncomingState *mis)
{
    DPRINTF("%s", __func__);
    if (mis->postcopy_ram_state != POSTCOPY_RAM_INCOMING_ADVISE) {
        error_report("CMD_POSTCOPY_RAM_LISTEN in wrong postcopy state (%d)",
                     mis->postcopy_ram_state);
        return -1;
    }

    mis->postcopy_ram_state = POSTCOPY_RAM_INCOMING_LISTENING;

    /*
     * Sensitise RAM - can now generate requests for blocks that don't exist
     * However, at this point the CPU shouldn't be running, and the IO
     * shouldn't be doing anything yet so don't actually expect requests
     */
    if (postcopy_ram_enable_notify(mis)) {
        return -1;
    }

    /* TODO start up the postcopy listening thread */
    return 0;
}

/* After all discards we can start running and asking for pages */
static int loadvm_postcopy_ram_handle_run(MigrationIncomingState *mis)
{
    DPRINTF("%s", __func__);
    if (mis->postcopy_ram_state != POSTCOPY_RAM_INCOMING_LISTENING) {
        error_report("CMD_POSTCOPY_RAM_RUN in wrong postcopy state (%d)",
                     mis->postcopy_ram_state);
        return -1;
    }

    mis->postcopy_ram_state = POSTCOPY_RAM_INCOMING_RUNNING;
    if (autostart) {
        /* Hold onto your hats, starting the CPU */
        vm_start();
    } else {
        /* leave it paused and let management decide when to start the CPU */
        runstate_set(RUN_STATE_PAUSED);
    }

    return 0;
}

/* The end - with a byte from the source which can tell us to fail. */
static int loadvm_postcopy_ram_handle_end(MigrationIncomingState *mis)
{
    DPRINTF("%s", __func__);
    if (mis->postcopy_ram_state == POSTCOPY_RAM_INCOMING_NONE) {
        error_report("CMD_POSTCOPY_RAM_END in wrong postcopy state (%d)",
                     mis->postcopy_ram_state);
        return -1;
    }
    return -1; /* TODO - expecting 1 byte good/fail */
}

static int loadvm_process_command_simple_lencheck(const char *name,
                                                  unsigned int actual,
                                                  unsigned int expected)
{
    if (actual != expected) {
        error_report("%s received with bad length - expecting %d, got %d",
                     name, expected, actual);
        return -1;
    }

    return 0;
}

/* Immediately following this command is a blob of data containing an embedded
 * chunk of migration stream; read it and load it.
 */
static int loadvm_handle_cmd_packaged(MigrationIncomingState *mis,
                                      uint32_t length,
                                      LoadStateEntry_Head *loadvm_handlers)
{
    int ret;
    uint8_t *buffer;
    QEMUSizedBuffer *qsb;

    DPRINTF("loadvm_handle_cmd_packaged: length=%u", length);

    if (length > MAX_VM_CMD_PACKAGED_SIZE) {
        error_report("Unreasonably large packaged state: %u", length);
        return -1;
    }
    buffer = g_malloc0(length);
    ret = qemu_get_buffer(mis->file, buffer, (int)length);
    if (ret != length) {
        g_free(buffer);
        error_report("CMD_PACKAGED: Buffer receive fail ret=%d length=%d\n",
                ret, length);
        return (ret < 0) ? ret : -EAGAIN;
    }
    DPRINTF("%s: Received %d package, going to load", __func__, ret);

    /* Setup a dummy QEMUFile that actually reads from the buffer */
    qsb = qsb_create(buffer, length);
    g_free(buffer); /* Because qsb_create copies */
    QEMUFile *packf = qemu_bufopen("r", qsb);

    ret = qemu_loadvm_state_main(packf, loadvm_handlers);
    DPRINTF("%s: qemu_loadvm_state_main returned %d", __func__, ret);
    qemu_fclose(packf); /* also frees the qsb */

    return ret;
}

/*
 * Process an incoming 'QEMU_VM_COMMAND'
 * negative return on error (will issue error message)
 * 0   just a normal return
 * 1   All good, but exit the loop
 */
static int loadvm_process_command(QEMUFile *f,
                                  LoadStateEntry_Head *loadvm_handlers)
{
    MigrationIncomingState *mis = migration_incoming_get_current();
    uint16_t com;
    uint16_t len;
    uint32_t tmp32;

    com = qemu_get_be16(f);
    len = qemu_get_be16(f);

    /* fprintf(stderr,"loadvm_process_command: com=0x%x len=%d\n", com,len); */
    switch (com) {
    case QEMU_VM_CMD_OPENRP:
        if (loadvm_process_command_simple_lencheck("CMD_OPENRP", len, 0)) {
            return -1;
        }
        if (mis->return_path) {
            error_report("CMD_OPENRP called when RP already open");
            /* Not really a problem, so don't give up */
            return 0;
        }
        mis->return_path = qemu_file_get_return_path(f);
        if (!mis->return_path) {
            error_report("CMD_OPENRP failed - could not open return path");
            return -1;
        }
        break;

    case QEMU_VM_CMD_REQACK:
        if (loadvm_process_command_simple_lencheck("CMD_REQACK", len, 4)) {
            return -1;
        }
        tmp32 = qemu_get_be32(f);
        DPRINTF("Received REQACK 0x%x", tmp32);
        if (!mis->return_path) {
            error_report("CMD_REQACK (0x%x) received with no open return path",
                         tmp32);
            return -1;
        }
        migrate_send_rp_ack(mis, tmp32);
        break;

    case QEMU_VM_CMD_PACKAGED:
        if (loadvm_process_command_simple_lencheck("CMD_POSTCOPY_RAM_ADVISE",
            len, 4)) {
            return -1;
         }
        tmp32 = qemu_get_be32(f);
        return loadvm_handle_cmd_packaged(mis, tmp32, loadvm_handlers);

    case QEMU_VM_CMD_POSTCOPY_RAM_ADVISE:
        if (loadvm_process_command_simple_lencheck("CMD_POSTCOPY_RAM_ADVISE",
                                                   len, 0)) {
            return -1;
        }
        return loadvm_postcopy_ram_handle_advise(mis);

    case QEMU_VM_CMD_POSTCOPY_RAM_DISCARD:
        return loadvm_postcopy_ram_handle_discard(mis, len);

    case QEMU_VM_CMD_POSTCOPY_RAM_LISTEN:
        if (loadvm_process_command_simple_lencheck("CMD_POSTCOPY_RAM_LISTEN",
                                                   len, 0)) {
            return -1;
        }
        return loadvm_postcopy_ram_handle_listen(mis);

    case QEMU_VM_CMD_POSTCOPY_RAM_RUN:
        if (loadvm_process_command_simple_lencheck("CMD_POSTCOPY_RAM_RUN",
                                                   len, 0)) {
            return -1;
        }
        return loadvm_postcopy_ram_handle_run(mis);

    case QEMU_VM_CMD_POSTCOPY_RAM_END:
        if (loadvm_process_command_simple_lencheck("CMD_POSTCOPY_RAM_END",
                                                   len, 1)) {
            return -1;
        }
        return loadvm_postcopy_ram_handle_end(mis);

    default:
        error_report("VM_COMMAND 0x%x unknown (len 0x%x)", com, len);
        return -1;
    }

    return 0;
}

static int qemu_loadvm_state_main(QEMUFile *f,
                                  LoadStateEntry_Head *loadvm_handlers)
{
    LoadStateEntry *le;
    uint8_t section_type;
    int ret;
    int exitcode = 0;

    while ((section_type = qemu_get_byte(f)) != QEMU_VM_EOF) {
        uint32_t instance_id, version_id, section_id;
        SaveStateEntry *se;
        char idstr[256];

        DPRINTF("qemu_loadvm_state loop: section_type=%d", section_type);
        switch (section_type) {
        case QEMU_VM_SECTION_START:
        case QEMU_VM_SECTION_FULL:
            /* Read section start */
            section_id = qemu_get_be32(f);
            if (qemu_get_counted_string(f, (uint8_t *)idstr)) {
                error_report("Unable to read ID string for section %u",
                            section_id);
                return -EINVAL;
            }
            instance_id = qemu_get_be32(f);
            version_id = qemu_get_be32(f);

            DPRINTF("qemu_loadvm_state loop START/FULL: id=%d(%s)",
                    section_id, idstr);

            /* Find savevm section */
            se = find_se(idstr, instance_id);
            if (se == NULL) {
                error_report("Unknown savevm section or instance '%s' %d",
                             idstr, instance_id);
                return -EINVAL;
            }

            /* Validate version */
            if (version_id > se->version_id) {
                error_report("savevm: unsupported version %d for '%s' v%d",
                        version_id, idstr, se->version_id);
                return -EINVAL;
            }

            /* Add entry */
            le = g_malloc0(sizeof(*le));

            le->se = se;
            le->section_id = section_id;
            le->version_id = version_id;
            QLIST_INSERT_HEAD(loadvm_handlers, le, entry);

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                error_report("qemu: error while loading state for"
                             "instance 0x%x of device '%s'",
                             instance_id, idstr);
                return ret;
            }
            break;
        case QEMU_VM_SECTION_PART:
        case QEMU_VM_SECTION_END:
            section_id = qemu_get_be32(f);

            DPRINTF("QEMU_VM_SECTION_PART/END entry for id=%d", section_id);
            QLIST_FOREACH(le, loadvm_handlers, entry) {
                if (le->section_id == section_id) {
                    break;
                }
            }
            if (le == NULL) {
                error_report("Unknown savevm section %d", section_id);
                return -EINVAL;
            }

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                error_report("qemu: error while loading state section"
                             " id %d (%s)", section_id, le->se->idstr);
                return ret;
            }
            DPRINTF("QEMU_VM_SECTION_PART/END done for id=%d", section_id);
            break;
        case QEMU_VM_COMMAND:
            ret = loadvm_process_command(f, loadvm_handlers);
            DPRINTF("%s QEMU_VM_COMMAND ret: %d", __func__, ret);
            if ((ret < 0) || (ret & LOADVM_EXITCODE_QUITLOOP)) {
                return ret;
            }
            exitcode |= ret; /* Lets us pass flags up to the parent */
            break;
        default:
            error_report("Unknown savevm section type %d", section_type);
            return -EINVAL;
        }
    }
    DPRINTF("qemu_loadvm_state loop: exited loop");

    if (exitcode & LOADVM_EXITCODE_QUITPARENT) {
        DPRINTF("loadvm_handlers_state_main: End of loop with QUITPARENT");
        exitcode &= ~LOADVM_EXITCODE_QUITPARENT;
        exitcode &= LOADVM_EXITCODE_QUITLOOP;
    }

    return exitcode;
}

int qemu_loadvm_state(QEMUFile *f)
{
    LoadStateEntry *le, *new_le;
    unsigned int v;
    int ret;

    if (qemu_savevm_state_blocked(NULL)) {
        return -EINVAL;
    }

    v = qemu_get_be32(f);
    if (v != QEMU_VM_FILE_MAGIC) {
        return -EINVAL;
    }

    v = qemu_get_be32(f);
    if (v == QEMU_VM_FILE_VERSION_COMPAT) {
        error_report("SaveVM v2 format is obsolete and don't work anymore");
        return -ENOTSUP;
    }
    if (v != QEMU_VM_FILE_VERSION) {
        return -ENOTSUP;
    }

    QLIST_INIT(&loadvm_handlers);
    ret = qemu_loadvm_state_main(f, &loadvm_handlers);

    if (ret == 0) {
        cpu_synchronize_all_post_init();
    }

    if ((ret < 0) || !(ret & LOADVM_EXITCODE_KEEPHANDLERS)) {
        QLIST_FOREACH_SAFE(le, &loadvm_handlers, entry, new_le) {
            QLIST_REMOVE(le, entry);
            g_free(le);
        }
    }

    if (ret == 0) {
        ret = qemu_file_get_error(f);
    }

    DPRINTF("qemu_loadvm_state out: ret=%d", ret);
    return ret;
}

static BlockDriverState *find_vmstate_bs(void)
{
    BlockDriverState *bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs)) {
            return bs;
        }
    }
    return NULL;
}

/*
 * Deletes snapshots of a given name in all opened images.
 */
static int del_existing_snapshots(Monitor *mon, const char *name)
{
    BlockDriverState *bs;
    QEMUSnapshotInfo sn1, *snapshot = &sn1;
    Error *err = NULL;

    bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs) &&
            bdrv_snapshot_find(bs, snapshot, name) >= 0) {
            bdrv_snapshot_delete_by_id_or_name(bs, name, &err);
            if (err) {
                monitor_printf(mon,
                               "Error while deleting snapshot on device '%s':"
                               " %s\n",
                               bdrv_get_device_name(bs),
                               error_get_pretty(err));
                error_free(err);
                return -1;
            }
        }
    }

    return 0;
}

void do_savevm(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo sn1, *sn = &sn1, old_sn1, *old_sn = &old_sn1;
    int ret;
    QEMUFile *f;
    int saved_vm_running;
    uint64_t vm_state_size;
    qemu_timeval tv;
    struct tm tm;
    const char *name = qdict_get_try_str(qdict, "name");

    /* Verify if there is a device that doesn't support snapshots and is writable */
    bs = NULL;
    while ((bs = bdrv_next(bs))) {

        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
            continue;
        }

        if (!bdrv_can_snapshot(bs)) {
            monitor_printf(mon, "Device '%s' is writable but does not support snapshots.\n",
                               bdrv_get_device_name(bs));
            return;
        }
    }

    bs = find_vmstate_bs();
    if (!bs) {
        monitor_printf(mon, "No block device can accept snapshots\n");
        return;
    }

    saved_vm_running = runstate_is_running();
    vm_stop(RUN_STATE_SAVE_VM);

    memset(sn, 0, sizeof(*sn));

    /* fill auxiliary fields */
    qemu_gettimeofday(&tv);
    sn->date_sec = tv.tv_sec;
    sn->date_nsec = tv.tv_usec * 1000;
    sn->vm_clock_nsec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (name) {
        ret = bdrv_snapshot_find(bs, old_sn, name);
        if (ret >= 0) {
            pstrcpy(sn->name, sizeof(sn->name), old_sn->name);
            pstrcpy(sn->id_str, sizeof(sn->id_str), old_sn->id_str);
        } else {
            pstrcpy(sn->name, sizeof(sn->name), name);
        }
    } else {
        /* cast below needed for OpenBSD where tv_sec is still 'long' */
        localtime_r((const time_t *)&tv.tv_sec, &tm);
        strftime(sn->name, sizeof(sn->name), "vm-%Y%m%d%H%M%S", &tm);
    }

    /* Delete old snapshots of the same name */
    if (name && del_existing_snapshots(mon, name) < 0) {
        goto the_end;
    }

    /* save the VM state */
    f = qemu_fopen_bdrv(bs, 1);
    if (!f) {
        monitor_printf(mon, "Could not open VM state file\n");
        goto the_end;
    }
    ret = qemu_savevm_state(f);
    vm_state_size = qemu_ftell(f);
    qemu_fclose(f);
    if (ret < 0) {
        monitor_printf(mon, "Error %d while writing VM\n", ret);
        goto the_end;
    }

    /* create the snapshots */

    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            /* Write VM state size only to the image that contains the state */
            sn->vm_state_size = (bs == bs1 ? vm_state_size : 0);
            ret = bdrv_snapshot_create(bs1, sn);
            if (ret < 0) {
                monitor_printf(mon, "Error while creating snapshot on '%s'\n",
                               bdrv_get_device_name(bs1));
            }
        }
    }

 the_end:
    if (saved_vm_running) {
        vm_start();
    }
}

void qmp_xen_save_devices_state(const char *filename, Error **errp)
{
    QEMUFile *f;
    int saved_vm_running;
    int ret;

    saved_vm_running = runstate_is_running();
    vm_stop(RUN_STATE_SAVE_VM);

    f = qemu_fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        goto the_end;
    }
    ret = qemu_save_device_state(f);
    qemu_fclose(f);
    if (ret < 0) {
        error_set(errp, QERR_IO_ERROR);
    }

 the_end:
    if (saved_vm_running) {
        vm_start();
    }
}

int load_vmstate(const char *name)
{
    BlockDriverState *bs, *bs_vm_state;
    QEMUSnapshotInfo sn;
    QEMUFile *f;
    int ret;

    bs_vm_state = find_vmstate_bs();
    if (!bs_vm_state) {
        error_report("No block device supports snapshots");
        return -ENOTSUP;
    }

    /* Don't even try to load empty VM states */
    ret = bdrv_snapshot_find(bs_vm_state, &sn, name);
    if (ret < 0) {
        return ret;
    } else if (sn.vm_state_size == 0) {
        error_report("This is a disk-only snapshot. Revert to it offline "
            "using qemu-img.");
        return -EINVAL;
    }

    /* Verify if there is any device that doesn't support snapshots and is
    writable and check if the requested snapshot is available too. */
    bs = NULL;
    while ((bs = bdrv_next(bs))) {

        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
            continue;
        }

        if (!bdrv_can_snapshot(bs)) {
            error_report("Device '%s' is writable but does not support snapshots.",
                               bdrv_get_device_name(bs));
            return -ENOTSUP;
        }

        ret = bdrv_snapshot_find(bs, &sn, name);
        if (ret < 0) {
            error_report("Device '%s' does not have the requested snapshot '%s'",
                           bdrv_get_device_name(bs), name);
            return ret;
        }
    }

    /* Flush all IO requests so they don't interfere with the new state.  */
    bdrv_drain_all();

    bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs)) {
            ret = bdrv_snapshot_goto(bs, name);
            if (ret < 0) {
                error_report("Error %d while activating snapshot '%s' on '%s'",
                             ret, name, bdrv_get_device_name(bs));
                return ret;
            }
        }
    }

    /* restore the VM state */
    f = qemu_fopen_bdrv(bs_vm_state, 0);
    if (!f) {
        error_report("Could not open VM state file");
        return -EINVAL;
    }

    qemu_system_reset(VMRESET_SILENT);
    migration_incoming_state_init(f);
    ret = qemu_loadvm_state(f);

    qemu_fclose(f);
    migration_incoming_state_destroy();
    if (ret < 0) {
        error_report("Error %d while loading VM state", ret);
        return ret;
    }

    return 0;
}

void do_delvm(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs, *bs1;
    Error *err = NULL;
    const char *name = qdict_get_str(qdict, "name");

    bs = find_vmstate_bs();
    if (!bs) {
        monitor_printf(mon, "No block device supports snapshots\n");
        return;
    }

    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            bdrv_snapshot_delete_by_id_or_name(bs, name, &err);
            if (err) {
                monitor_printf(mon,
                               "Error while deleting snapshot on device '%s':"
                               " %s\n",
                               bdrv_get_device_name(bs),
                               error_get_pretty(err));
                error_free(err);
            }
        }
    }
}

void do_info_snapshots(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo *sn_tab, *sn, s, *sn_info = &s;
    int nb_sns, i, ret, available;
    int total;
    int *available_snapshots;

    bs = find_vmstate_bs();
    if (!bs) {
        monitor_printf(mon, "No available block device supports snapshots\n");
        return;
    }

    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        monitor_printf(mon, "bdrv_snapshot_list: error %d\n", nb_sns);
        return;
    }

    if (nb_sns == 0) {
        monitor_printf(mon, "There is no snapshot available.\n");
        return;
    }

    available_snapshots = g_malloc0(sizeof(int) * nb_sns);
    total = 0;
    for (i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        available = 1;
        bs1 = NULL;

        while ((bs1 = bdrv_next(bs1))) {
            if (bdrv_can_snapshot(bs1) && bs1 != bs) {
                ret = bdrv_snapshot_find(bs1, sn_info, sn->id_str);
                if (ret < 0) {
                    available = 0;
                    break;
                }
            }
        }

        if (available) {
            available_snapshots[total] = i;
            total++;
        }
    }

    if (total > 0) {
        bdrv_snapshot_dump((fprintf_function)monitor_printf, mon, NULL);
        monitor_printf(mon, "\n");
        for (i = 0; i < total; i++) {
            sn = &sn_tab[available_snapshots[i]];
            bdrv_snapshot_dump((fprintf_function)monitor_printf, mon, sn);
            monitor_printf(mon, "\n");
        }
    } else {
        monitor_printf(mon, "There is no suitable snapshot available\n");
    }

    g_free(sn_tab);
    g_free(available_snapshots);

}

void vmstate_register_ram(MemoryRegion *mr, DeviceState *dev)
{
    qemu_ram_set_idstr(memory_region_get_ram_addr(mr) & TARGET_PAGE_MASK,
                       memory_region_name(mr), dev);
}

void vmstate_unregister_ram(MemoryRegion *mr, DeviceState *dev)
{
    qemu_ram_unset_idstr(memory_region_get_ram_addr(mr) & TARGET_PAGE_MASK);
}

void vmstate_register_ram_global(MemoryRegion *mr)
{
    vmstate_register_ram(mr, NULL);
}
