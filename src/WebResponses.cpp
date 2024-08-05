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
#include "ESPAsyncWebServer.h"
#include "WebResponseImpl.h"
#include "cbuf.h"

#ifdef ASYNCWEBSERVER_DEBUG_TRACE
#define DEBUG_PRINTFP(fmt, ...) Serial.printf_P(PSTR("[%u]" fmt), (unsigned) millis(), ##__VA_ARGS__)
#else
#define DEBUG_PRINTFP(...)
#endif

#ifdef ESP8266
#define GET_MAX_BLOCK_SIZE getMaxFreeBlockSize
#else
#define GET_MAX_BLOCK_SIZE getMaxAllocHeap
#endif

// Since ESP8266 does not link memchr by default, here's its implementation.
void* memchr(void* ptr, int ch, size_t count)
{
  unsigned char* p = static_cast<unsigned char*>(ptr);
  while(count--)
    if(*p++ == static_cast<unsigned char>(ch))
      return --p;
  return nullptr;
}


/*
 * Abstract Response
 * */
const __FlashStringHelper* AsyncWebServerResponse::_responseCodeToString(int code) {
  switch (code) {
    case 100: return F("Continue");
    case 101: return F("Switching Protocols");
    case 200: return F("OK");
    case 201: return F("Created");
    case 202: return F("Accepted");
    case 203: return F("Non-Authoritative Information");
    case 204: return F("No Content");
    case 205: return F("Reset Content");
    case 206: return F("Partial Content");
    case 300: return F("Multiple Choices");
    case 301: return F("Moved Permanently");
    case 302: return F("Found");
    case 303: return F("See Other");
    case 304: return F("Not Modified");
    case 305: return F("Use Proxy");
    case 307: return F("Temporary Redirect");
    case 400: return F("Bad Request");
    case 401: return F("Unauthorized");
    case 402: return F("Payment Required");
    case 403: return F("Forbidden");
    case 404: return F("Not Found");
    case 405: return F("Method Not Allowed");
    case 406: return F("Not Acceptable");
    case 407: return F("Proxy Authentication Required");
    case 408: return F("Request Time-out");
    case 409: return F("Conflict");
    case 410: return F("Gone");
    case 411: return F("Length Required");
    case 412: return F("Precondition Failed");
    case 413: return F("Request Entity Too Large");
    case 414: return F("Request-URI Too Large");
    case 415: return F("Unsupported Media Type");
    case 416: return F("Requested range not satisfiable");
    case 417: return F("Expectation Failed");
    case 500: return F("Internal Server Error");
    case 501: return F("Not Implemented");
    case 502: return F("Bad Gateway");
    case 503: return F("Service Unavailable");
    case 504: return F("Gateway Time-out");
    case 505: return F("HTTP Version not supported");
    default:  return F("");
  }
}

AsyncWebServerResponse::AsyncWebServerResponse()
  : _code(0)
  , _headers({})
  , _contentType()
  , _contentLength(0)
  , _sendContentLength(true)
  , _chunked(false)
  , _headLength(0)
  , _sentLength(0)
  , _ackedLength(0)
  , _writtenLength(0)
  , _state(RESPONSE_SETUP)
{
  for(auto& header: DefaultHeaders::Instance()) {
    _headers.add(header);
  }
}

AsyncWebServerResponse::~AsyncWebServerResponse(){
  _headers.free();
}

void AsyncWebServerResponse::setCode(int code){
  if(_state == RESPONSE_SETUP)
    _code = code;
}

void AsyncWebServerResponse::setContentLength(size_t len){
  if(_state == RESPONSE_SETUP)
    _contentLength = len;
}

void AsyncWebServerResponse::setContentType(const String& type){
  if(_state == RESPONSE_SETUP)
    _contentType = type;
}

void AsyncWebServerResponse::addHeader(String name, String value){
  _headers.add(AsyncWebHeader(std::move(name), std::move(value)));
}

String AsyncWebServerResponse::_assembleHead(uint8_t version){
  // Precalculate the output header block length
  size_t est_header_size = 10 + 4 + 2;  // HTTP://1.version code + newlines
  est_header_size += strlen_P((const char*) _responseCodeToString(_code));
  if(_sendContentLength) {
    est_header_size += 18 + 10; // GBs ought to be enough for anyone
  };
  if (_contentType.length()) {
    est_header_size += 16 + _contentType.length();
  }
  if (version) {
    est_header_size += 14 + 5 + 4;
    if (_chunked) {
      est_header_size += 18 + 8 + 4;
    }
  }
  for(const auto& header: _headers) {
    est_header_size += header.name().length() + header.value().length() + 4;
  };

  String out = String();
  out.reserve(est_header_size);
  const static int bufSize = 300;
  char buf[bufSize];

  snprintf_P(buf, bufSize, PSTR("HTTP/1.%d %d "), version, _code);
  out.concat(buf);
  out.concat(_responseCodeToString(_code));
  out.concat(F("\r\n"));

  if(_sendContentLength) {
    snprintf_P(buf, bufSize, PSTR("Content-Length: %d\r\n"), _contentLength);
    out.concat(buf);
  }
  if(_contentType.length()) {
    snprintf_P(buf, bufSize, PSTR("Content-Type: %s\r\n"), _contentType.c_str());
    out.concat(buf);
  }

  for(const auto& header: _headers){
    snprintf_P(buf, bufSize, PSTR("%s: %s\r\n"), header.name().c_str(), header.value().c_str());
    out.concat(buf);
  }
  _headers.free();

  if(version) {
    out.concat(F("Accept-Ranges: none\r\n"));
    if(_chunked) {
      out.concat(F("Transfer-Encoding: chunked\r\n"));
    }
  }

  out.concat(F("\r\n"));
  _headLength = out.length();
  return out;
}

bool AsyncWebServerResponse::_started() const { return _state > RESPONSE_SETUP; }
bool AsyncWebServerResponse::_finished() const { return _state > RESPONSE_WAIT_ACK; }
bool AsyncWebServerResponse::_failed() const { return _state == RESPONSE_FAILED; }
bool AsyncWebServerResponse::_sourceValid() const { return false; }
void AsyncWebServerResponse::_respond(AsyncWebServerRequest *request){ _state = RESPONSE_END; request->client()->close(); }
size_t AsyncWebServerResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){ (void)request; (void)len; (void)time; return 0; }

/*
 * String/Code Response
 * */
AsyncBasicResponse::AsyncBasicResponse(int code, String contentType, String content){
  _code = code;
  _content = std::move(content);
  _contentType = std::move(contentType);
  if(_content.length()){
    _contentLength = _content.length();
    if(!_contentType.length())
      _contentType = FPSTR(CONTENT_TYPE_PLAIN);
  }
  addHeader(F("Connection"),F("close"));
}

void AsyncBasicResponse::_respond(AsyncWebServerRequest *request){
  _state = RESPONSE_HEADERS;
  String out = _assembleHead(request->version());
  size_t outLen = out.length();
  size_t space = request->client()->space();
  if(!_contentLength && space >= outLen){
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_WAIT_ACK;
  } else if(_contentLength && space >= outLen + _contentLength){
    out += _content;
    outLen += _contentLength;
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_WAIT_ACK;
  } else if(space && space < outLen){
    String partial = out.substring(0, space);
    _content = out.substring(space) + _content;
    _contentLength += outLen - space;
    _writtenLength += request->client()->write(partial.c_str(), partial.length());
    _state = RESPONSE_CONTENT;
  } else if(space > outLen && space < (outLen + _contentLength)){
    size_t shift = space - outLen;
    outLen += shift;
    _sentLength += shift;
    out += _content.substring(0, shift);
    _content = _content.substring(shift);
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_CONTENT;
  } else {
    _content = out + _content;
    _contentLength += outLen;
    _state = RESPONSE_CONTENT;
  }
}

size_t AsyncBasicResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  _ackedLength += len;
  if(_state == RESPONSE_CONTENT){
    size_t available = _contentLength - _sentLength;
    size_t space = request->client()->space();
    //we can fit in this packet
    if(space > available){
      _writtenLength += request->client()->write(_content.c_str(), available);
      _content = String();
      _state = RESPONSE_WAIT_ACK;
      return available;
    }
    //send some data, the rest on ack
    String out = _content.substring(0, space);
    _content = _content.substring(space);
    _sentLength += space;
    _writtenLength += request->client()->write(out.c_str(), space);
    return space;
  } else if(_state == RESPONSE_WAIT_ACK){
    if(_ackedLength >= _writtenLength){
      _state = RESPONSE_END;
    }
  }
  return 0;
}


/*
 * Abstract Response
 * */

AsyncAbstractResponse::AsyncAbstractResponse(AwsTemplateProcessor callback): _callback(callback)
{
  // In case of template processing, we're unable to determine real response size
  if(callback) {
    _contentLength = 0;
    _sendContentLength = false;
    _chunked = true;
  }
}

void AsyncAbstractResponse::_respond(AsyncWebServerRequest *request){
  addHeader(F("Connection"),F("close"));
  _head = _assembleHead(request->version());
  _state = RESPONSE_HEADERS;
  _ack(request, 0, 0);
}

template<typename T>
static void dealloc_vector(T& vec) {
  vec = T{};  // move construct an empty vector; space will be freed when scope exits
}

size_t AsyncAbstractResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  DEBUG_PRINTFP("(%d) ack %d\n", (intptr_t) this, len);

  if(!_sourceValid()){
    _state = RESPONSE_FAILED;
    request->client()->close();
    return 0;
  }
  _ackedLength += len;

  size_t space = request->client()->space();  // TCP window space available; NOT a guarantee we can actually send this much  
  size_t headLen = _head.length();
  bool needs_send = false;

  if(_state == RESPONSE_HEADERS){
    auto headWritten = request->client()->add(_head.c_str(), std::min(space, headLen));
    _writtenLength += headWritten;
    if (headWritten < headLen) {
      _head = _head.substring(headWritten);
      request->client()->send();
      return headWritten;
    }
    _state = RESPONSE_CONTENT;
    space -= headWritten;
    _head = String(); // done      
    needs_send = true;
  }

  if(_state == RESPONSE_CONTENT){
    if (_packet.size()) {
      // Complete the cached data
      auto written = request->client()->add((const char*) _packet.data(), std::min(space, (size_t) _packet.size()));
      _writtenLength += written;
      _packet.advance(written);
      space -= written;
      if (_packet.size()) {
        //  Couldn't queue the full cache
        request->client()->send();
        return written;
      }
      needs_send = true;
    }
    _packet.reset();

    size_t outLen, readLen;
    if(_chunked){
      if(space <= 8){
        goto content_abort;
      }
      outLen = space;
    } else if(!_sendContentLength){
      outLen = space;
    } else {
      outLen = std::min(space, _contentLength - _sentLength);
    }
#ifdef ESP8266
    // Limit outlen based on available memory
    // We require two packet buffers - one allocated here, and one belonging to the TCP stack
    {      
      auto old_space = _packet.capacity();
      auto max_block_size = ESP.getMaxFreeBlockSize() - 128;
      if ((old_space < outLen) || (outLen > max_block_size)) {
        DEBUG_PRINTFP("(%d) Space adjustment, have %d, want %d, avail %d\n", (intptr_t)this, old_space, outLen, max_block_size);
        do { 
          dealloc_vector(_packet);
          outLen = std::min(outLen, max_block_size);
          _packet.reallocate(outLen);
          max_block_size = ESP.getMaxFreeBlockSize() - 128;
          DEBUG_PRINTFP("(%d) Checking %d vs %d\n", (intptr_t)this, outLen, max_block_size);
        } while (max_block_size < outLen);
      } else {
        _packet.reallocate(outLen);
      }
    }
#else
    //Serial.printf("[%u] %d/%d -> %d\n", (unsigned) millis(),  ESP.getMaxAllocHeap(), ESP.getFreeHeap(), outLen);
    _packet.reallocate(outLen);
#endif
    
    if(_chunked){      
      // HTTP 1.1 allows leading zeros in chunk length. Or spaces may be added.
      // See RFC2616 sections 2, 3.6.1.
      readLen = _fillBufferAndProcessTemplates((uint8_t*) (_packet.data() + 6), outLen - 8);
      if(readLen == RESPONSE_TRY_AGAIN){
          _packet.clear();
          goto content_abort;
      }
      outLen = sprintf((char*)_packet.data(), "%x", readLen);
      while(outLen < 4) _packet[outLen++] = ' ';
      _packet[outLen++] = '\r';
      _packet[outLen++] = '\n';
      outLen += readLen;
      _packet[outLen++] = '\r';
      _packet[outLen++] = '\n';
    } else {
      readLen = _fillBufferAndProcessTemplates((uint8_t*)_packet.data(), _packet.size());
      if(readLen == RESPONSE_TRY_AGAIN){
          _packet.clear();
          goto content_abort;
      }
      outLen = readLen;
    }
    _packet.resize(outLen);

    if(_packet.size()){      
        auto acceptedLen = request->client()->write((const char*)_packet.data(), _packet.size());
        _writtenLength += acceptedLen;
        _packet.advance(acceptedLen);
        if (acceptedLen < outLen) {
          // Save the unsent block in cache
          DEBUG_PRINTFP("(%d) Incomplete write, %d/%d\nHeap: %d/%d\nSpace:%d\n", (intptr_t) this, acceptedLen, outLen, ESP.GET_MAX_BLOCK_SIZE(), ESP.getFreeHeap(), request->client()->space());
          // Try again, with less
          acceptedLen = request->client()->write((const char*)_packet.data(), _packet.size()/2);
          _writtenLength += acceptedLen;
          _packet.advance(acceptedLen);
        }
        DEBUG_PRINTFP("(%d) Accepted: %d\n", (intptr_t) this, acceptedLen);
        if (_packet.size() == 0) dealloc_vector(_packet);
    }

    if( (_chunked && readLen == 0)  // Chunked mode, no more data
        || (!_sendContentLength && outLen == 0) // No content length, no more data
        || (!_chunked && _writtenLength == (_headLength + _contentLength))) // non chunked mode, all data written
    {
      _state = RESPONSE_WAIT_ACK;
    }
    return outLen;

  } else if(_state == RESPONSE_WAIT_ACK){
    if(!_sendContentLength || _ackedLength >= _writtenLength){
      _state = RESPONSE_END;
      if(!_chunked && !_sendContentLength)
        request->client()->close(true);
    }
  }
  return 0;

content_abort:  
  if (needs_send) {
    request->client()->send();
  }
  return 0;  
}

size_t AsyncAbstractResponse::_readDataFromCacheOrContent(uint8_t* data, const size_t len)
{
    // If we have something in cache, copy it to buffer
    const size_t readFromCache = std::min(len, _cache.size());
    if(readFromCache) {
      memcpy(data, _cache.data(), readFromCache);
      _cache.advance(readFromCache);
    }
    // If we need to read more...
    if (len > readFromCache) {
      const size_t needFromFile = len - readFromCache;
      const size_t readFromContent = _fillBuffer(data + readFromCache, needFromFile);
      if (readFromContent != RESPONSE_TRY_AGAIN) {
        _sentLength += readFromContent;
        return readFromCache + readFromContent;
      }
      if (readFromCache == 0) return readFromContent;
    } 
    return readFromCache;
}

static void push_front(Walkable<DynamicBuffer>& buf, const uint8_t* data, const uint8_t* end) {
  auto size = end - data;
  auto old_size = buf.size();
  auto new_buf = Walkable<DynamicBuffer> { old_size + size };
  // TODO: error checks
  memcpy(new_buf.data(), data, size);
  memcpy(new_buf.data() + size, buf.data(), old_size);
  buf = std::move(new_buf);
}

size_t AsyncAbstractResponse::_fillBufferAndProcessTemplates(uint8_t* data, size_t len)
{
  if(!_callback)
    return _readDataFromCacheOrContent(data, len);

  const size_t originalLen = len;
  len = _readDataFromCacheOrContent(data, len);
  // Now we've read 'len' bytes, either from cache or from file
  // Search for template placeholders
  uint8_t* pTemplateStart = data;
  while((pTemplateStart < &data[len]) && (pTemplateStart = (uint8_t*)memchr(pTemplateStart, TEMPLATE_PLACEHOLDER, &data[len - 1] - pTemplateStart + 1))) { // data[0] ... data[len - 1]
    uint8_t* pTemplateEnd = (pTemplateStart < &data[len - 1]) ? (uint8_t*)memchr(pTemplateStart + 1, TEMPLATE_PLACEHOLDER, &data[len - 1] - pTemplateStart) : nullptr;
    // temporary buffer to hold parameter name
    uint8_t buf[TEMPLATE_PARAM_NAME_LENGTH + 1];
    String paramName;
    // If closing placeholder is found:
    if(pTemplateEnd) {
      // prepare argument to callback
      const size_t paramNameLength = std::min(sizeof(buf) - 1, (unsigned int)(pTemplateEnd - pTemplateStart - 1));
      if(paramNameLength) {
        memcpy(buf, pTemplateStart + 1, paramNameLength);
        buf[paramNameLength] = 0;
        paramName = String(reinterpret_cast<char*>(buf));
      } else { // double percent sign encountered, this is single percent sign escaped.
        // remove the 2nd percent sign
        memmove(pTemplateEnd, pTemplateEnd + 1, &data[len] - pTemplateEnd - 1);
        len += _readDataFromCacheOrContent(&data[len - 1], 1) - 1;
        ++pTemplateStart;
      }
    } else if(&data[len - 1] - pTemplateStart + 1 < TEMPLATE_PARAM_NAME_LENGTH + 2) { // closing placeholder not found, check if it's in the remaining file data
      memcpy(buf, pTemplateStart + 1, &data[len - 1] - pTemplateStart);
      const size_t readFromCacheOrContent = _readDataFromCacheOrContent(buf + (&data[len - 1] - pTemplateStart), TEMPLATE_PARAM_NAME_LENGTH + 2 - (&data[len - 1] - pTemplateStart + 1));
      if(readFromCacheOrContent) {
        pTemplateEnd = (uint8_t*)memchr(buf + (&data[len - 1] - pTemplateStart), TEMPLATE_PLACEHOLDER, readFromCacheOrContent);
        if(pTemplateEnd) {
          // prepare argument to callback
          *pTemplateEnd = 0;
          paramName = String(reinterpret_cast<char*>(buf));
          // Copy remaining read-ahead data into cache
          push_front(_cache, pTemplateEnd + 1, buf + (&data[len - 1] - pTemplateStart) + readFromCacheOrContent);
          pTemplateEnd = &data[len - 1];
        }
        else // closing placeholder not found in file data, store found percent symbol as is and advance to the next position
        {
          // but first, store read file data in cache
          push_front(_cache, buf + (&data[len - 1] - pTemplateStart), buf + (&data[len - 1] - pTemplateStart) + readFromCacheOrContent);
          ++pTemplateStart;
        }
      }
      else // closing placeholder not found in content data, store found percent symbol as is and advance to the next position
        ++pTemplateStart;
    }
    else // closing placeholder not found in content data, store found percent symbol as is and advance to the next position
      ++pTemplateStart;
    if(paramName.length()) {
      // call callback and replace with result.
      // Everything in range [pTemplateStart, pTemplateEnd] can be safely replaced with parameter value.
      // Data after pTemplateEnd may need to be moved.
      // The first byte of data after placeholder is located at pTemplateEnd + 1.
      // It should be located at pTemplateStart + numBytesCopied (to begin right after inserted parameter value).
      const String paramValue(_callback(paramName));
      const char* pvstr = paramValue.c_str();
      const unsigned int pvlen = paramValue.length();
      const size_t numBytesCopied = std::min(pvlen, static_cast<unsigned int>(&data[originalLen - 1] - pTemplateStart + 1));
      // make room for param value
      // 1. move extra data to cache if parameter value is longer than placeholder AND if there is no room to store
      if((pTemplateEnd + 1 < pTemplateStart + numBytesCopied) && (originalLen - (pTemplateStart + numBytesCopied - pTemplateEnd - 1) < len)) {
        push_front(_cache, &data[originalLen - (pTemplateStart + numBytesCopied - pTemplateEnd - 1)], &data[len]);
        //2. parameter value is longer than placeholder text, push the data after placeholder which not saved into cache further to the end
        memmove(pTemplateStart + numBytesCopied, pTemplateEnd + 1, &data[originalLen] - pTemplateStart - numBytesCopied);
        len = originalLen; // fix issue with truncated data, not sure if it has any side effects
      } else if(pTemplateEnd + 1 != pTemplateStart + numBytesCopied)
        //2. Either parameter value is shorter than placeholder text OR there is enough free space in buffer to fit.
        //   Move the entire data after the placeholder
        memmove(pTemplateStart + numBytesCopied, pTemplateEnd + 1, &data[len] - pTemplateEnd - 1);
      // 3. replace placeholder with actual value
      memcpy(pTemplateStart, pvstr, numBytesCopied);
      // If result is longer than buffer, copy the remainder into cache (this could happen only if placeholder text itself did not fit entirely in buffer)
      if(numBytesCopied < pvlen) {
        push_front(_cache, (uint8_t*) pvstr + numBytesCopied,  (uint8_t*) pvstr + pvlen);
      } else if(pTemplateStart + numBytesCopied < pTemplateEnd + 1) { // result is copied fully; if result is shorter than placeholder text...
        // there is some free room, fill it from cache
        const size_t roomFreed = pTemplateEnd + 1 - pTemplateStart - numBytesCopied;
        const size_t totalFreeRoom = originalLen - len + roomFreed;
        len += _readDataFromCacheOrContent(&data[len - roomFreed], totalFreeRoom) - roomFreed;
      } else { // result is copied fully; it is longer than placeholder text
        const size_t roomTaken = pTemplateStart + numBytesCopied - pTemplateEnd - 1;
        len = std::min(len + roomTaken, originalLen);
      }
    }
  } // while(pTemplateStart)
  return len;
}


/*
 * File Response
 * */

AsyncFileResponse::~AsyncFileResponse(){
  if(_content)
    _content.close();
}

void AsyncFileResponse::_setContentType(const String& path){
  _contentType = contentTypeFor(path);
}

static File fs_open_zipped(FS& fs, const String& path, bool force_absolute) {
  if (!force_absolute && !fs.exists(path)) {
    auto gz_path = path + "." + FPSTR(GZIP_EXTENSION);
    if (fs.exists(gz_path)) return fs.open(gz_path, "r");
  }
  return fs.open(path, "r");
};

AsyncFileResponse::AsyncFileResponse(FS &fs, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback) 
  : AsyncFileResponse(fs_open_zipped(fs, path, download), path, contentType, download, callback) {};

AsyncFileResponse::AsyncFileResponse(File content, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback): AsyncAbstractResponse(callback){
  _code = 200;
  _path = path;

  if(!download && String(content.name()).endsWith(FPSTR(GZIP_EXTENSION)) && !path.endsWith(FPSTR(GZIP_EXTENSION))){
    addHeader(F("Content-Encoding"), F("gzip"));
    _callback = nullptr; // Unable to process gzipped templates
    _sendContentLength = true;
    _chunked = false;
  }

  _content = content;
  _contentLength = _content.size();

  if(contentType.length() == 0)
    _setContentType(path);
  else
    _contentType = contentType;

  int filenameStart = path.lastIndexOf('/') + 1;
  char buf[26+path.length()-filenameStart];
  char* filename = (char*)path.c_str() + filenameStart;

  if(download) {
    snprintf_P(buf, sizeof (buf), PSTR("attachment; filename=\"%s\""), filename);
  } else {
    snprintf_P(buf, sizeof (buf), PSTR("inline"));
  }
  addHeader(F("Content-Disposition"), buf);
}

size_t AsyncFileResponse::_fillBuffer(uint8_t *data, size_t len){
  return _content.read(data, len);
}

/*
 * Stream Response
 * */

AsyncStreamResponse::AsyncStreamResponse(Stream &stream, const String& contentType, size_t len, AwsTemplateProcessor callback): AsyncAbstractResponse(callback) {
  _code = 200;
  _content = &stream;
  _contentLength = len;
  _contentType = contentType;
}

size_t AsyncStreamResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t available = _content->available();
  size_t outLen = (available > len)?len:available;
  size_t i;
  for(i=0;i<outLen;i++)
    data[i] = _content->read();
  return outLen;
}

/*
 * Callback Response
 * */

AsyncCallbackResponse::AsyncCallbackResponse(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback): AsyncAbstractResponse(templateCallback) {
  _code = 200;
  _content = callback;
  _contentLength = len;
  if(!len)
    _sendContentLength = false;
  _contentType = contentType;
  _filledLength = 0;
}

size_t AsyncCallbackResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t ret = _content(data, len, _filledLength);
  if(ret != RESPONSE_TRY_AGAIN){
      _filledLength += ret;
  }
  return ret;
}

/*
 * Chunked Response
 * */

AsyncChunkedResponse::AsyncChunkedResponse(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor processorCallback): AsyncAbstractResponse(processorCallback) {
  _code = 200;
  _content = callback;
  _contentLength = 0;
  _contentType = contentType;
  _sendContentLength = false;
  _chunked = true;
  _filledLength = 0;
}

size_t AsyncChunkedResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t ret = _content(data, len, _filledLength);
  if(ret != RESPONSE_TRY_AGAIN){
      _filledLength += ret;
  }
  return ret;
}

/*
 * Progmem Response
 * */

AsyncProgmemResponse::AsyncProgmemResponse(int code, const String& contentType, const uint8_t * content, size_t len, AwsTemplateProcessor callback): AsyncAbstractResponse(callback) {
  _code = code;
  _content = content;
  _contentType = contentType;
  _contentLength = len;
  _readLength = 0;
}

size_t AsyncProgmemResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t left = _contentLength - _readLength;
  if (left > len) {
    memcpy_P(data, _content + _readLength, len);
    _readLength += len;
    return len;
  }
  memcpy_P(data, _content + _readLength, left);
  _readLength += left;
  return left;
}


/*
 * Response Stream (You can print/write/printf to it, up to the contentLen bytes)
 * */

AsyncResponseStream::AsyncResponseStream(const String& contentType, size_t bufferSize){
  _code = 200;
  _contentLength = 0;
  _contentType = contentType;
  _content = new cbuf(bufferSize);
}

AsyncResponseStream::~AsyncResponseStream(){
  delete _content;
}

size_t AsyncResponseStream::_fillBuffer(uint8_t *buf, size_t maxLen){
  return _content->read((char*)buf, maxLen);
}

size_t AsyncResponseStream::write(const uint8_t *data, size_t len){
  if(_started())
    return 0;

  if(len > _content->room()){
    size_t needed = len - _content->room();
    _content->resizeAdd(needed);
  }
  size_t written = _content->write((const char*)data, len);
  _contentLength += written;
  return written;
}

size_t AsyncResponseStream::write(uint8_t data){
  return write(&data, 1);
}
