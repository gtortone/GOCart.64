
#ifndef FILEREADER_H
#define FILEREADER_H

#include "ff.h"

#include "Reader.h"

class FileReader : public IDataReader {
public:
   FileReader(const char* filename) : fileOpened(false) {
      if (f_open(&file, filename, FA_READ) == FR_OK)
         fileOpened = true;
   }

   ~FileReader() {
      if (fileOpened) 
         f_close(&file);
   }

   bool read(uint8_t* dst, size_t n) override {
      UINT br;
      if (!fileOpened) 
         return false;
      return f_read(&file, dst, n, &br) == FR_OK && br == n;
   }

   bool seek(size_t pos) override {
      return fileOpened && f_lseek(&file, pos) == FR_OK;
   }

   size_t tell() const override {
      return fileOpened ? f_tell(const_cast<FIL*>(&file)) : 0;
   }

   bool eof() const override {
      return !fileOpened || f_eof(&file);
   }

   size_t size() const override {
      return fileOpened ? f_size(const_cast<FIL*>(&file)) : 0;
   }

private:
   FIL file;
   bool fileOpened;
};

#endif
