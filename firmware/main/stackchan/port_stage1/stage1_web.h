// Stage-1 settings HTTP service: start on Wi-Fi connection, before activation.
#pragma once
namespace stackchan::stage1::web {
bool Start(); // true when ready; false when a later retry is needed
bool IsReady(); // true only after all HTTP handlers are registered
}
