/*
 * SCSerial.cpp
 * 飞特串行舵机硬件接口层程序
 * 日期: 2024.4.2
 * 作者:
 */

#include "SCSerial.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char *TAG = "FT_SCSerial";

SCSerial::SCSerial()
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

SCSerial::SCSerial(u8 End):SCS(End)
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

SCSerial::SCSerial(u8 End, u8 Level):SCS(End, Level)
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

bool SCSerial::begin(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin, int buf_size)
{
	this->uart_num = uart_num;

	uart_config_t uart_config = {
		.baud_rate = baud_rate,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.rx_flow_ctrl_thresh = 0,
		.source_clk = UART_SCLK_DEFAULT,
	};

	esp_err_t ret = uart_driver_install(uart_num, buf_size, buf_size, kEventDepth, &_events, 0);
	if (ret == ESP_ERR_NO_MEM) {
		// Diagnostics must not prevent the original bus from starting.
		_events = nullptr;
		ret = uart_driver_install(uart_num, buf_size, buf_size, 0, NULL, 0);
		if (ret == ESP_OK) ESP_LOGW(TAG, "UART event diagnostics unavailable");
	}
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
		return false;
	}

	ret = uart_param_config(uart_num, &uart_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
		uart_driver_delete(uart_num);
		_events = nullptr;
		this->uart_num = UART_NUM_MAX;
		return false;
	}

	ret = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
		uart_driver_delete(uart_num);
		_events = nullptr;
		this->uart_num = UART_NUM_MAX;
		return false;
	}

	// Short servo replies (6-21 bytes) must reach the driver promptly.
	// Keep the existing baud rate and protocol; avoid per-byte interrupts.
	const esp_err_t fifo_result = uart_set_rx_full_threshold(uart_num, 16);
	const esp_err_t timeout_result = uart_set_rx_timeout(uart_num, 3);
	if (fifo_result != ESP_OK || timeout_result != ESP_OK) {
		ESP_LOGW(TAG, "servo RX tuning failed: fifo=%s timeout=%s",
		         esp_err_to_name(fifo_result), esp_err_to_name(timeout_result));
	}
	return true;
}

void SCSerial::end()
{
	if (uart_num < UART_NUM_MAX) {
		uart_driver_delete(uart_num);
		_events = nullptr;
		uart_num = UART_NUM_MAX;
	}
}

int SCSerial::readBytes(unsigned char *nDat, int nLen, unsigned long timeout, bool interByteTimeout)
{
	if (uart_num >= UART_NUM_MAX || nLen <= 0) return 0;

	int size = 0;
	const int64_t call_started = esp_timer_get_time();
	int64_t started = call_started / 1000;
	unsigned char discard[32];
	while (size < nLen) {
		size_t available = 0;
		uart_get_buffered_data_len(uart_num, &available);
		if (available > 0) {
			size_t count = static_cast<size_t>(nLen - size);
			if (count > available) count = available;
			if (!nDat && count > sizeof(discard)) count = sizeof(discard);
			const int len = uart_read_bytes(uart_num, nDat ? nDat + size : discard, count, 0);
			if (len > 0) {
				size += len;
				if (interByteTimeout) started = esp_timer_get_time() / 1000;
				if (size >= nLen) break;
				if (esp_timer_get_time() / 1000 - started > static_cast<int64_t>(timeout)) break;
				continue; // Drain buffered bytes without yielding between bytes.
			}
		}
		if (esp_timer_get_time() / 1000 - started > static_cast<int64_t>(timeout)) break;
		vTaskDelay(1);
	}
	const uint32_t elapsed = esp_timer_get_time() - call_started;
	++_rx_calls;
	if (size != nLen) ++_rx_short;
	if (elapsed > _rx_max_us) _rx_max_us = elapsed;
	return size;
}

int SCSerial::readSCS(unsigned char *nDat, int nLen, unsigned long TimeOut)
{
	return readBytes(nDat, nLen, TimeOut, false);
}

int SCSerial::readSCS(unsigned char *nDat, int nLen)
{
	return readBytes(nDat, nLen, IOTimeOut, true);
}

int SCSerial::writeSCS(unsigned char *nDat, int nLen)
{
	if (uart_num >= UART_NUM_MAX || nDat == NULL) {
		return 0;
	}

	return uart_write_bytes(uart_num, reinterpret_cast<const char *>(nDat), nLen);
}

int SCSerial::writeSCS(unsigned char bDat)
{
	if (uart_num >= UART_NUM_MAX) {
		return 0;
	}

	return uart_write_bytes(uart_num, reinterpret_cast<const char *>(&bDat), 1);
}

void SCSerial::rFlushSCS()
{
	if (uart_num >= UART_NUM_MAX) {
		return;
	}

	drainEvents();
	uart_flush_input(uart_num);
}

void SCSerial::wFlushSCS()
{
	if (uart_num >= UART_NUM_MAX) {
		return;
	}

	const int64_t started = esp_timer_get_time();
	const esp_err_t result = uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100));
	const uint32_t elapsed = esp_timer_get_time() - started;
	if (elapsed > _tx_max_us) _tx_max_us = elapsed;
	if (result != ESP_OK) {
		++_tx_fail;
		ESP_LOGW(TAG, "servo TX completion failed: %s", esp_err_to_name(result));
	}
}

// Optional write replies: bounded microsecond polling, independent of RTOS tick.
// Do not impose a full read timeout when a servo does not return an ACK.
int SCSerial::readOptionalAck(u8* data, int count)
{
	if (uart_num >= UART_NUM_MAX || !data || count <= 0) return 0;
	const int64_t started = esp_timer_get_time();
	const int64_t deadline = started + 1000;
	int size = 0;
	while (size < count) {
		const int received = uart_read_bytes(uart_num, data + size, count - size, 0);
		if (received > 0) size += received;
		if (size == count || esp_timer_get_time() >= deadline) break;
		esp_rom_delay_us(20);
	}
	const uint32_t elapsed = esp_timer_get_time() - started;
	if (elapsed > _ack_max_us) _ack_max_us = elapsed;
	return size;
}

void SCSerial::drainEvents()
{
	if (!_events) return;
	if (uxQueueMessagesWaiting(_events) >= kEventDepth) ++_event_full;
	uart_event_t event;
	// Bound diagnostic work even if new events arrive continuously.
	for (int i = 0; i < kEventDepth && xQueueReceive(_events, &event, 0) == pdTRUE; ++i) {
		switch (event.type) {
			case UART_FIFO_OVF: ++_fifo_overflow; break;
			case UART_BUFFER_FULL: ++_buffer_full; break;
			case UART_FRAME_ERR: ++_frame_error; break;
			case UART_PARITY_ERR: ++_parity_error; break;
			case UART_BREAK: ++_breaks; break;
			default: break;
		}
	}
}

void SCSerial::reportDiagnostics()
{
	const int64_t now = esp_timer_get_time();
	if (now - _report_us < 10000000) return;
	_report_us = now;
	drainEvents();
	ESP_LOGI(TAG, "[SERVO_UART] events=%d fifo_ovf=%lu buffer_full=%lu frame=%lu parity=%lu break=%lu queue_full=%lu",
	         _events != nullptr, (unsigned long)_fifo_overflow, (unsigned long)_buffer_full,
	         (unsigned long)_frame_error, (unsigned long)_parity_error, (unsigned long)_breaks, (unsigned long)_event_full);
	ESP_LOGI(TAG, "[SERVO_IO] rx_calls=%lu rx_short=%lu rx_max_us=%lu ack_max_us=%lu tx_max_us=%lu tx_fail=%lu",
	         (unsigned long)_rx_calls, (unsigned long)_rx_short, (unsigned long)_rx_max_us,
	         (unsigned long)_ack_max_us, (unsigned long)_tx_max_us, (unsigned long)_tx_fail);
	_rx_max_us = _ack_max_us = _tx_max_us = 0;
}
