#pragma once

#include "stackchan/port_stage1/event_log.h"

namespace stackchan::stage1::event_log::history {

// The caller owns the CoreS3 SD transaction for each operation.
bool EnsureDirectory(const char* root);
bool Append(const char* root, Group group, const Snapshot& snapshot,
            const Snapshot* sd_only = nullptr);
bool Read(const char* root, Group group, Snapshot& snapshot);

} // namespace stackchan::stage1::event_log::history
