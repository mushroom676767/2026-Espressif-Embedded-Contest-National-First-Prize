#include "tool_cabinet.h"

#include <stdio.h>
#include <string.h>

_Static_assert(TOOL_CABINET_SLOT_COUNT == 9, "TOOL_CABINET_SLOT_COUNT 9");

static const tool_catalog_entry_t s_catalog[TOOL_CABINET_SLOT_COUNT] = {
    {1, "SD-001", "螺丝刀1", "screwdriver"},
    {2, "SD-002", "螺丝刀2", "screwdriver"},
    {3, "SSD-001", "套筒螺丝刀", "socket_screwdriver"},
    {4, "PL-001", "老虎钳", "pliers"},
    {5, "CR-001", "压线钳", "crimper"},
    {6, "WR-001", "扳手", "wrench"},
    {7, "EP-001", "电子钳", "electronic_pliers"},
    {8, "HM-001", "锤子", "hammer"},
    {9, "TM-001", "卷尺", "tape_measure"},
};

const tool_catalog_entry_t *tool_cabinet_catalog(void)
{
    return s_catalog;
}

size_t tool_cabinet_catalog_count(void)
{
    return TOOL_CABINET_SLOT_COUNT;
}

const tool_catalog_entry_t *tool_cabinet_get_slot(uint8_t slot_no)
{
    if (slot_no == 0 || slot_no > TOOL_CABINET_SLOT_COUNT) {
        return NULL;
    }
    return &s_catalog[slot_no - 1];
}

void tool_cabinet_init_states(tool_slot_runtime_t *states, size_t count)
{
    if (states == NULL) {
        return;
    }

    size_t limit = count < TOOL_CABINET_SLOT_COUNT ? count : TOOL_CABINET_SLOT_COUNT;
    for (size_t i = 0; i < limit; i++) {
        states[i].slot_no = s_catalog[i].slot_no;
        states[i].status = TOOL_SLOT_AVAILABLE;
    }
}

tool_slot_status_t tool_cabinet_next_touch_status(tool_slot_status_t status)
{
    switch (status) {
        case TOOL_SLOT_AVAILABLE:
            return TOOL_SLOT_BORROWED;
        case TOOL_SLOT_BORROWED:
            return TOOL_SLOT_WARNING;
        case TOOL_SLOT_WARNING:
        case TOOL_SLOT_EMPTY:
        default:
            return TOOL_SLOT_AVAILABLE;
    }
}

const char *tool_cabinet_status_to_backend(tool_slot_status_t status)
{
    switch (status) {
        case TOOL_SLOT_AVAILABLE:
            return "present";
        case TOOL_SLOT_BORROWED:
        case TOOL_SLOT_EMPTY:
            return "borrowed";
        case TOOL_SLOT_WARNING:
        default:
            return "uncertain";
    }
}

const char *tool_cabinet_status_label(tool_slot_status_t status)
{
    switch (status) {
        case TOOL_SLOT_AVAILABLE:
            return "在位";
        case TOOL_SLOT_BORROWED:
        case TOOL_SLOT_EMPTY:
            return "借出";
        case TOOL_SLOT_WARNING:
        default:
            return "异常";
    }
}

static void copy_change(tool_change_t *dst,
                        const tool_catalog_entry_t *tool,
                        tool_slot_status_t from_status,
                        tool_slot_status_t to_status)
{
    if (dst == NULL || tool == NULL) {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->slot_no = tool->slot_no;
    snprintf(dst->tool_code, sizeof(dst->tool_code), "%s", tool->tool_code);
    snprintf(dst->tool_name, sizeof(dst->tool_name), "%s", tool->tool_name);
    snprintf(dst->tool_class, sizeof(dst->tool_class), "%s", tool->tool_class);
    dst->from_status = from_status;
    dst->to_status = to_status;
}

void tool_cabinet_diff(const tool_slot_runtime_t *before,
                       const tool_slot_runtime_t *after,
                       size_t count,
                       tool_operation_diff_t *diff)
{
    if (diff == NULL) {
        return;
    }

    memset(diff, 0, sizeof(*diff));
    if (before == NULL || after == NULL) {
        return;
    }

    size_t limit = count < TOOL_CABINET_SLOT_COUNT ? count : TOOL_CABINET_SLOT_COUNT;
    for (size_t i = 0; i < limit; i++) {
        if (before[i].status == after[i].status) {
            continue;
        }

        const uint8_t slot_no = after[i].slot_no != 0 ? after[i].slot_no : (uint8_t)(i + 1);
        const tool_catalog_entry_t *tool = tool_cabinet_get_slot(slot_no);
        if (tool == NULL) {
            continue;
        }

        diff->changed = true;
        if (after[i].status == TOOL_SLOT_WARNING && diff->anomaly_count < TOOL_CABINET_MAX_CHANGES) {
            copy_change(&diff->anomalies[diff->anomaly_count++], tool, before[i].status, after[i].status);
        } else if (before[i].status == TOOL_SLOT_AVAILABLE &&
                   after[i].status == TOOL_SLOT_BORROWED &&
                   diff->borrowed_count < TOOL_CABINET_MAX_CHANGES) {
            copy_change(&diff->borrowed[diff->borrowed_count++], tool, before[i].status, after[i].status);
        } else if (before[i].status == TOOL_SLOT_BORROWED &&
                   after[i].status == TOOL_SLOT_AVAILABLE &&
                   diff->returned_count < TOOL_CABINET_MAX_CHANGES) {
            copy_change(&diff->returned[diff->returned_count++], tool, before[i].status, after[i].status);
        } else if (diff->anomaly_count < TOOL_CABINET_MAX_CHANGES) {
            copy_change(&diff->anomalies[diff->anomaly_count++], tool, before[i].status, after[i].status);
        }
    }
}

const char *tool_cabinet_result_type(const tool_operation_diff_t *diff)
{
    if (diff == NULL || !diff->changed) {
        return "no_change";
    }
    if (diff->anomaly_count > 0) {
        return "anomaly";
    }
    if (diff->borrowed_count > 0) {
        return "borrow_ok";
    }
    return "return_ok";
}

void tool_cabinet_format_time_iso(char *out, size_t out_size, time_t now)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    now += 8 * 60 * 60;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%S", &tm_now);
}

void tool_cabinet_make_event_id(const char *device_code,
                                time_t now,
                                uint32_t seq,
                                char *out,
                                size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    now += 8 * 60 * 60;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    snprintf(out, out_size,
             "%s-%04d%02d%02d%02d%02d%02d-%04u",
             device_code != NULL && device_code[0] != '\0' ? device_code : "DEMO-CABINET-001",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (unsigned int)(seq % 10000));
}
