/*
 * SCS.h
 * 飞特串行舵机通信层协议程序
 * 日期: 2024.11.21
 * 作者: txl
 */

#ifndef _SCS_H
#define _SCS_H

#include "INST.h"
#include <stdint.h>

class SCS{
public:
	SCS();
	SCS(u8 End);
	SCS(u8 End, u8 Level);
	int genWrite(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen);//普通写指令
	int regWrite(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen);//异步写指令
	int RegWriteAction(u8 ID = 0xfe);//异步写执行指令
	void syncWrite(u8 ID[], u8 IDN, u8 MemAddr, u8 *nDat, u8 nLen);//同步写指令
	int writeByte(u8 ID, u8 MemAddr, u8 bDat);//写1个字节
	int writeWord(u8 ID, u8 MemAddr, u16 wDat);//写2个字节
	int Read(u8 ID, u8 MemAddr, u8 *nData, u8 nLen);//读指令
	int readByte(u8 ID, u8 MemAddr);//读1个字节
	int readWord(u8 ID, u8 MemAddr);//读2个字节
	int Ping(u8 ID);//Ping指令
	int syncReadPacketTx(u8 ID[], u8 IDN, u8 MemAddr, u8 nLen);//同步读指令包发送
	int syncReadPacketRx(u8 ID, u8 *nDat);//同步读返回包解码，成功返回内存字节数，失败返回0
	int syncReadRxPacketToByte();//解码一个字节
	int syncReadRxPacketToWrod(u8 negBit=0);//解码两个字节，negBit为方向为，negBit=0表示无方向
	void syncReadBegin(u8 IDN, u8 rxLen, u32 TimeOut);//同步读开始
	void syncReadEnd();//同步读结束
	int Reset(u8 ID);//重置舵机状态
	int Recal(u8 ID);//重置舵机中位
	u8 getState() { return u8Status; }
	u8 getLastError() { return u8Error; }
	// Access under the existing shared servo bus lock. No heap allocation.
	struct ReadHeaderDiagnostic {
		uint32_t wrong_id = 0, wrong_length = 0;
		uint32_t rx_ff = 0, rx_other_servo = 0, rx_zero = 0, rx_other = 0;
		uint32_t ack_length = 0, read_echo_prefix = 0, ack_skipped = 0;
		uint8_t address = 0, requested_bytes = 0;
		uint8_t rx_id = 0, rx_length = 0, rx_status = 0;
		uint8_t previous_id = 0, previous_command = 0, previous_address = 0;
	};
	// Separate ordinary reads from long feedback reads for each servo.
	const ReadHeaderDiagnostic& getReadHeaderDiagnostic(u8 id, bool long_read) const {
		return read_header_diag[id == 2 ? 1 : 0][long_read ? 1 : 0];
	}

	struct WriteAckDiagnostic {
		uint32_t received = 0, absent = 0, invalid = 0;
	};
	const WriteAckDiagnostic& getWriteAckDiagnostic(u8 id) const {
		return write_ack_diag[id == 2 ? 1 : 0];
	}
public:
	u8 Level;//舵机返回等级
	u8 End;//处理器大小端结构
	u8 u8Status;//舵机状态
	u8 u8Error;//通信状态
	u8 syncReadRxPacketIndex;
	u8 syncReadRxPacketLen;
	u8 *syncReadRxPacket;
	u8 *syncReadRxBuff;
	u16 syncReadRxBuffLen;
	u16 syncReadRxBuffMax;
	u32 syncTimeOut;
protected:
	virtual int writeSCS(unsigned char *nDat, int nLen) = 0;
	virtual int readSCS(unsigned char *nDat, int nLen) = 0;
	virtual int readSCS(unsigned char *nDat, int nLen, unsigned long TimeOut) = 0;
	virtual int writeSCS(unsigned char bDat) = 0;
	virtual void rFlushSCS() = 0;
	virtual void wFlushSCS() = 0;
	virtual int readOptionalAck(u8 id, u8* data, int count) { (void)id; (void)data; (void)count; return 0; }
	virtual bool transmissionOk() const { return true; }
protected:
	void writeBuf(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen, u8 Fun);
	void Host2SCS(u8 *DataL, u8* DataH, u16 Data);//1个16位数拆分为2个8位数
	u16	SCS2Host(u8 DataL, u8 DataH);//2个8位数组合为1个16位数
	int	Ack(u8 ID);//返回应答
	int checkHead();//帧头检测
private:
	ReadHeaderDiagnostic read_header_diag[2][2]{};
	WriteAckDiagnostic write_ack_diag[2]{};
	static bool validAck(u8 id, const u8* body);
	u8 last_tx_id = 0, last_tx_command = 0, last_tx_address = 0;
	void recordReadHeader(u8 id, u8 address, u8 count, const u8* header,
	                      u8 previous_id, u8 previous_command, u8 previous_address);

};
#endif