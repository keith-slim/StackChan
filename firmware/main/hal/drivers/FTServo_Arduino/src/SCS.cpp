/*
 * SCS.cpp
 * 飞特串行舵机通信层协议程序
 * 日期: 2024.12.21
 * 作者: txl
 */

#include <stddef.h>
#include <string.h>
#include "esp_timer.h"
#include "SCS.h"

SCS::SCS()
{
	Level = 1;//除广播指令所有指令返回应答
	u8Status = 0;
}

SCS::SCS(u8 End)
{
	Level = 1;
	this->End = End;
	u8Status = 0;
}

SCS::SCS(u8 End, u8 Level)
{
	this->Level = Level;
	this->End = End;
	u8Status = 0;
}

//1个16位数拆分为2个8位数
//DataL为低位，DataH为高位
void SCS::Host2SCS(u8 *DataL, u8* DataH, u16 Data)
{
	if(End){
		*DataL = (Data>>8);
		*DataH = (Data&0xff);
	}else{
		*DataH = (Data>>8);
		*DataL = (Data&0xff);
	}
}

//2个8位数组合为1个16位数
//DataL为低位，DataH为高位
u16 SCS::SCS2Host(u8 DataL, u8 DataH)
{
	u16 Data;
	if(End){
		Data = DataL;
		Data<<=8;
		Data |= DataH;
	}else{
		Data = DataH;
		Data<<=8;
		Data |= DataL;
	}
	return Data;
}

void SCS::writeBuf(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen, u8 Fun)
{
	if (nDat && nLen > 252) return;
	u8 packet[259];
	const unsigned length = (nDat ? unsigned(nLen) + 1 : 0) + 2;
	packet[0] = packet[1] = 0xff;
	packet[2] = ID;
	packet[3] = static_cast<u8>(length);
	packet[4] = Fun;
	if (nDat) {
		packet[5] = MemAddr;
		memcpy(packet + 6, nDat, nLen);
	}
	u8 sum = 0;
	for (unsigned i = 2; i < length + 3; ++i) sum += packet[i];
	packet[length + 3] = static_cast<u8>(~sum);
	writeSCS(packet, length + 4);
}

//普通写指令
//舵机ID，MemAddr内存表地址，写入数据，写入长度
int SCS::genWrite(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen)
{
	rFlushSCS();
	writeBuf(ID, MemAddr, nDat, nLen, INST_WRITE);
	wFlushSCS();
	return Ack(ID);
}

//异步写指令
//舵机ID，MemAddr内存表地址，写入数据，写入长度
int SCS::regWrite(u8 ID, u8 MemAddr, u8 *nDat, u8 nLen)
{
	rFlushSCS();
	writeBuf(ID, MemAddr, nDat, nLen, INST_REG_WRITE);
	wFlushSCS();
	return Ack(ID);
}

//异步写执行指令
//舵机ID
int SCS::RegWriteAction(u8 ID)
{
	rFlushSCS();
	writeBuf(ID, 0, NULL, 0, INST_REG_ACTION);
	wFlushSCS();
	return Ack(ID);
}

//同步写指令
//舵机ID[]数组，IDN数组长度，MemAddr内存表地址，写入数据，写入长度
void SCS::syncWrite(u8 ID[], u8 IDN, u8 MemAddr, u8 *nDat, u8 nLen)
{
	rFlushSCS();
	u8 mesLen = ((nLen+1)*IDN+4);
	u8 Sum = 0;
	u8 bBuf[7];
	bBuf[0] = 0xff;
	bBuf[1] = 0xff;
	bBuf[2] = 0xfe;
	bBuf[3] = mesLen;
	bBuf[4] = INST_SYNC_WRITE;
	bBuf[5] = MemAddr;
	bBuf[6] = nLen;
	writeSCS(bBuf, 7);

	Sum = 0xfe + mesLen + INST_SYNC_WRITE + MemAddr + nLen;
	u8 i, j;
	for(i=0; i<IDN; i++){
		writeSCS(ID[i]);
		writeSCS(nDat+i*nLen, nLen);
		Sum += ID[i];
		for(j=0; j<nLen; j++){
			Sum += nDat[i*nLen+j];
		}
	}
	writeSCS(~Sum);
	wFlushSCS();
}

int SCS::writeByte(u8 ID, u8 MemAddr, u8 bDat)
{
	rFlushSCS();
	writeBuf(ID, MemAddr, &bDat, 1, INST_WRITE);
	wFlushSCS();
	return Ack(ID);
}

int SCS::writeWord(u8 ID, u8 MemAddr, u16 wDat)
{
	u8 bBuf[2];
	Host2SCS(bBuf+0, bBuf+1, wDat);
	rFlushSCS();
	writeBuf(ID, MemAddr, bBuf, 2, INST_WRITE);
	wFlushSCS();
	return Ack(ID);
}

//读指令
//舵机ID，MemAddr内存表地址，返回数据nData，数据长度nLen
int SCS::receiveStatus(u8 ID, u8 *data, u8 nLen, u8 *replyID)
{
	u8Error = ERR_NO_REPLY;
	u8Status = 0;
	if (nLen > 253) { u8Error = ERR_BUFF_LEN; return 0; }
	u8 packet[259];
	unsigned used = 0;
	const int64_t deadline = esp_timer_get_time() + 20000;
	while (esp_timer_get_time() < deadline) {
		const int64_t remaining = deadline - esp_timer_get_time();
		if (remaining <= 0) break;
		u8 byte;
		if (readSCS(&byte, 1, static_cast<unsigned long>((remaining + 999) / 1000)) != 1) break;
		packet[used++] = byte;
		while (used >= 2) {
			if (packet[0] != 0xff || packet[1] != 0xff ||
			    (used >= 3 && packet[2] == 0xff)) {
				memmove(packet, packet + 1, --used);
				continue;
			}
			if (used < 4) break;
			if ((ID != 0xfe && packet[2] != ID) || packet[2] >= 0xfe) {
				u8Error = ERR_SLAVE_ID;
				memmove(packet, packet + 1, --used);
				continue;
			}
			if (packet[3] != unsigned(nLen) + 2) {
				u8Error = ERR_BUFF_LEN;
				memmove(packet, packet + 1, --used);
				continue;
			}
			const unsigned total = unsigned(nLen) + 6;
			if (used < total) break;
			u8 sum = 0;
			for (unsigned i = 2; i < total; ++i) sum += packet[i];
			if (sum != 0xff) {
				u8Error = ERR_CRC_CMP;
				memmove(packet, packet + 1, --used);
				continue;
			}
			if (data && nLen) memcpy(data, packet + 5, nLen);
			if (replyID) *replyID = packet[2];
			u8Status = packet[4];
			u8Error = 0;
			return 1;
		}
	}
	return 0;
}

int SCS::Read(u8 ID, u8 MemAddr, u8 *nData, u8 nLen)
{
	rFlushSCS();
	writeBuf(ID, MemAddr, &nLen, 1, INST_READ);
	wFlushSCS();
	return receiveStatus(ID, nData, nLen, NULL) ? nLen : 0;
}

//读1字节，超时返回-1
int SCS::readByte(u8 ID, u8 MemAddr)
{
	u8 bDat;
	int Size = Read(ID, MemAddr, &bDat, 1);
	if(Size!=1){
		return -1;
	}else{
		return bDat;
	}
}

//读2字节，超时返回-1
int SCS::readWord(u8 ID, u8 MemAddr)
{	
	u8 nDat[2];
	int Size;
	u16 wDat;
	Size = Read(ID, MemAddr, nDat, 2);
	if(Size!=2)
		return -1;
	wDat = SCS2Host(nDat[0], nDat[1]);
	return wDat;
}

//Ping指令，返回舵机ID，超时返回-1
int SCS::Ping(u8 ID)
{
	rFlushSCS();
	writeBuf(ID, 0, NULL, 0, INST_PING);
	wFlushSCS();
	u8 replyID = 0;
	return receiveStatus(ID, NULL, 0, &replyID) ? replyID : -1;
}

int SCS::checkHead()
{
	u8 bDat;
	u8 bBuf[] = {0, 0};
	u8 Cnt = 0;
	while(1){
		if(!readSCS(&bDat, 1)){
			return 0;
		}
		bBuf[1] = bBuf[0];
		bBuf[0] = bDat;
		if(bBuf[0]==0xff && bBuf[1]==0xff){
			break;
		}
		Cnt++;
		if(Cnt>10){
			return 0;
		}
	}
	return 1;
}

int SCS::Ack(u8 ID)
{
	u8Error = 0;
	if (ID != 0xfe && Level) return receiveStatus(ID, NULL, 0, NULL);
	return 1;
}

int	SCS::syncReadPacketTx(u8 ID[], u8 IDN, u8 MemAddr, u8 nLen)
{
	rFlushSCS();
	syncReadRxPacketLen = nLen;
	u8 checkSum = (4+0xfe) + IDN + MemAddr + nLen + INST_SYNC_READ;
	u8 i;
	writeSCS(0xff);
	writeSCS(0xff);
	writeSCS(0xfe);
	writeSCS(IDN+4);
	writeSCS(INST_SYNC_READ);
	writeSCS(MemAddr);
	writeSCS(nLen);
	for(i=0; i<IDN; i++){
		writeSCS(ID[i]);
		checkSum += ID[i];
	}
	checkSum = ~checkSum;
	writeSCS(checkSum);
	wFlushSCS();
	
	syncReadRxBuffLen = readSCS(syncReadRxBuff, syncReadRxBuffMax, syncTimeOut);
	return syncReadRxBuffLen;
}

void SCS::syncReadBegin(u8 IDN, u8 rxLen, u32 TimeOut)
{
	syncReadRxBuffMax = IDN*(rxLen+6);
	syncReadRxBuff = new u8[syncReadRxBuffMax];
	syncTimeOut = TimeOut;
}

void SCS::syncReadEnd()
{
	if(syncReadRxBuff){
		delete syncReadRxBuff;
		syncReadRxBuff = NULL;
	}
}

int SCS::syncReadPacketRx(u8 ID, u8 *nDat)
{
	u16 syncReadRxBuffIndex = 0;
	syncReadRxPacket = nDat;
	syncReadRxPacketIndex = 0;
	u8Error = 0;
	while((syncReadRxBuffIndex+6+syncReadRxPacketLen)<=syncReadRxBuffLen){
		u8 bBuf[] = {0, 0, 0};
		u8 calSum = 0;
		while(syncReadRxBuffIndex<syncReadRxBuffLen){
			bBuf[0] = bBuf[1];
			bBuf[1] = bBuf[2];
			bBuf[2] = syncReadRxBuff[syncReadRxBuffIndex++];
			if(bBuf[0]==0xff && bBuf[1]==0xff && bBuf[2]!=0xff){
				break;
			}
		}
		if(bBuf[2]!=ID){
			continue;
		}
		if(syncReadRxBuff[syncReadRxBuffIndex++]!=(syncReadRxPacketLen+2)){
			continue;
		}
		u8Status = syncReadRxBuff[syncReadRxBuffIndex++];
		calSum = ID + (syncReadRxPacketLen+2) + u8Status;
		for(u8 i=0; i<syncReadRxPacketLen; i++){
			syncReadRxPacket[i] = syncReadRxBuff[syncReadRxBuffIndex++];
			calSum += syncReadRxPacket[i];
		}
		calSum = ~calSum;
		if(calSum!=syncReadRxBuff[syncReadRxBuffIndex++]){
			u8Error = ERR_CRC_CMP;
			return 0;
		}
		return syncReadRxPacketLen;
	}
	return 0;
}

int SCS::syncReadRxPacketToByte()
{
	if(syncReadRxPacketIndex>=syncReadRxPacketLen){
		u8Error = ERR_BUFF_LEN;
		return -1;
	}
	return syncReadRxPacket[syncReadRxPacketIndex++];
}

int SCS::syncReadRxPacketToWrod(u8 negBit)
{
	if((syncReadRxPacketIndex+1)>=syncReadRxPacketLen){
		u8Error = ERR_BUFF_LEN;
		return -1;
	}
	int Word = SCS2Host(syncReadRxPacket[syncReadRxPacketIndex], syncReadRxPacket[syncReadRxPacketIndex+1]);
	syncReadRxPacketIndex += 2;
	if(negBit){
		if(Word&(1<<negBit)){
			Word = -(Word & ~(1<<negBit));
		}
	}
	return Word;
}

int SCS::Reset(u8 ID)
{
	rFlushSCS();
	writeBuf(ID, 0, NULL, 0, INST_RESET);
	wFlushSCS();
	return Ack(ID);
}

int SCS::Recal(u8 ID)
{
	rFlushSCS();
	writeBuf(ID, 0, NULL, 0, INST_CAL);
	wFlushSCS();
	return Ack(ID);
}
