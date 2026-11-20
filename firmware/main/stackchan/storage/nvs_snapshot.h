#pragma once
namespace stackchan::sd {
// Restore only when the default NVS partition contains no stored keys.
// The caller must reboot after a successful restoration, before application setup.
bool RestoreNvsIfEmpty();
// Copy every committed key, namespace and value type to the S3-style
// /sdcard/config.json with /sdcard/Backup/config.json as the previous version.
// Returns false when there are no NVS keys, SD is absent, or an error occurs.
bool BackupNvsIfChanged();
} // namespace stackchan::sd
