// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb13 — canonical protobuf-JSON <-> nanopb bridge. See json.h.

#include "json.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// JSON -> Command
// ---------------------------------------------------------------------------

namespace {

// Copy a JSON string field into a fixed nanopb char[] (truncating safely).
void copy_str(char *dst, size_t cap, const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    }
}

// Fill a touchy_SetPropertyCmd from the "setProperty" JSON object.
bool parse_set_property(const cJSON *o, touchy_SetPropertyCmd *sp, const char **err)
{
    copy_str(sp->widget_id, sizeof(sp->widget_id),
             cJSON_GetObjectItemCaseSensitive(o, "widgetId"));

    // property oneof: propertyName (string) | propertyId (number).
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "propertyName");
    const cJSON *pid  = cJSON_GetObjectItemCaseSensitive(o, "propertyId");
    if (cJSON_IsString(name) && name->valuestring) {
        sp->which_property = touchy_SetPropertyCmd_property_name_tag;
        strncpy(sp->property.property_name, name->valuestring,
                sizeof(sp->property.property_name) - 1);
        sp->property.property_name[sizeof(sp->property.property_name) - 1] = '\0';
    } else if (cJSON_IsNumber(pid)) {
        sp->which_property = touchy_SetPropertyCmd_property_id_tag;
        sp->property.property_id = (uint32_t)pid->valuedouble;
    } else {
        if (err) *err = "setProperty needs propertyName or propertyId";
        return false;
    }

    // value oneof (optional; absent = remove the override).
    const cJSON *bv = cJSON_GetObjectItemCaseSensitive(o, "boolValue");
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(o, "intValue");
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(o, "stringValue");
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(o, "colorValue");
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(o, "pointValue");
    if (cJSON_IsBool(bv)) {
        sp->which_value = touchy_SetPropertyCmd_bool_value_tag;
        sp->value.bool_value = cJSON_IsTrue(bv);
    } else if (cJSON_IsNumber(iv)) {
        sp->which_value = touchy_SetPropertyCmd_int_value_tag;
        sp->value.int_value = (int32_t)iv->valuedouble;
    } else if (cJSON_IsString(sv) && sv->valuestring) {
        sp->which_value = touchy_SetPropertyCmd_string_value_tag;
        strncpy(sp->value.string_value, sv->valuestring,
                sizeof(sp->value.string_value) - 1);
        sp->value.string_value[sizeof(sp->value.string_value) - 1] = '\0';
    } else if (cJSON_IsNumber(cv)) {
        sp->which_value = touchy_SetPropertyCmd_color_value_tag;
        sp->value.color_value = (uint32_t)cv->valuedouble;
    } else if (cJSON_IsObject(pv)) {
        sp->which_value = touchy_SetPropertyCmd_point_value_tag;
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(pv, "x");
        const cJSON *y = cJSON_GetObjectItemCaseSensitive(pv, "y");
        sp->value.point_value.x = cJSON_IsNumber(x) ? (int32_t)x->valuedouble : 0;
        sp->value.point_value.y = cJSON_IsNumber(y) ? (int32_t)y->valuedouble : 0;
    }
    // else: no value arm → which_value stays 0 → remove.
    return true;
}

}  // namespace

bool json_to_command(const char *json, size_t len, touchy_Command *out,
                     const char **err)
{
    if (err) *err = nullptr;
    *out = touchy_Command_init_zero;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsObject(root)) {
        if (err) *err = "body is not a JSON object";
        cJSON_Delete(root);
        return false;
    }

    // Canonical protobuf-JSON wraps the chosen oneof arm under its own key,
    // e.g. {"setProperty": {...}}. Find the single command key.
    bool ok = true;
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(root, "setProperty");
    if (sp) {
        out->which_cmd = touchy_Command_set_property_tag;
        ok = parse_set_property(sp, &out->cmd.set_property, err);
    } else if (cJSON_GetObjectItemCaseSensitive(root, "sysBoardInfoGet")) {
        out->which_cmd = touchy_Command_sys_board_info_get_tag;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "screenWake")) {
        out->which_cmd = touchy_Command_screen_wake_tag;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "getPreferences")) {
        out->which_cmd = touchy_Command_get_preferences_tag;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "sysRebootBootloader")) {
        out->which_cmd = touchy_Command_sys_reboot_bootloader_tag;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "eventConsume")) {
        out->which_cmd = touchy_Command_event_consume_tag;
    } else {
        if (err) *err = "no supported command key in JSON";
        ok = false;
    }

    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// Response -> JSON
// ---------------------------------------------------------------------------

namespace {

const char *result_code_name(touchy_ResultCode c)
{
    switch (c) {
    case touchy_ResultCode_OK:            return "OK";
    case touchy_ResultCode_UNKNOWN_ERROR: return "UNKNOWN_ERROR";
    case touchy_ResultCode_INVALID_ARG:   return "INVALID_ARG";
    case touchy_ResultCode_NOT_FOUND:     return "NOT_FOUND";
    case touchy_ResultCode_NO_SPACE:      return "NO_SPACE";
    case touchy_ResultCode_IO_ERROR:      return "IO_ERROR";
    case touchy_ResultCode_NOT_SUPPORTED: return "NOT_SUPPORTED";
    default:                              return "UNKNOWN_ERROR";
    }
}

// Add a uint64 as a JSON string, matching canonical proto3 JSON for 64-bit
// integer fields (avoids the double-precision loss cJSON numbers would have).
void add_u64_str(cJSON *o, const char *key, uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    cJSON_AddStringToObject(o, key, buf);
}

void render_board_info(cJSON *parent, const touchy_SysBoardInfoResponse *b)
{
    cJSON *o = cJSON_AddObjectToObject(parent, "sysBoardInfo");
    if (!o) return;
    // protocol_version is an enum; emit its numeric value (readable, and a
    // valid canonical-JSON alternative to the name form).
    if (b->protocol_version) cJSON_AddNumberToObject(o, "protocolVersion", b->protocol_version);
    if (b->firmware_version) cJSON_AddNumberToObject(o, "firmwareVersion", b->firmware_version);
    if (b->firmware_version_str[0]) cJSON_AddStringToObject(o, "firmwareVersionStr", b->firmware_version_str);
    if (b->board_name[0]) cJSON_AddStringToObject(o, "boardName", b->board_name);
    if (b->display_width)  cJSON_AddNumberToObject(o, "displayWidth", b->display_width);
    if (b->display_height) cJSON_AddNumberToObject(o, "displayHeight", b->display_height);
    if (b->is_multitouch) cJSON_AddBoolToObject(o, "isMultitouch", true);
    if (b->has_usb)       cJSON_AddBoolToObject(o, "hasUsb", true);
    if (b->serial[0])     cJSON_AddStringToObject(o, "serial", b->serial);
    if (b->free_heap_bytes)  add_u64_str(o, "freeHeapBytes", b->free_heap_bytes);
    if (b->free_psram_bytes) add_u64_str(o, "freePsramBytes", b->free_psram_bytes);
    if (b->fs_total_bytes)   add_u64_str(o, "fsTotalBytes", b->fs_total_bytes);
    if (b->fs_used_bytes)    add_u64_str(o, "fsUsedBytes", b->fs_used_bytes);
    if (b->temp_is_flash) cJSON_AddBoolToObject(o, "tempIsFlash", true);
    if (b->is_touchable)  cJSON_AddBoolToObject(o, "isTouchable", true);
}

}  // namespace

char *response_to_json(const touchy_Response *resp)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;

    // Omit code when OK (proto3 JSON drops default/zero fields).
    if (resp->code != touchy_ResultCode_OK) {
        cJSON_AddStringToObject(root, "code", result_code_name(resp->code));
    }

    switch (resp->which_payload) {
    case touchy_Response_sys_board_info_tag:
        render_board_info(root, &resp->payload.sys_board_info);
        break;
    default:
        // Other payloads (event_consume, preferences_read, …) are not
        // rendered to JSON yet — the top-level code still round-trips.
        break;
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
