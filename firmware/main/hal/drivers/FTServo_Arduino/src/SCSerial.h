/*
 * SCSerial.h
 * 飞特串行舵机硬件接口层程序
 * 日期: 2024.11.22
 * 作者: txl
 */

#ifndef _SCSERIAL_H
#define _SCSERIAL_H

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "SCS.h"

class SCSerial : public SCS
{
public:
	SCSerial();
	SCSerial(u8 End);
	SCSerial(u8 End, u8 Level);
	bool begin(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin, int buf_size = 1024);
	void end();
	void reportDiagnostics();

private:
	static constexpr int kEventDepth = 16;
	QueueHandle_t _events = nullptr;
	uint32_t _fifo_overflow = 0, _buffer_full = 0, _frame_error = 0, _parity_error = 0, _breaks = 0;
	uint32_t _event_full = 0, _rx_calls = 0, _rx_short = 0, _rx_max_us = 0, _ack_max_us = 0;
	uint32_t _tx_max_us = 0, _tx_fail = 0;
	int64_t _report_us = 0;
	int64_t _reply_deadline_us = 0;
	bool _tx_ok = true;
	// Exact TX frame for stripping local echoes; no heap allocation.
	u8 _tx_frame[262]{};
	int _tx_frame_size = 0;
	bool _tx_frame_complete = true;
	uint32_t _ack_echo = 0, _ack_resync = 0, _ack_echo_only = 0, _ack_after_1ms = 0;
	void rememberTx(const u8* data, int count);
	void drainEvents();
	int readBytes(unsigned char *nDat, int nLen, unsigned long timeout, bool interByteTimeout);
protected:
	int writeSCS(unsigned char *nDat, int nLen);//输出nLen字节
	int readSCS(unsigned char *nDat, int nLen);//输入nLen字节
	int readSCS(unsigned char *nDat, int nLen, unsigned long TimeOut);
	int writeSCS(unsigned char bDat);//输出1字节
	void rFlushSCS();//
	void wFlushSCS();//
	int readOptionalAck(u8 id, u8* data, int count, u16 timeoutUs) override;
	bool transmissionOk() const override { return _tx_ok; }
public:
	unsigned long IOTimeOut;//输入输出超时
	uart_port_t uart_num;//串口编号
};

#endif
