#pragma once
#include <esp_http_server.h>

namespace stackchan::stage1::setup_web {
esp_err_t Read(httpd_req_t* request);
esp_err_t Write(httpd_req_t* request);
}
