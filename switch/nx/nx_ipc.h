// Calls to the system's services that libnx has no function for. They're in C because libnx's macros for them only work there.
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

/// What the home menu goes by to find a game's content.
typedef struct {
    NcmContentMetaKey key;
    u64 storage;
} NsextRecord;

Result nsextPushRecords(Service* manager, u64 application, const NsextRecord* records, s32 count);
Result nsextListRecords(Service* manager, u64 application, s32 offset, NsextRecord* records, s32 count, s32* written);
Result nsextDeleteRecords(Service* manager, u64 application);

Result esextCount(Service* es, bool personalized, u32* count);
Result esextList(Service* es, bool personalized, u32* written, void* rightsIds, size_t size);
Result esextGet(Service* es, bool personalized, const void* rightsId, u64* size, void* ticket, size_t capacity);

#ifdef __cplusplus
}
#endif
