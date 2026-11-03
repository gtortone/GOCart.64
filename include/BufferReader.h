
#ifndef BUFFERREADER_H
#define BUFFERREADER_H

#include <cstring>

class BufferReader : public IDataReader {
public:
   BufferReader(const uint8_t* buf, size_t len) : buffer(buf), length(len), pos(0) {}

   bool read(uint8_t* dst, size_t n) override {
      if (pos + n > length) 
         return false;
      memcpy(dst, buffer + pos, n);
      pos += n;
      return true;
   }

   bool seek(size_t p) override {
      if (p > length)
         return false;
      pos = p;
      return true;
   }

   size_t tell() const override { 
      return pos; 
   }

   bool eof() const override { 
      return pos >= length; 
   }

   size_t size() const override {
      return length;
   }

private:
   const uint8_t* buffer;
   size_t length;
   size_t pos;
};

#endif
