/*
 * SCS.cpp
 * 飞特串行舵机通信层协议程序
 * 日期: 2024.12.21
 * 作者: txl
 */

#include <stddef.h>
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
	last_tx_id = ID;
	last_tx_command = Fun;
	last_tx_address = MemAddr;
	u8 msgLen = 2;
	u8 bBuf[6];
	u8 CheckSum = 0;
	bBuf[0] = 0xff;
	bBuf[1] = 0xff;
	bBuf[2] = ID;
	bBuf[4] = Fun;
	if(nDat){
		msgLen += nLen + 1;
		bBuf[3] = msgLen;
		bBuf[5] = MemAddr;
		writeSCS(bBuf, 6);
		
	}else{
		bBuf[3] = msgLen;
		writeSCS(bBuf, 5);
	}
	CheckSum = ID + msgLen + Fun + MemAddr;
	u8 i = 0;
	if(nDat){
		for(i=0; i<nLen; i++){
			CheckSum += nDat[i];
		}
		writeSCS(nDat, nLen);
	}
	writeSCS(~CheckSum);
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
void SCS::recordReadHeader(u8 id, u8 address, u8 count, const u8* header,
                           u8 previous_id, u8 previous_command, u8 previous_address)
{
	if (id != 1 && id != 2) return;
	auto& d = read_header_diag[id - 1][count > 2 ? 1 : 0];
	if (header[0] != id) {
		++d.wrong_id;
		if (header[0] == 0xff) ++d.rx_ff;
		else if (header[0] == 1 || header[0] == 2) ++d.rx_other_servo;
		else if (header[0] == 0) ++d.rx_zero;
		else ++d.rx_other;
	} else {
		++d.wrong_length;
	}
	// These are header patterns only, not validated packets.
	if (header[1] == 2) ++d.ack_length;
	if (header[0] == id && header[1] == 4 && header[2] == INST_READ)
		++d.read_echo_prefix;
	d.address = address;
	d.requested_bytes = count;
	d.rx_id = header[0];
	d.rx_length = header[1];
	d.rx_status = header[2];
	d.previous_id = previous_id;
	d.previous_command = previous_command;
	d.previous_address = previous_address;
}

int SCS::Read(u8 ID, u8 MemAddr, u8 *nData, u8 nLen)
{
	const u8 previous_id = last_tx_id;
	const u8 previous_command = last_tx_command;
	const u8 previous_address = last_tx_address;
	rFlushSCS();
	writeBuf(ID, MemAddr, &nLen, 1, INST_READ);
	wFlushSCS();
	u8Error = 0;
	u8 bBuf[4];
	u8Status = 0;
	// Only discard checksum-valid, zero-data status frames during a data read.
	// Bound the number of skipped replies; keep normal bulk payload reads.
	for (int skipped = 0;;) {
		if (!checkHead() || readSCS(bBuf, 3) != 3) {
			u8Error = ERR_NO_REPLY;
			return 0;
		}
		if (nLen == 0 || bBuf[1] != 2 ||
		    (bBuf[0] != 1 && bBuf[0] != 2) || skipped >= 2) break;
		recordReadHeader(ID, MemAddr, nLen, bBuf, previous_id, previous_command, previous_address);
		if (readSCS(bBuf + 3, 1) != 1) {
			u8Error = ERR_NO_REPLY;
			return 0;
		}
		if (!validAck(bBuf[0], bBuf)) {
			u8Error = ERR_CRC_CMP;
			return 0;
		}
		if (ID == 1 || ID == 2) ++read_header_diag[ID - 1][nLen > 2 ? 1 : 0].ack_skipped;
		++skipped;
	}
	if(bBuf[0]!=ID && ID!=0xfe){
		recordReadHeader(ID, MemAddr, nLen, bBuf, previous_id, previous_command, previous_address);
		u8Error = ERR_SLAVE_ID;
		return 0;
	}
	if(bBuf[1]!=(nLen+2)){
		recordReadHeader(ID, MemAddr, nLen, bBuf, previous_id, previous_command, previous_address);
		u8Error = ERR_BUFF_LEN;
		return 0;
	}
	int Size = readSCS(nData, nLen);
	if(Size!=nLen){
		u8Error = ERR_NO_REPLY;
		return 0;
	}
	if(readSCS(bBuf+3, 1)!=1){
		u8Error = ERR_NO_REPLY;
		return 0;
	}
	u8 calSum = bBuf[0]+bBuf[1]+bBuf[2];
	u8 i;
	for(i=0; i<Size; i++){
		calSum += nData[i];
	}
	calSum = ~calSum;
	if(calSum!=bBuf[3]){
		u8Error = ERR_CRC_CMP;
		return 0;
	}
	u8Status = bBuf[2];
	return Size;
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
int	SCS::Ping(u8 ID)
{
	rFlushSCS();
	writeBuf(ID, 0, NULL, 0, INST_PING);
	wFlushSCS();
	u8Status = 0;
	if(!checkHead()){
		u8Error = ERR_NO_REPLY;
		return -1;
	}
	u8 bBuf[4];
	u8Error = 0;
	if(readSCS(bBuf, 4)!=4){
		u8Error = ERR_NO_REPLY;
		return -1;
	}
	if(bBuf[0]!=ID && ID!=0xfe){
		u8Error = ERR_SLAVE_ID;
		return -1;
	}
	if(bBuf[1]!=2){
		u8Error = ERR_BUFF_LEN;
		return -1;
	}
	u8 calSum = ~(bBuf[0]+bBuf[1]+bBuf[2]);
	if(calSum!=bBuf[3]){
		u8Error = ERR_CRC_CMP;
		return -1;			
	}
	u8Status = bBuf[2];
	return bBuf[0];
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

bool SCS::validAck(u8 id, const u8* body)
{
	return body[0] == id && body[1] == 2 &&
	       static_cast<u8>(~(body[0] + body[1] + body[2])) == body[3];
}

int	SCS::Ack(u8 ID)
{
	u8Error = 0;
	if (!Level && (ID == 1 || ID == 2)) {
		u8 frame[6];
		auto& d = write_ack_diag[ID - 1];
		const int received = readOptionalAck(frame, sizeof(frame));
		if (received == 0) ++d.absent;
		else if (received == sizeof(frame) && frame[0] == 0xff &&
		         frame[1] == 0xff && validAck(ID, frame + 2)) {
			++d.received;
			u8Status = frame[4];
		} else ++d.invalid;
		// Preserve Level=0 write return semantics; missing ACK is not fatal.
		return 1;
	}
	if(ID!=0xfe && Level){
		if(!checkHead()){
			u8Error = ERR_NO_REPLY;
			return 0;
		}
		u8Status = 0;
		u8 bBuf[4];
		if(readSCS(bBuf, 4)!=4){
			u8Error = ERR_NO_REPLY;
			return 0;
		}
		if(bBuf[0]!=ID){
			u8Error = ERR_SLAVE_ID;
			return 0;
		}
		if(bBuf[1]!=2){
			u8Error = ERR_BUFF_LEN;
			return 0;
		}
		u8 calSum = ~(bBuf[0]+bBuf[1]+bBuf[2]);
		if(calSum!=bBuf[3]){
			u8Error = ERR_CRC_CMP;
			return 0;
		}
		u8Status = bBuf[2];
	}
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
