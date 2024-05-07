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
#include "WebHandlerImpl.h"

#ifdef ASYNCWEBSERVER_DEBUG_TRACE
#define DEBUG_PRINTFP(fmt, ...) Serial.printf_P(PSTR("[%d]" fmt), (unsigned) millis(), ##__VA_ARGS__)
#else
#define DEBUG_PRINTFP(...)
#endif

#ifdef ASYNCWEBSERVER_NEEDS_MUTEX
#ifdef DEBUG_GUARDS
struct guard_type {
  std::unique_lock<std::mutex> _lock;
  size_t _line;
  guard_type(std::mutex& m, size_t line) : _line(line) {
    DEBUG_PRINTFP("acquire: %d\n", _line);
    _lock = decltype(_lock) { m };  // defer construction
  }
  ~guard_type() {    
    _lock.unlock();
    DEBUG_PRINTFP("release: %d\n", _line);
  }
};
#define guard() const guard_type guard(_mutex, __LINE__)
#else
#define guard() const std::lock_guard<std::mutex> guard(_mutex)
#endif
#else
#define guard()
#endif

#ifndef ASYNCWEBSERVER_MINIMUM_ALLOC
#define ASYNCWEBSERVER_MINIMUM_ALLOC 1024
#endif

#ifndef ASYNCWEBSERVER_MINIMUM_HEAP
#define ASYNCWEBSERVER_MINIMUM_HEAP 2048
#endif


bool ON_STA_FILTER(AsyncWebServerRequest *request) {
  return WiFi.localIP() == request->client()->localIP();
}

bool ON_AP_FILTER(AsyncWebServerRequest *request) {
  return WiFi.localIP() != request->client()->localIP();
}


static bool minimal_send_503(AsyncClient* c) {
    const static char msg[] PROGMEM = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
#ifdef PROGMEM  
    char msg_stack[sizeof(msg)];  // stack, so we can pull it out of flash memory
    memcpy_P(msg_stack, msg, sizeof(msg));
    #define MSG_503 msg_stack
#else
    #define MSG_503 msg
#endif
    auto w = c->write(MSG_503, sizeof(MSG_503)-1, ASYNC_WRITE_FLAG_COPY);

    // assume any nonzero value is success
    DEBUG_PRINTFP("*** Sent 503 to %d (%d), result %d\n", (intptr_t) c, c->getRemotePort(), w);
    if (w == 0) {    
      c->close(true); // sorry bud, we're really that strapped for ram  
    }
    return (w != 0);  
}

#ifdef ESP8266
#define GET_MAX_BLOCK_SIZE getMaxFreeBlockSize
#else
#define GET_MAX_BLOCK_SIZE getMaxAllocHeap
#endif

static bool heap_ok(size_t minHeap) {
  return (ESP.getFreeHeap() > minHeap)
    && (ESP.GET_MAX_BLOCK_SIZE() > ASYNCWEBSERVER_MINIMUM_ALLOC);
}

AsyncWebServer::AsyncWebServer(uint16_t port)
  : AsyncWebServer(IPADDR_ANY, port)
{
}

AsyncWebServer::AsyncWebServer(uint16_t port, const AsyncWebServerQueueLimits& limits)
  : AsyncWebServer(IPADDR_ANY, port, limits)
{
}


AsyncWebServer::AsyncWebServer(IPAddress addr, uint16_t port)
  : AsyncWebServer(addr, port, {0, 0, 0, 0})
{
}

AsyncWebServer::AsyncWebServer(IPAddress addr, uint16_t port, const AsyncWebServerQueueLimits& limits)
  : _queueLimits(limits)
  , _server(addr, port)
  , _rewrites([](AsyncWebRewrite* r){ delete r; })
  , _handlers([](AsyncWebHandler* h){ delete h; })
  , _requestQueue(LinkedList<AsyncWebServerRequest*>::OnRemove {})
  , _queueActive(false)
{
  _catchAllHandler = new AsyncCallbackWebHandler();
  if(_catchAllHandler == NULL)
    return;
  _server.onClient([this](void *s, AsyncClient* c){
    if(c == NULL)
      return;

    if (!heap_ok(ASYNCWEBSERVER_MINIMUM_HEAP)) {
      // Protect ourselves from crashing - just abandon this request.
      DEBUG_PRINTFP("*** Dropping client %d (%d): %d, %d\n", (intptr_t) c, c->getRemotePort(), _requestQueue.length(), ESP.getFreeHeap());
      c->close(true);
      delete c;
      return;
    }

    guard();

    if (((_queueLimits.queueHeapRequired > 0) && !heap_ok(_queueLimits.queueHeapRequired))
       || ((_queueLimits.nMax > 0) && (_requestQueue.length() >= _queueLimits.nMax))
    ) {
      // Don't even allocate anything we can avoid.  Tell the client we're in trouble with a static response.
      DEBUG_PRINTFP("*** Rejecting client %d (%d): %d, %d\n", (intptr_t) c, c->getRemotePort(), _requestQueue.length(), ESP.getFreeHeap());
      c->setNoDelay(true);
      c->onDisconnect([](void*r, AsyncClient* rc){
        DEBUG_PRINTFP("*** Client %d (%d) disconnected\n", (intptr_t)rc, rc->getRemotePort());
        delete rc;  // There is almost certainly something wrong with this - it's not OK to delete a function object while it's running
      });
      c->onAck([](void *, AsyncClient* rc, size_t s, uint32_t ){  
        rc->close(true);        
      });
      minimal_send_503(c);
      return;
    }

    c->setRxTimeout(3);

    AsyncWebServerRequest *r = new AsyncWebServerRequest((AsyncWebServer*)s, c);
    if(r == NULL){
      c->close(true);
      c->free();
      delete c;
      return;
    }
    
    
    _requestQueue.add(r);
  }, this);
}

AsyncWebServer::~AsyncWebServer(){
  reset();  
  end();
  if(_catchAllHandler) delete _catchAllHandler;
}

AsyncWebRewrite& AsyncWebServer::addRewrite(AsyncWebRewrite* rewrite){
  _rewrites.add(rewrite);
  return *rewrite;
}

bool AsyncWebServer::removeRewrite(AsyncWebRewrite *rewrite){
  return _rewrites.remove(rewrite);
}

AsyncWebRewrite& AsyncWebServer::rewrite(const char* from, const char* to){
  return addRewrite(new AsyncWebRewrite(from, to));
}

AsyncWebHandler& AsyncWebServer::addHandler(AsyncWebHandler* handler){
  _handlers.add(handler);
  return *handler;
}

bool AsyncWebServer::removeHandler(AsyncWebHandler *handler){
  return _handlers.remove(handler);
}

void AsyncWebServer::begin(){
  _server.setNoDelay(true);
  _server.begin();
}

void AsyncWebServer::end(){
  _server.end();
}

#if ASYNC_TCP_SSL_ENABLED
void AsyncWebServer::onSslFileRequest(AcSSlFileHandler cb, void* arg){
  _server.onSslFileRequest(cb, arg);
}

void AsyncWebServer::beginSecure(const char *cert, const char *key, const char *password){
  _server.beginSecure(cert, key, password);
}
#endif

void AsyncWebServer::_handleDisconnect(AsyncWebServerRequest *request){
  delete request;
}

void AsyncWebServer::_rewriteRequest(AsyncWebServerRequest *request){
  for(const auto& r: _rewrites){
    if (r->match(request)){
      request->_url = r->toUrl();
      request->_addGetParams(r->params());
    }
  }
}

void AsyncWebServer::_attachHandler(AsyncWebServerRequest *request){
  for(const auto& h: _handlers){
    if (h->filter(request) && h->canHandle(request)){
      request->setHandler(h);
      return;
    }
  }
  
  request->addInterestingHeader("ANY");
  request->setHandler(_catchAllHandler);
}


AsyncCallbackWebHandler& AsyncWebServer::on(String uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(std::move(uri));
  handler->setMethod(method);
  handler->onRequest(onRequest);
  handler->onUpload(onUpload);
  handler->onBody(onBody);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(String uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(std::move(uri));
  handler->setMethod(method);
  handler->onRequest(onRequest);
  handler->onUpload(onUpload);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(String uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(std::move(uri));
  handler->setMethod(method);
  handler->onRequest(onRequest);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(String uri, ArRequestHandlerFunction onRequest){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(std::move(uri));
  handler->onRequest(onRequest);
  addHandler(handler);
  return *handler;
}

AsyncStaticWebHandler& AsyncWebServer::serveStatic(String uri, fs::FS& fs, String path, const char* cache_control){
  AsyncStaticWebHandler* handler = new AsyncStaticWebHandler(std::move(uri), fs, std::move(path), cache_control);
  addHandler(handler);
  return *handler;
}

void AsyncWebServer::onNotFound(ArRequestHandlerFunction fn){
  _catchAllHandler->onRequest(fn);
}

void AsyncWebServer::onFileUpload(ArUploadHandlerFunction fn){
  _catchAllHandler->onUpload(fn);
}

void AsyncWebServer::onRequestBody(ArBodyHandlerFunction fn){
  _catchAllHandler->onBody(fn);
}

void AsyncWebServer::reset(){
  _rewrites.free();
  _handlers.free();
  
  if (_catchAllHandler != NULL){
    _catchAllHandler->onRequest(NULL);
    _catchAllHandler->onUpload(NULL);
    _catchAllHandler->onBody(NULL);
  }
}

size_t AsyncWebServer::numClients(){
  guard();
  return _requestQueue.length();
}

size_t AsyncWebServer::queueLength(){
  guard();
  size_t count = 0U;
  for(const auto& client: _requestQueue) { if (client->_parseState >= 200) ++count; };
  return count;
}

void AsyncWebServer::processQueue(){  
  // Consider the state of the requests in the queue.
  // Requests in STATE_END have already been handled; we can assume any heap they need has already been allocated.
  // Requests in STATE_QUEUED are pending.  Each iteration we consider the first one.
  // We always allow one request, regardless of heap state.
#ifdef ASYNCWEBSERVER_DEBUG_TRACE
  size_t count = 0, active = 0, queued = 0;
#endif

  {
    guard();
    if (_queueActive) return; // already in progress
    _queueActive = true;

#ifdef ASYNCWEBSERVER_DEBUG_TRACE
    for(auto element: _requestQueue) {
      ++count;
      if (element->_parseState == 100) ++active;
      if (element->_parseState == 200) ++queued;
    }
#endif    
  }

  DEBUG_PRINTFP("Queue: %d entries, %d running, %d queued\n", count, active, queued);

  do { 
    auto heap_ok = ESP.getFreeHeap() >= (_queueLimits.requestHeapRequired + _queueLimits.queueHeapRequired);
    size_t active_entries = 0;
    AsyncWebServerRequest* next_queued_request = nullptr;

    {
      // Get a queued entry while holding the lock
      guard();
      for(auto entry: _requestQueue) {
        if (entry->_parseState == 100) {
          ++active_entries;
        } else if ((entry->_parseState == 200) && !next_queued_request) {
          next_queued_request = entry;
        };
      }
    }

    if (!next_queued_request) break;  // all done
    if ((_queueLimits.nParallel > 0) && (active_entries >= _queueLimits.nParallel)) break; // lots running
    if ((active_entries > 0) && (!heap_ok)) break;  // heap not ok    
    next_queued_request->_handleRequest();
  } while(1); // as long as we have memory and queued requests

  {
    guard();
    for(auto entry: _requestQueue) {
      // Un-defer requests
      if (entry->_parseState == 201) entry->_parseState = 200;
    }
    _queueActive = false;
  }
}

void AsyncWebServer::_dequeue(AsyncWebServerRequest *request){
  {
    DEBUG_PRINTFP("Removing %d from queue\n", (intptr_t) request);
    guard();    
    _requestQueue.remove(request);
  }
  processQueue();
}

void AsyncWebServer::setQueueLimits(const AsyncWebServerQueueLimits& limits) {
  guard();
  _queueLimits = limits;
}

static char* append_vprintf_P(char* buf, char* end, const char* /*PROGMEM*/ fmt, ...){
  va_list argp;
  va_start(argp, fmt);
  const auto max = end-buf;
  const auto needed = vsnprintf_P(buf, max, fmt, argp);
  va_end(argp);
  return (needed >= max) ? end-1 : buf + needed;
}

void AsyncWebServer::printStatus(Print& dest){
  char buf[1024];
  char* buf_p = buf;
  char* end = &buf[sizeof(buf)];
  buf[0] = 0;
  {
    guard();  
    for(const auto& entry: _requestQueue) {
      buf_p = append_vprintf_P(buf_p, end, PSTR("\n- Request %X [%X], state %d"), (intptr_t) entry, (intptr_t) entry->_client, entry->_parseState);
      if (entry->_response) {
        auto r = entry->_response;
        buf_p = append_vprintf_P(buf_p, end, PSTR(" -- Response %X, state %d, [%d %d - %d %d %d]"), (intptr_t) r, r->_state, r->_headLength, r->_contentLength, r->_sentLength, r->_ackedLength, r->_writtenLength);
      }
    }
  }
  buf[sizeof(buf)-1] = 0; // just in case
  dest.print(F("Web server status:"));
  if (buf[0]) {
    dest.println(buf);
  } else {
    dest.println(F(" Idle"));
  }
}
