#pragma once
#include <esp_http_server.h>
namespace stackchan::wifi::web {
esp_err_t ReadSettings(httpd_req_t* req);
esp_err_t WriteSettings(httpd_req_t* req);
// Called by StackChan's existing update task; no extra permanent task stack.
void ApplyPendingIfIdle();
}
