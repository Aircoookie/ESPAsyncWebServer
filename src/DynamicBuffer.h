// DynamicBuffer and DynamicBufferList
// Simple class for holding a dynamically allocated buffer

#pragma once

#include "Arduino.h"
#include <memory>
#include <list>
#include <utility>

// Forward declaration
class SharedBuffer;

// The DynamicBuffer class holds a malloc() allocated heap buffer.
// It's similar to std::vector<char>, but permits allocation failures without crashing the system.
class DynamicBuffer {
  char* _data;
  size_t _len;

  public:

  void clear() { if (_data) free(_data); _data = nullptr; _len = 0; }

  DynamicBuffer() : _data(nullptr), _len(0) {};
  explicit DynamicBuffer(size_t len) : _data(len ? reinterpret_cast<char*>(malloc(len)): nullptr), _len(_data ? len : 0) {};
  DynamicBuffer(const char* buf, size_t len) : DynamicBuffer(len) { if (_data) memcpy(_data, buf, len); };
  explicit DynamicBuffer(const String& s) : DynamicBuffer(s.begin(), s.length()) {};
  explicit DynamicBuffer(String&&);  // Move string contents in to buffer if possible
  DynamicBuffer(const SharedBuffer&);
  DynamicBuffer(SharedBuffer&&);
  ~DynamicBuffer() { clear(); };
  
  // Move
  DynamicBuffer(DynamicBuffer&& d) : _data(d._data), _len(d._len) { d._data = nullptr; d._len = 0; };
  DynamicBuffer& operator=(DynamicBuffer&& d) { std::swap(_data, d._data); std::swap(_len, d._len); return *this; };

  // Copy
  DynamicBuffer(const DynamicBuffer& d) : DynamicBuffer(d._data, d._len) {};  // copy
  DynamicBuffer& operator=(const DynamicBuffer& d) { *this = DynamicBuffer(d); return *this; }; // use move to copy

  // Accessors
  char* data() const { return _data; };
  size_t size() const { return _len; };
  char& operator[](ptrdiff_t p) const { return *(_data + p); };

  explicit operator bool() const { return (_data != nullptr) && (_len > 0); }

  // Release the buffer without freeing it
  char* release() { char* temp = _data; _data = nullptr; _len = 0; return temp; }

  // Resize the buffer.  Returns new size on success, current size on failure.
  size_t resize(size_t);
};

// Same interface as DynamicBuffer, but with shared_ptr semantics: buffer is held until last copy releases it.
class SharedBuffer {
  std::shared_ptr<DynamicBuffer> _buf;
  friend class DynamicBuffer;

  public:

  SharedBuffer() = default;
  explicit SharedBuffer(size_t len) : _buf(std::make_shared<DynamicBuffer>(len)) {};
  SharedBuffer(const char* buf, size_t len) : _buf(std::make_shared<DynamicBuffer>(buf, len)) {};
  explicit SharedBuffer(const String& s) : _buf(std::make_shared<DynamicBuffer>(s)) {};
  explicit SharedBuffer(String&& s) : _buf(std::make_shared<DynamicBuffer>(std::move(s))) {};
  SharedBuffer(const DynamicBuffer &d) : _buf(std::make_shared<DynamicBuffer>(d)) {};
  SharedBuffer(DynamicBuffer&& d) : _buf(std::make_shared<DynamicBuffer>(std::move(d))) {};

  char* data() const { return _buf ? _buf->data() : nullptr; };
  size_t size() const { return _buf ? _buf->size() : 0U; };
  char& operator[](ptrdiff_t p) const { return *(data() + p); };
  void clear() { _buf.reset(); };

  explicit operator bool() const { return _buf && *_buf; };
  DynamicBuffer copy() const { return *_buf; }; // Make a copy of the buffer
};

// Utility functions
String toString(DynamicBuffer buf);   // Move a buffer in to a string.  Buffer will be moved if buf is an rvalue, copied otherwise.


// DynamicBufferList - an RAII list of DynamicBuffers
// This structure can be used to create a chain of buffers, useful when the heap could get fragmented
typedef std::list<DynamicBuffer> DynamicBufferList;
typedef std::list<SharedBuffer> SharedBufferList;

// Factory function: creates a list of buffers of max_buffer_size up to total bytes long.
// The last buffer will fit exactly the required size.
DynamicBufferList allocateDynamicBufferList(size_t total, size_t max_buffer_size = 0);
SharedBufferList allocateSharedBufferList(size_t total, size_t max_buffer_size = 0);

// Returns the total size of a DynamicBufferList
size_t totalSize(const DynamicBufferList&);
size_t totalSize(const SharedBufferList&);

// Helper class - lets you stream something in to a DynamicBufferList, such as ArduinoJSON
// If buffer size is nonzero, allocates additional buffers of buffer_size as required.
template<typename list_type>
class BufferListPrint : public Print {
  private:
    list_type& _list;
    size_t _buffer_size;
    typename list_type::const_iterator _next;
    size_t _offset;
    bool _valid;

  public:
    BufferListPrint(list_type& list, size_t buffer_size = 0) : _list(list), _buffer_size(buffer_size), _next(_list.begin()), _offset(0), _valid(true) {};
    virtual ~BufferListPrint(){}

    size_t write(const uint8_t *buffer, size_t size) {
      if (!_valid) return 0;  // short circuit exit if we ran out of space (or memory, if allocating new buffers)
      size_t written = 0;
      while(size) {
        if (_next == _list.end()) {
          if (_buffer_size > 0) {
            _list.emplace_back(_buffer_size);
            if (!_list.back().size()) { // out of memory
              _list.pop_back();
              _valid = false;
              return written;
            }
            _next = _list.end();
            --_next;
          } else {
            _valid = false;
            return written;
          }
        }
        size_t to_write = std::min(_next->size() - _offset, size);
        memcpy(_next->data() + _offset, buffer, to_write);
        written += to_write;
        buffer += to_write;
        size -= to_write;
        _offset += to_write;
        if (_offset == _next->size()) {
          ++_next;
          _offset = 0;
        }
      }
      return written;      
    }

    size_t write(uint8_t c) {
      return this->write(&c, 1);
    }

    bool valid() const { return _valid; };
    explicit operator bool() const { return valid(); };
};

typedef BufferListPrint<DynamicBufferList> DynamicBufferListPrint;
typedef BufferListPrint<SharedBufferList> SharedBufferListPrint;


// Walkable buffer
// A buffer class that permits "consuming" data from either end, adjusting data() and size() to match
template<typename buffer_type>
class Walkable
{
  buffer_type _buf;
  size_t _left, _right;

  public:
  Walkable() : _left(0), _right(0) {};
  explicit Walkable(size_t len) : _buf(len), _left(0), _right(0) {};
  Walkable(const char* buf, size_t len) : _buf(buf, len), _left(0), _right(0) {};
  explicit Walkable(const String& s) : _buf(s), _left(0), _right(0) {};
  explicit Walkable(String&& s) : _buf(std::move(s)), _left(0), _right(0) {};

  // Accessors
  // Buffer interface
  char* data() const { return _buf.data() + _left; };
  size_t size() const { return _buf.size() - (_left + _right); };
  size_t capacity() const { return _buf.size(); };  // for similarity with STL types
  explicit operator bool() const { return (buffer_type::data() != nullptr) && (size() > 0); }
  char& operator[](ptrdiff_t p) const { return *(data() + p); };
  void clear() { _buf.clear(); reset(); };

  // Raw interface
  const buffer_type& buffer() const { return _buf; };
  size_t offset() const { return _left; }
  size_t roffset() const { return _right; }  
  
  // Modifiers
  void reset() { _left = _right = 0; }; // Reset the walking counters
  void advance(ptrdiff_t count) { // Consume some data from the left hand side
    if (count > 0) {
      _left = std::min(_left+count, _buf.size() - _right);
    } else {
      if (abs(count) <= _left) {
        _left += count;
      } else {
        _left = 0;  // do not wrap
      }
    }
  }
  void radvance(ptrdiff_t count) {  // Consume some data from the right hand side
    if (count > 0) {
      _right = std::min(_right+count, _buf.size() - _left);
    } else {
      if (abs(count) <= _right) {
        _right += count;
      } else {
        _right = 0;  // do not wrap
      }
    }
  }

  // Contract buffer to specified size
  size_t resize(size_t s) {
    auto bs = _buf.size() - _left;    
    _right = s <= bs ? (bs - s) : 0U;
    return size();
  }

  // Resize the underlying buffer storage, preserving contents
  // Returns new size on success, current size on failure.
  size_t reallocate(size_t s) {
    if (s <= size()) {
      auto new_buf = buffer_type(data(), s);
      if (new_buf) {
        _buf = std::move(new_buf);
        reset();
      }
    } else {
      auto new_buf = buffer_type(s);
      if (new_buf) {
        memcpy(new_buf.data(), data(), size());
        _buf = std::move(new_buf);
        reset();
      }
    }
    return _buf.size();
  }
};
