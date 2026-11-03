
#ifndef READER_H
#define READER_H

class IDataReader {
public:
   virtual ~IDataReader() {}
   virtual bool read(uint8_t* dst, size_t n) = 0;
   virtual bool seek(size_t pos) = 0;
   virtual size_t tell() const = 0;
   virtual bool eof() const = 0;
   // total size in bytes (0: unknown)
   virtual size_t size() const { return 0; }
};

#endif
