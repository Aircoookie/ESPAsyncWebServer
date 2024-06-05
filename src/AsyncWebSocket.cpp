/*
  Asynchronous WebServer library for Espressif MCUs

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "Arduino.h"
#include "AsyncWebSocket.h"

#include <libb64/cencode.h>

#ifndef ESP8266
#if ESP_IDF_VERSION_MAJOR < 5
#include "./port/SHA1Builder.h"
#else
#include <SHA1Builder.h>
#endif
#else
#include <Hash.h>
#endif
#ifdef ESP32
#if ESP_IDF_VERSION_MAJOR >= 5
#include "rom/ets_sys.h"
#endif
#endif

#ifdef ASYNCWEBSERVER_DEBUG_TRACE
#define DEBUG_PRINTFP(fmt, ...) Serial.printf_P(PSTR("[%u]{%d}" fmt "\n"), (unsigned) millis(), ESP.getFreeHeap(), ##__VA_ARGS__)
#else
#define DEBUG_PRINTFP(...)
#endif

#define MAX_PRINTF_LEN 64
#define MAX_HEADER_SIZE 8

// Return a guess at the maximum packet size we can send
static size_t webSocketSendFrameWindow(AsyncClient *client){
  if(!client->canSend())
    return 0;
  size_t space = client->space();
  if(space <= MAX_HEADER_SIZE)
    return 0;
  // TODO - consider if we have enough contiguous RAM to allocate  
  return space - MAX_HEADER_SIZE;
}

static size_t webSocketHeaderLength(bool mask, size_t len) {
  return ((len < 126)?2:4) + (mask * 4);
}

static size_t webSocketSendFrameHeader(AsyncClient *client, bool final, uint8_t opcode, uint8_t mask_buf[4], size_t len){
  if(!client->canSend())
    return 0;

  uint8_t buf[8];  // header buffer
  uint8_t headLen = webSocketHeaderLength(mask_buf != nullptr, len);

  if(client->space() < headLen)
    return 0;

  buf[0] = opcode & 0x0F;
  if(final)
    buf[0] |= 0x80;
  if(len < 126)
    buf[1] = len & 0x7F;
  else {
    buf[1] = 126;
    buf[2] = (uint8_t)((len >> 8) & 0xFF);
    buf[3] = (uint8_t)(len & 0xFF);
  }
  if(len && mask_buf){
    buf[1] |= 0x80;

    for (int i = 0; i < 4; ++i) {
      buf[headLen-4+i] = mask_buf[i] = rand() % 0xFF;
    }
  }

  size_t sent = client->add((const char *)buf, headLen);
  if(sent != headLen){
    //os_printf("error adding %lu header bytes\n", headLen);
    // we are in BIG trouble as we don't cache the headers...!
    // TODO: might be better to close the connection here
  }
  return sent;
}

static size_t webSocketSendFrame(AsyncClient *client, bool final, uint8_t opcode, bool mask, uint8_t *data, size_t len){
  uint8_t mbuf[4];
  if (webSocketSendFrameHeader(client, final, opcode, mask ? mbuf : nullptr, len) == 0) {
    return 0;
  }

  size_t added = 0;
  if(len){
    // Apply the mask
    if(len && mask){
      size_t i;
      for(i=0;i<len;i++)
        data[i] = data[i] ^ mbuf[i%4];
    }    
    added = client->add((const char *)data, len);
  }
  client->send();
  return added;
}



/*
 * Control Frame
 */

class AsyncWebSocketControl {
  private:
    uint8_t _opcode;
    DynamicBuffer _buf;
    bool _mask;
    bool _finished;
  public:
    AsyncWebSocketControl(uint8_t opcode, DynamicBuffer buf, bool mask=false)
      :_opcode(opcode)
      ,_buf(std::move(buf))
      ,_mask(_buf.size() && mask)
      ,_finished(false)
      {}

    AsyncWebSocketControl(uint8_t opcode, const void *data=NULL, size_t len=0, bool mask=false)
      : AsyncWebSocketControl(opcode, DynamicBuffer(data,len), mask)
      {}

    ~AsyncWebSocketControl(){}
    bool finished() const { return _finished; }
    uint8_t opcode(){ return _opcode; }
    uint8_t len(){ return _buf.size() + 2; }
    size_t send(AsyncClient *client){
      _finished = true;
      return webSocketSendFrame(client, true, _opcode & 0x0F, _mask, reinterpret_cast<uint8_t*>(_buf.data()), _buf.size());
    }
};

/*
 * Basic Buffered Message
 */


AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(const char * data, size_t len, uint8_t opcode, bool mask)
  :_len(len)
  ,_attempted(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
{
  _opcode = opcode & 0x07;
  _mask = mask;
  _data = (uint8_t*)malloc(_len+1);
  if(_data == NULL){
    _len = 0;
    _status = WS_MSG_ERROR;
  } else {
    _status = WS_MSG_SENDING;
    memcpy(_data, data, _len);
    _data[_len] = 0;
  }
}
AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(uint8_t opcode, bool mask)
  :_len(0)
  ,_attempted(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
  ,_data(NULL)
{
  _opcode = opcode & 0x07;
  _mask = mask;
  
}


AsyncWebSocketBasicMessage::~AsyncWebSocketBasicMessage() {
  if(_data != NULL)
    free(_data);
}

 void AsyncWebSocketBasicMessage::ack(size_t len, uint32_t time)  {
   (void)time;
  _acked += len;
  if(_sent == _len && _acked == _ack){
    _status = WS_MSG_SENT;
  }
}
 size_t AsyncWebSocketBasicMessage::send(AsyncClient *client)  {
  DEBUG_PRINTFP("BasicMessage %d - %d (%d/%d/%d - %d/%d)", (intptr_t) this, _status, _sent, _attempted, _len, _acked, _ack);
  if(_status != WS_MSG_SENDING)
    return 0;
  if(_acked < _ack){
    return 0;
  }
  if(_sent == _len){
    if(_acked == _ack)
      _status = WS_MSG_SENT;
    return 0;
  }
  if(_sent > _len){
      _status = WS_MSG_ERROR;
      return 0;
  }
  if (_sent < _attempted) {
    // Frame was truncated
    size_t sent = client->write((const char*)(_data + _sent), _attempted - _sent);
    _ack += sent;
    _sent += sent;
    return sent;
  }

  size_t toSend = _len - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if(window < toSend) {
      toSend = window;
  }

  _attempted += toSend;
  _sent += toSend;
  _ack += toSend + webSocketHeaderLength(_mask, toSend);

  bool final = (_sent == _len);
  uint8_t* dPtr = (uint8_t*)(_data + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend)?_opcode:(uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;
  if(toSend && sent != toSend){
      _attempted -= (toSend - sent);
      _ack -= (toSend - sent);
      // TODO - what if header never sent
  }
  return sent;
}

// bool AsyncWebSocketBasicMessage::reserve(size_t size) { 
//   if (size) {
//     _data = (uint8_t*)malloc(size +1);
//     if (_data) {
//       memset(_data, 0, size); 
//       _len = size; 
//       _status = WS_MSG_SENDING;
//       return true; 
//     }
//   }
//   return false; 
//  }


/*
 * AsyncWebSocketMultiMessage Message
 */


AsyncWebSocketMultiMessage::AsyncWebSocketMultiMessage(AsyncWebSocketSharedBuffer buffer, uint8_t opcode, bool mask)
  :_attempted(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
  ,_WSbuffer(std::move(buffer))
{
  _opcode = opcode & 0x07;
  _mask = mask;

  if (_WSbuffer) {
    _status = WS_MSG_SENDING;
    //ets_printf("M: %u\n", _len);
  } else {
    _status = WS_MSG_ERROR;
  }
} 


AsyncWebSocketMultiMessage::~AsyncWebSocketMultiMessage() {
}

 void AsyncWebSocketMultiMessage::ack(size_t len, uint32_t time)  {
   (void)time;
  _acked += len;
  if(_sent >= _WSbuffer.size() && _acked >= _ack){
    _status = WS_MSG_SENT;
  }
  //ets_printf("A: %u\n", len);
}
 size_t AsyncWebSocketMultiMessage::send(AsyncClient *client)  {
  DEBUG_PRINTFP("MultiMessage %d - %d (%d/%d/%d - %d/%d)", (intptr_t) this, _status, _sent, _attempted, _WSbuffer.size(), _acked, _ack);
  if(_status != WS_MSG_SENDING)
    return 0;
  if(_acked < _ack){
    return 0;
  }
  if(_sent == _WSbuffer.size()){
    _status = WS_MSG_SENT;
    return 0;
  }
  if(_sent > _WSbuffer.size()){
      _status = WS_MSG_ERROR;
      //ets_printf("E: %u > %u\n", _sent, _len);
      return 0;
  }
  if (_sent < _attempted) {
    // Frame was truncated
    size_t sent = client->write(_WSbuffer.data() + _sent, _attempted - _sent);
    _ack += sent;
    _sent += sent;
    return sent;
  }  

  size_t toSend = _WSbuffer.size() - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if(window < toSend) {
      toSend = window;
  }

  _attempted += toSend;
  _sent += toSend;
  _ack += toSend + webSocketHeaderLength(_mask, toSend);

  //ets_printf("W: %u %u\n", _sent - toSend, toSend);

  bool final = (_sent == _WSbuffer.size());
  uint8_t* dPtr = (uint8_t*)(_WSbuffer.data() + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend)?_opcode:(uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;
  if(toSend && sent != toSend){
      //ets_printf("E: %u != %u\n", toSend, sent);
      _sent -= (toSend - sent);
      _ack -= (toSend - sent);
      // TODO - what if header never sent
  }
  //ets_printf("S: %u %u\n", _sent, sent);
  return sent;
}


AsyncWebSocketMultiMessage* AsyncWebSocketMultiMessage::clone() const {
  // Safely instantiate a new copy of this message.
  // If we aren't masking, we can share the buffer - otherwise copy the buffer contents
  if (_mask && _WSbuffer) {
    return new AsyncWebSocketMultiMessage(SharedBuffer(_WSbuffer.copy()), _opcode, _mask);
  } else {
    return new AsyncWebSocketMultiMessage(_WSbuffer, _opcode, _mask);
  }
}


/*
 * Async WebSocket Client
 */
 const char AWSC_PING_PAYLOAD[] PROGMEM = "awscPING";
 constexpr size_t AWSC_PING_PAYLOAD_LEN = sizeof(AWSC_PING_PAYLOAD);

AsyncWebSocketClient::AsyncWebSocketClient(AsyncWebServerRequest *request, AsyncWebSocket *server)
  : _controlQueue([](AsyncWebSocketControl *c){ delete  c; })
  , _messageQueue([](AsyncWebSocketMessage *m){ delete  m; })
  , _tempObject(NULL)
{
  _client = request->client();
  _server = server;
  _clientId = _server->_getNextId();
  _status = WS_CONNECTED;
  _pstate = 0;
  _partialHeaderLen = 0;
  _lastMessageTime = millis();
  _keepAlivePeriod = 0;
  _client->setRxTimeout(0);
  _client->onError([](void *r, AsyncClient* c, int8_t error){ (void)c; ((AsyncWebSocketClient*)(r))->_onError(error); }, this);
  _client->onAck([](void *r, AsyncClient* c, size_t len, uint32_t time){ (void)c; ((AsyncWebSocketClient*)(r))->_onAck(len, time); }, this);
  _client->onDisconnect([](void *r, AsyncClient* c){ ((AsyncWebSocketClient*)(r))->_onDisconnect(); delete c; }, this);
  _client->onTimeout([](void *r, AsyncClient* c, uint32_t time){ (void)c; ((AsyncWebSocketClient*)(r))->_onTimeout(time); }, this);
  _client->onData([](void *r, AsyncClient* c, void *buf, size_t len){ (void)c; ((AsyncWebSocketClient*)(r))->_onData(buf, len); }, this);
  _client->onPoll([](void *r, AsyncClient* c){ (void)c; ((AsyncWebSocketClient*)(r))->_onPoll(); }, this);
  _server->_addClient(this);
  _server->_handleEvent(this, WS_EVT_CONNECT, request, NULL, 0);
  delete request;
}

AsyncWebSocketClient::~AsyncWebSocketClient(){
  _messageQueue.free();
  _controlQueue.free();
  _server->_handleEvent(this, WS_EVT_DISCONNECT, NULL, NULL, 0);
}

void AsyncWebSocketClient::_onAck(size_t len, uint32_t time){
  _lastMessageTime = millis();
  if(!_controlQueue.isEmpty()){
    auto head = _controlQueue.front();
    if(head->finished()){
      len -= head->len();
      if(_status == WS_DISCONNECTING && head->opcode() == WS_DISCONNECT){
        _controlQueue.remove(head);
        _status = WS_DISCONNECTED;
        _client->close(true);
        return;
      }
      _controlQueue.remove(head);
    }
  }
  if(len && !_messageQueue.isEmpty()){
    _messageQueue.front()->ack(len, time);
  }
  _runQueue();
}

void AsyncWebSocketClient::_onPoll(){
  if(_client->canSend() && (!_controlQueue.isEmpty() || !_messageQueue.isEmpty())){
    _runQueue();
  } else if(_keepAlivePeriod > 0 && _controlQueue.isEmpty() && _messageQueue.isEmpty() && (millis() - _lastMessageTime) >= _keepAlivePeriod){
    ping(AWSC_PING_PAYLOAD, AWSC_PING_PAYLOAD_LEN);
  }
}

void AsyncWebSocketClient::_runQueue(){
  while(!_messageQueue.isEmpty() && _messageQueue.front()->finished()){
    _messageQueue.remove(_messageQueue.front());
  }

  if(!_controlQueue.isEmpty() && (_messageQueue.isEmpty() || _messageQueue.front()->betweenFrames()) && webSocketSendFrameWindow(_client) > (size_t)(_controlQueue.front()->len() - 1)){
    _controlQueue.front()->send(_client);
  } else if(!_messageQueue.isEmpty() && _messageQueue.front()->betweenFrames() && webSocketSendFrameWindow(_client)){
    _messageQueue.front()->send(_client);
  }
}

bool AsyncWebSocketClient::queueIsFull(){
  if((_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES) || (_status != WS_CONNECTED) ) return true;
  return false;
}

//Added by Aircoookie for WLED
uint16_t AsyncWebSocketClient::queueLength(){
  return _messageQueue.length();
}

void AsyncWebSocketClient::_queueMessage(AsyncWebSocketMessage *dataMessage){
  if(dataMessage == NULL)
    return;
  if(_status != WS_CONNECTED){
    delete dataMessage;
    return;
  }
  if(_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES){
      ets_printf("ERR WSq\n");
      delete dataMessage;
  } else {
      _messageQueue.add(dataMessage);
  }
  if(_client->canSend())
    _runQueue();
}

void AsyncWebSocketClient::_queueControl(AsyncWebSocketControl *controlMessage){
  if(controlMessage == NULL)
    return;
  _controlQueue.add(controlMessage);
  if(_client->canSend())
    _runQueue();
}

void AsyncWebSocketClient::close(uint16_t code, const char * message){
  if(_status != WS_CONNECTED)
    return;
  if(code){
    uint8_t packetLen = 2;
    if(message != NULL){
      size_t mlen = strlen(message);
      if(mlen > 123) mlen = 123;
      packetLen += mlen;
    }
    auto dbuf = DynamicBuffer(packetLen);
    if(dbuf){
      char* buf = dbuf.data();
      buf[0] = (uint8_t)(code >> 8);
      buf[1] = (uint8_t)(code & 0xFF);
      if(message != NULL){
        memcpy(buf+2, message, packetLen -2);
      }
      _queueControl(new AsyncWebSocketControl(WS_DISCONNECT,std::move(dbuf)));
      return;
    }
  }
  _queueControl(new AsyncWebSocketControl(WS_DISCONNECT));
}

void AsyncWebSocketClient::ping(const void *data, size_t len){
  if(_status == WS_CONNECTED)
    _queueControl(new AsyncWebSocketControl(WS_PING, data, len));
}

void AsyncWebSocketClient::_onError(int8_t){}

void AsyncWebSocketClient::_onTimeout(uint32_t time){
  (void)time;
  _client->close(true);
}

void AsyncWebSocketClient::_onDisconnect(){
  _client = NULL;
  _server->_handleDisconnect(this);
}

void AsyncWebSocketClient::_onData(void *pbuf, size_t plen){
  _lastMessageTime = millis();
  uint8_t *data = (uint8_t*)pbuf;
  while(plen > 0){
    if(!_pstate) {
      ssize_t dataPayloadOffset = 0;
      const uint8_t *headerBuf = data;

      // plen is backed up to initialPlen because, in case we receive a partial header, we would like to undo all of our
      // parsing and copy all of what we have of the header into a buffer for later use.
      // plen is modified during the parsing attempt, so if we don't back it up we won't know how much we need to copy.
      // partialHeaderLen is also backed up for the same reason.
      size_t initialPlen = plen;
      size_t partialHeaderLen = 0;

      if (_partialHeaderLen > 0) {
        // We previously received a truncated header. Recover it by doing the following:
        // - Copy the new header chunk into the previous partial header, filling the buffer. It is allocated as a
        //   buffer in a class field.
        // - Change *headerBuf to point to said buffer
        // - Update the length counters so that:
        //   - The initialPlen and plen, which refer to the length of the remaining packet data, also accounts for the
        //     previously received truncated header
        //   - The dataPayloadOffset, which is the offset after the header at which the payload begins, so that it
        //     refers to a point potentially before the beginning of the buffer. As we parse the header we increment it,
        //     and we can pretty much guarantee it will go back to being positive unless there is a major bug.
        //   - The class _partialHeaderLen is back to zero since we took ownership of the contained data.
        memcpy(_partialHeader + _partialHeaderLen, data,
               std::min(plen, (size_t) WS_MAX_HEADER_LEN - _partialHeaderLen));
        headerBuf = _partialHeader;
        initialPlen += _partialHeaderLen;
        plen += _partialHeaderLen;
        dataPayloadOffset -= _partialHeaderLen;
        partialHeaderLen = _partialHeaderLen;

        _partialHeaderLen = 0;
      }

      // The following series of gotos could have been a try-catch but we are likely being built with -fno-exceptions
      if (plen < 2)
        goto _exceptionHandleFailPartialHeader;

      _pinfo.index = 0;
      _pinfo.final = (headerBuf[0] & 0x80) != 0;
      _pinfo.opcode = headerBuf[0] & 0x0F;
      _pinfo.masked = (headerBuf[1] & 0x80) != 0;
      _pinfo.len = headerBuf[1] & 0x7F;
      dataPayloadOffset += 2;
      plen -= 2;

      if (_pinfo.len == 126) {
        if (plen < 2)
          goto _exceptionHandleFailPartialHeader;

        _pinfo.len = headerBuf[3] | (uint16_t)(headerBuf[2]) << 8;
        dataPayloadOffset += 2;
        plen -= 2;
      } else if (_pinfo.len == 127) {
        if (plen < 8)
          goto _exceptionHandleFailPartialHeader;

        _pinfo.len = headerBuf[9] | (uint16_t)(headerBuf[8]) << 8 | (uint32_t)(headerBuf[7]) << 16 |
                     (uint32_t)(headerBuf[6]) << 24 | (uint64_t)(headerBuf[5]) << 32 | (uint64_t)(headerBuf[4]) << 40 |
                     (uint64_t)(headerBuf[3]) << 48 | (uint64_t)(headerBuf[2]) << 56;
        dataPayloadOffset += 8;
        plen -= 8;
      }

      if (_pinfo.masked) {
        if (plen < 4)
          goto _exceptionHandleFailPartialHeader;

        memcpy(_pinfo.mask, headerBuf + dataPayloadOffset + partialHeaderLen, 4);
        dataPayloadOffset += 4;
        plen -= 4;
      }

      // Yes I know the control flow here isn't 100% legible but we must support -fno-exceptions.
      // If we got to this point it means we did NOT receive a truncated header, therefore we can skip the exception
      // handling.
      // Control flow resumes after the following block.
      goto _headerParsingSuccessful;

      // We DID receive a truncated header:
      // - We copy it to our buffer and set the _partialHeaderLen
      // - We return early
      // This will trigger the partial recovery at the next call of this method, once more data is received and we have
      // a full header.
      _exceptionHandleFailPartialHeader:
      {
        if (initialPlen <= WS_MAX_HEADER_LEN) {
          // If initialPlen > WS_MAX_HEADER_LEN there must be something wrong with this code. It should never happen but
          // but it's better safe than sorry.
          memcpy(_partialHeader, headerBuf, initialPlen * sizeof(uint8_t));
          _partialHeaderLen = initialPlen;
        } else {
          DEBUGF("[AsyncWebSocketClient::_onData] initialPlen (= %d) > WS_MAX_HEADER_LEN (= %d)\n", initialPlen,
                 WS_MAX_HEADER_LEN);
        }
        return;
      }

      _headerParsingSuccessful:

      data += dataPayloadOffset;
    }

    const size_t datalen = std::min((size_t)(_pinfo.len - _pinfo.index), plen);

    if(_pinfo.masked){
      for(size_t i=0;i<datalen;i++)
        data[i] ^= _pinfo.mask[(_pinfo.index+i)%4];
    }

    if((datalen + _pinfo.index) < _pinfo.len){
      _pstate = 1;

      if(_pinfo.index == 0){
        if(_pinfo.opcode){
          _pinfo.message_opcode = _pinfo.opcode;
          _pinfo.num = 0;
        } else _pinfo.num += 1;
      }
      _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, (uint8_t*)data, datalen);

      _pinfo.index += datalen;
    } else if((datalen + _pinfo.index) == _pinfo.len){
      _pstate = 0;
      if(_pinfo.opcode == WS_DISCONNECT){
        if(datalen){
          uint16_t reasonCode = (uint16_t)(data[0] << 8) + data[1];
          char * reasonString = (char*)(data+2);
          if(reasonCode > 1001){
            _server->_handleEvent(this, WS_EVT_ERROR, (void *)&reasonCode, (uint8_t*)reasonString, strlen(reasonString));
          }
        }
        if(_status == WS_DISCONNECTING){
          _status = WS_DISCONNECTED;
          _client->close(true);
        } else {
          _status = WS_DISCONNECTING;
          _client->ackLater();
          _queueControl(new AsyncWebSocketControl(WS_DISCONNECT, data, datalen));
        }
      } else if(_pinfo.opcode == WS_PING){
        _queueControl(new AsyncWebSocketControl(WS_PONG, data, datalen));
      } else if(_pinfo.opcode == WS_PONG){
        if(datalen != AWSC_PING_PAYLOAD_LEN || memcmp(AWSC_PING_PAYLOAD, data, AWSC_PING_PAYLOAD_LEN) != 0)
          _server->_handleEvent(this, WS_EVT_PONG, NULL, data, datalen);
      } else if(_pinfo.opcode < 8){//continuation or text/binary frame
        _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, data, datalen);
      }
    } else {
      //os_printf("frame error: len: %u, index: %llu, total: %llu\n", datalen, _pinfo.index, _pinfo.len);
      //what should we do?
      break;
    }

    data += datalen;
    plen -= datalen;
  }
}

size_t AsyncWebSocketClient::printf(const char *format, ...) {
  va_list arg;
  va_start(arg, format);
  char* temp = new char[MAX_PRINTF_LEN];
  if(!temp){
    va_end(arg);
    return 0;
  }
  char* buffer = temp;
  size_t len = vsnprintf(temp, MAX_PRINTF_LEN, format, arg);
  va_end(arg);

  if (len > (MAX_PRINTF_LEN - 1)) {
    buffer = new char[len + 1];
    if (!buffer) {
   	  delete[] temp;
      return 0;
    }
    va_start(arg, format);
    vsnprintf(buffer, len + 1, format, arg);
    va_end(arg);
  }
  text(buffer, len);
  if (buffer != temp) {
    delete[] buffer;
  }
  delete[] temp;
  return len;
}

#ifndef ESP32
size_t AsyncWebSocketClient::printf_P(PGM_P formatP, ...) {
  va_list arg;
  va_start(arg, formatP);
  char* temp = new char[MAX_PRINTF_LEN];
  if(!temp){
    va_end(arg);
    return 0;
  }
  char* buffer = temp;
  size_t len = vsnprintf_P(temp, MAX_PRINTF_LEN, formatP, arg);
  va_end(arg);

  if (len > (MAX_PRINTF_LEN - 1)) {
    buffer = new char[len + 1];
    if (!buffer) {
   	  delete[] temp;
      return 0;
    }
    va_start(arg, formatP);
    vsnprintf_P(buffer, len + 1, formatP, arg);
    va_end(arg);
  }
  text(buffer, len);
  if (buffer != temp) {
    delete[] buffer;
  }
  delete[] temp;
  return len;
}
#endif

void AsyncWebSocketClient::text(const char * message, size_t len){
  _queueMessage(new AsyncWebSocketBasicMessage(message, len));
}
void AsyncWebSocketClient::text(const char * message){
  text(message, strlen(message));
}
void AsyncWebSocketClient::text(uint8_t * message, size_t len){
  text((const char *)message, len);
}
void AsyncWebSocketClient::text(char * message){
  text(message, strlen(message));
}
void AsyncWebSocketClient::text(const String &message){
  text(message.c_str(), message.length());
}
void AsyncWebSocketClient::text(const __FlashStringHelper *data){
  PGM_P p = reinterpret_cast<PGM_P>(data);
  size_t n = 0;
  while (1) {
    if (pgm_read_byte(p+n) == 0) break;
      n += 1;
  }
  char * message = (char*) malloc(n+1);
  if(message){
    for(size_t b=0; b<n; b++)
      message[b] = pgm_read_byte(p++);
    message[n] = 0;
    text(message, n);
    free(message);
  }
}
void AsyncWebSocketClient::text(AsyncWebSocketBuffer buffer)
{
  _queueMessage(new AsyncWebSocketMultiMessage(std::move(buffer)));
}
void AsyncWebSocketClient::text(AsyncWebSocketMessageBuffer* buffer)
{
  if (!buffer) return;
  text(std::move(buffer->_buf));
  delete buffer;
}

void AsyncWebSocketClient::binary(const char * message, size_t len){
  _queueMessage(new AsyncWebSocketBasicMessage(message, len, WS_BINARY));
}
void AsyncWebSocketClient::binary(const char * message){
  binary(message, strlen(message));
}
void AsyncWebSocketClient::binary(uint8_t * message, size_t len){
  binary((const char *)message, len);
}
void AsyncWebSocketClient::binary(char * message){
  binary(message, strlen(message));
}
void AsyncWebSocketClient::binary(const String &message){
  binary(message.c_str(), message.length());
}
void AsyncWebSocketClient::binary(const __FlashStringHelper *data, size_t len){
  PGM_P p = reinterpret_cast<PGM_P>(data);
  char * message = (char*) malloc(len);
  if(message){
    for(size_t b=0; b<len; b++)
      message[b] = pgm_read_byte(p++);
    binary(message, len);
    free(message); 
  }
  
}
void AsyncWebSocketClient::binary(AsyncWebSocketBuffer buffer)
{
  _queueMessage(new AsyncWebSocketMultiMessage(std::move(buffer), WS_BINARY));
}

void AsyncWebSocketClient::binary(AsyncWebSocketMessageBuffer* buffer)
{
  if (!buffer) return;
  binary(std::move(buffer->_buf));
  delete buffer;
}

IPAddress AsyncWebSocketClient::remoteIP() {
    if(!_client) {
        return IPAddress((uint32_t)0U);
    }
    return _client->remoteIP();
}

uint16_t AsyncWebSocketClient::remotePort() {
    if(!_client) {
        return 0;
    }
    return _client->remotePort();
}



/*
 * Async Web Socket - Each separate socket location
 */

AsyncWebSocket::AsyncWebSocket(const String& url)
  :_url(url)
  ,_clients([](AsyncWebSocketClient *c){ delete c; })
  ,_cNextId(1)
  ,_enabled(true)
{
  _eventHandler = NULL;
}

AsyncWebSocket::~AsyncWebSocket(){}

void AsyncWebSocket::_handleEvent(AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(_eventHandler != NULL){
    _eventHandler(this, client, type, arg, data, len);
  }
}

void AsyncWebSocket::_addClient(AsyncWebSocketClient * client){
  _clients.add(client);
}

void AsyncWebSocket::_handleDisconnect(AsyncWebSocketClient * client){
  
  _clients.remove_first([=](AsyncWebSocketClient * c){
    return c->id() == client->id();
  });
}

bool AsyncWebSocket::availableForWriteAll(){
  for(const auto& c: _clients){
    if(c->queueIsFull()) return false;
  }
  return true;
}

bool AsyncWebSocket::availableForWrite(uint32_t id){
  for(const auto& c: _clients){
    if(c->queueIsFull() && (c->id() == id )) return false;
  }
  return true;
}

size_t AsyncWebSocket::count() const {
  return _clients.count_if([](AsyncWebSocketClient * c){
    return c->status() == WS_CONNECTED;
  });
}

AsyncWebSocketClient * AsyncWebSocket::client(uint32_t id){
  for(const auto &c: _clients){
    if(c->id() == id && c->status() == WS_CONNECTED){
      return c;
    }
  }
  return nullptr;
}


void AsyncWebSocket::close(uint32_t id, uint16_t code, const char * message){
  AsyncWebSocketClient * c = client(id);
  if(c)
    c->close(code, message);
}

void AsyncWebSocket::closeAll(uint16_t code, const char * message){
  for(const auto& c: _clients){
    if(c->status() == WS_CONNECTED)
      c->close(code, message);
  }
}

void AsyncWebSocket::cleanupClients(uint16_t maxClients)
{
  if (count() > maxClients){
    _clients.front()->close();
  }
}

void AsyncWebSocket::ping(uint32_t id, uint8_t *data, size_t len){
  AsyncWebSocketClient * c = client(id);
  if(c)
    c->ping(data, len);
}

void AsyncWebSocket::pingAll(uint8_t *data, size_t len){
  for(const auto& c: _clients){
    if(c->status() == WS_CONNECTED)
      c->ping(data, len);
  }
}

void AsyncWebSocket::text(uint32_t id, const char * message, size_t len){
  AsyncWebSocketClient * c = client(id);
  if(c)
    c->text(message, len);
}

void AsyncWebSocket::textAll(AsyncWebSocketBuffer buffer){
  if (!buffer) return;
  messageAll(AsyncWebSocketMultiMessage(std::move(buffer)));
}

void AsyncWebSocket::textAll(const char * message, size_t len){
  if ((!message) || (len ==0)) return;
  messageAll(AsyncWebSocketMultiMessage(SharedBuffer{message, len}));
}

void AsyncWebSocket::textAll(const AsyncWebSocketMessageBuffer* buffer){
  if (!buffer) return;
  textAll(std::move(buffer->_buf));
  delete buffer;
}

void AsyncWebSocket::binary(uint32_t id, const char * message, size_t len){
  AsyncWebSocketClient * c = client(id);
  if(c)
    c->binary(message, len);
}

void AsyncWebSocket::binaryAll(const char * message, size_t len){
  binaryAll(DynamicBuffer(message, len));
}

void AsyncWebSocket::binaryAll(AsyncWebSocketBuffer buffer)
{
  messageAll(AsyncWebSocketMultiMessage(std::move(buffer), WS_BINARY));
}

void AsyncWebSocket::binaryAll(const AsyncWebSocketMessageBuffer* buffer){
  if (!buffer) return;
  binaryAll(std::move(buffer->_buf));
  delete buffer;
}

void AsyncWebSocket::message(uint32_t id, AsyncWebSocketMessage *message){
  AsyncWebSocketClient * c = client(id);
  if(c)
    c->message(message);
}

void AsyncWebSocket::messageAll(const AsyncWebSocketMultiMessage &message){
  for(const auto& c: _clients){
    if(c->status() == WS_CONNECTED)
      c->message(message.clone());
  }
}

size_t AsyncWebSocket::printf(uint32_t id, const char *format, ...){
  AsyncWebSocketClient * c = client(id);
  if(c){
    va_list arg;
    va_start(arg, format);
    size_t len = c->printf(format, arg);
    va_end(arg);
    return len;
  }
  return 0;
}

size_t AsyncWebSocket::printfAll(const char *format, ...) {
  va_list arg;
  char* temp = new char[MAX_PRINTF_LEN];
  if(!temp){
    return 0;
  }
  va_start(arg, format);
  size_t len = vsnprintf(temp, MAX_PRINTF_LEN, format, arg);
  va_end(arg);
  delete[] temp;
  
  AsyncWebSocketBuffer buffer(len); 
  if (!buffer) {
    return 0;
  }

  va_start(arg, format);
  vsnprintf( buffer.data(), len, format, arg);
  va_end(arg);

  textAll(buffer);
  return len;
}

#ifndef ESP32
size_t AsyncWebSocket::printf_P(uint32_t id, PGM_P formatP, ...){
  AsyncWebSocketClient * c = client(id);
  if(c != NULL){
    va_list arg;
    va_start(arg, formatP);
    size_t len = c->printf_P(formatP, arg);
    va_end(arg);
    return len;
  }
  return 0;
}
#endif

size_t AsyncWebSocket::printfAll_P(PGM_P formatP, ...) {
  va_list arg;
  char* temp = new char[MAX_PRINTF_LEN];
  if(!temp){
    return 0;
  }
  va_start(arg, formatP);
  size_t len = vsnprintf_P(temp, MAX_PRINTF_LEN, formatP, arg);
  va_end(arg);
  delete[] temp;
  
  AsyncWebSocketBuffer buffer(len); 
  if (!buffer) {
    return 0;
  }

  va_start(arg, formatP);
  vsnprintf_P(buffer.data(), len, formatP, arg);
  va_end(arg);

  textAll(buffer);
  return len;
}

void AsyncWebSocket::text(uint32_t id, const char * message){
  text(id, message, strlen(message));
}
void AsyncWebSocket::text(uint32_t id, uint8_t * message, size_t len){
  text(id, (const char *)message, len);
}
void AsyncWebSocket::text(uint32_t id, char * message){
  text(id, message, strlen(message));
}
void AsyncWebSocket::text(uint32_t id, const String &message){
  text(id, message.c_str(), message.length());
}
void AsyncWebSocket::text(uint32_t id, const __FlashStringHelper *message){
  AsyncWebSocketClient * c = client(id);
  if(c != NULL)
    c->text(message);
}
void AsyncWebSocket::textAll(const char * message){
  textAll(message, strlen(message));
}
void AsyncWebSocket::textAll(uint8_t * message, size_t len){
  textAll((const char *)message, len);
}
void AsyncWebSocket::textAll(char * message){
  textAll(message, strlen(message));
}
void AsyncWebSocket::textAll(const String &message){
  textAll(message.c_str(), message.length());
}
void AsyncWebSocket::textAll(const __FlashStringHelper *message){
  for(const auto& c: _clients){
    if(c->status() == WS_CONNECTED)
      c->text(message);
  }
}
void AsyncWebSocket::binary(uint32_t id, const char * message){
  binary(id, message, strlen(message));
}
void AsyncWebSocket::binary(uint32_t id, uint8_t * message, size_t len){
  binary(id, (const char *)message, len);
}
void AsyncWebSocket::binary(uint32_t id, char * message){
  binary(id, message, strlen(message));
}
void AsyncWebSocket::binary(uint32_t id, const String &message){
  binary(id, message.c_str(), message.length());
}
void AsyncWebSocket::binary(uint32_t id, const __FlashStringHelper *message, size_t len){
  AsyncWebSocketClient * c = client(id);
  if(c != NULL)
    c-> binary(message, len);
}
void AsyncWebSocket::binaryAll(const char * message){
  binaryAll(message, strlen(message));
}
void AsyncWebSocket::binaryAll(uint8_t * message, size_t len){
  binaryAll((const char *)message, len);
}
void AsyncWebSocket::binaryAll(char * message){
  binaryAll(message, strlen(message));
}
void AsyncWebSocket::binaryAll(const String &message){
  binaryAll(message.c_str(), message.length());
}
void AsyncWebSocket::binaryAll(const __FlashStringHelper *message, size_t len){
  for(const auto& c: _clients){
    if(c->status() == WS_CONNECTED)
      c-> binary(message, len);
  }
 }

const char WS_STR_CONNECTION[] PROGMEM = "Connection";
const char WS_STR_UPGRADE[] PROGMEM = "Upgrade";
const char WS_STR_ORIGIN[] PROGMEM = "Origin";
const char WS_STR_VERSION[] PROGMEM = "Sec-WebSocket-Version";
const char WS_STR_KEY[] PROGMEM = "Sec-WebSocket-Key";
const char WS_STR_PROTOCOL[] PROGMEM = "Sec-WebSocket-Protocol";
const char WS_STR_ACCEPT[] PROGMEM = "Sec-WebSocket-Accept";
const char WS_STR_UUID[] PROGMEM = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool AsyncWebSocket::canHandle(AsyncWebServerRequest *request){
  if(!_enabled)
    return false;
  
  if(request->method() != HTTP_GET || !request->url().equals(_url) || !request->isExpectedRequestedConnType(RCT_WS))
    return false;

  request->addInterestingHeader(FPSTR(WS_STR_CONNECTION));
  request->addInterestingHeader(FPSTR(WS_STR_UPGRADE));
  request->addInterestingHeader(FPSTR(WS_STR_ORIGIN));
  request->addInterestingHeader(FPSTR(WS_STR_VERSION));
  request->addInterestingHeader(FPSTR(WS_STR_KEY));
  request->addInterestingHeader(FPSTR(WS_STR_PROTOCOL));
  return true;
}

void AsyncWebSocket::handleRequest(AsyncWebServerRequest *request){
  if(!request->hasHeader(FPSTR(WS_STR_VERSION)) || !request->hasHeader(FPSTR(WS_STR_KEY))){
    request->send(400);
    return;
  }
  if((_username != "" && _password != "") && !request->authenticate(_username.c_str(), _password.c_str())){
    return request->requestAuthentication();
  }
  AsyncWebHeader* version = request->getHeader(FPSTR(WS_STR_VERSION));
  if(version->value().toInt() != 13){
    AsyncWebServerResponse *response = request->beginResponse(400);
    response->addHeader(FPSTR(WS_STR_VERSION),"13");
    request->send(response);
    return;
  }
  AsyncWebHeader* key = request->getHeader(FPSTR(WS_STR_KEY));
  AsyncWebServerResponse *response = new AsyncWebSocketResponse(key->value(), this);
  if(request->hasHeader(FPSTR(WS_STR_PROTOCOL))){
    AsyncWebHeader* protocol = request->getHeader(FPSTR(WS_STR_PROTOCOL));
    //ToDo: check protocol
    response->addHeader(FPSTR(WS_STR_PROTOCOL), protocol->value());
  }
  request->send(response);
}

// Deprecated API
AsyncWebSocketMessageBuffer* AsyncWebSocket::makeBuffer(size_t size)
{
  AsyncWebSocketBuffer buffer(size);  
  if (buffer.size() == 0) {
    return nullptr;
  }
  return new AsyncWebSocketMessageBuffer(std::move(buffer));
}

AsyncWebSocketMessageBuffer* AsyncWebSocket::makeBuffer(const uint8_t * data, size_t size)
{
  AsyncWebSocketBuffer buffer((const char*) data, size); 
  if (buffer.size() == 0) {
    return nullptr;
  }
  return new AsyncWebSocketMessageBuffer(std::move(buffer));
}

const AsyncWebSocket::AsyncWebSocketClientLinkedList& AsyncWebSocket::getClients() const {
  return _clients;
}

/*
 * Response to Web Socket request - sends the authorization and detaches the TCP Client from the web server
 * Authentication code from https://github.com/Links2004/arduinoWebSockets/blob/master/src/WebSockets.cpp#L480
 */

AsyncWebSocketResponse::AsyncWebSocketResponse(const String& key, AsyncWebSocket *server){
  _server = server;
  _code = 101;
  _sendContentLength = false;

  uint8_t hash[20];
  char buffer[33];

#ifdef ESP8266
  sha1(key + FPSTR(WS_STR_UUID), hash);
#else
    String k = key + WS_STR_UUID;
    SHA1Builder sha1;
    sha1.begin();
    sha1.add((const uint8_t*)k.c_str(), k.length());
    sha1.calculate();
    sha1.getBytes(hash);
#endif
  base64_encodestate _state;
  base64_init_encodestate(&_state);
  int len = base64_encode_block((const char *) hash, 20, buffer, &_state);
  len = base64_encode_blockend((buffer + len), &_state);
  addHeader(FPSTR(WS_STR_CONNECTION), FPSTR(WS_STR_UPGRADE));
  addHeader(FPSTR(WS_STR_UPGRADE), F("websocket"));
  addHeader(FPSTR(WS_STR_ACCEPT),buffer);
}

void AsyncWebSocketResponse::_respond(AsyncWebServerRequest *request){
  if(_state == RESPONSE_FAILED){
    request->client()->close(true);
    return;
  }
  String out = _assembleHead(request->version());
  request->client()->write(out.c_str(), _headLength);
  _state = RESPONSE_WAIT_ACK;
}

size_t AsyncWebSocketResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  if(len){
    new AsyncWebSocketClient(request, _server);
  }
  return 0;
}
