#include "nx_ipc.h"

#include <string.h>

Result nsextPushRecords(Service* manager, u64 application, const NsextRecord* records, s32 count) {
    // Event 3 is what installing something leaves behind.
    const struct {
        u8 lastEvent;
        u8 padding[7];
        u64 application;
    } in = {3, {0}, application};
    return serviceDispatchIn(manager, 16, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{records, (size_t)count * sizeof(NsextRecord)}});
}

Result nsextListRecords(Service* manager, u64 application, s32 offset, NsextRecord* records, s32 count, s32* written) {
    const struct {
        u64 offset;
        u64 application;
    } in = {(u64)offset, application};
    u32 out = 0;
    Result rc = serviceDispatchInOut(manager, 17, in, out,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{records, (size_t)count * sizeof(NsextRecord)}});
    if (R_SUCCEEDED(rc))
        *written = (s32)out;
    return rc;
}

Result nsextDeleteRecords(Service* manager, u64 application) { return serviceDispatchIn(manager, 27, application); }

Result esextCount(Service* es, bool personalized, u32* count) { return serviceDispatchOut(es, personalized ? 10 : 9, *count); }

Result esextList(Service* es, bool personalized, u32* written, void* rightsIds, size_t size) {
    return serviceDispatchOut(es, personalized ? 12 : 11, *written,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{rightsIds, size}});
}

Result esextGet(Service* es, bool personalized, const void* rightsId, u64* size, void* ticket, size_t capacity) {
    struct {
        u8 id[0x10];
    } in;
    memcpy(in.id, rightsId, sizeof(in.id));
    return serviceDispatchInOut(es, personalized ? 17 : 16, in, *size,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{ticket, capacity}});
}
