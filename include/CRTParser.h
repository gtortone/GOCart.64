
#ifndef CRTPARSER_H
#define CRTPARSER_H

#include <vector>
#include <cstring>
#include <cstdio>

#define NEXT_CHIP    2

struct ChipBlock {
   uint8_t type;              // 0:ROM, 1:RAM, 2:FLASH
   uint8_t number;
   uint16_t startAddress;
   uint16_t length;
};

/*
 * class CRTParser
 * constructor accepts IDataReader (file or buffer), buffer for banks data, size of buffer
 */
class CRTParser {

public:
   CRTParser(IDataReader& reader, uint8_t *crt_buf, uint32_t crt_buf_size, uint8_t *crt_map, uint8_t crt_map_size) : 
      reader(reader), crt_buf(crt_buf), crt_buf_size(crt_buf_size), crt_map(crt_map), crt_map_size(crt_map_size) {}

   char* getName(void) {
      return name;
   }

   uint16_t getType(void) {
      return type;
   }

   bool getExrom(void) {
      return exrom;
   }

   bool getGame(void) {
      return game;
   }

   uint32_t getSize(void) {
      return size;
   }

   bool parse(void) {
      if (!parseHeader()) 
         return false;

      while (true) {

         // parse chip blocks
         uint8_t res = parseChipBlock();

         if (res == NEXT_CHIP)
            continue;
         else if (res == true)
            break;
         else return false;
      }
      return true;
   }

   void print(void) {

      printf("name: %s\n", name);
      printf("hardware type: %d\n", type);
      printf("exrom: %d, game: %d\n", exrom, game);

      for(uint8_t i=0; i<chips.size(); i++) {
         printf("bank: %d\n", chips[i].number);
         printf("\ttype: %d\n", chips[i].type);
         printf("\tstart address: 0x%X\n", chips[i].startAddress);
         printf("\tlength: 0x%X\n", chips[i].length);
      }
   }

private:
   IDataReader& reader;

   uint16_t type;
   bool exrom, game;
   char name[33];
   uint32_t size = 0;

   std::vector<ChipBlock> chips;

   uint8_t *crt_buf;
   uint32_t crt_buf_size;
   uint8_t *crt_map;
   uint8_t crt_map_size;

   // temp data
   uint8_t prev_number = 0;
   uint8_t ram_bank_number= 0;
 
   bool parseHeader(void) {

      uint8_t buf[50];

      if (!reader.read(buf, 16))
         return false;
      if (memcmp(buf, "C64 CARTRIDGE   ", 16) != 0)
         return false;

      // skip file header length and cartridge version
      if(!reader.read(buf, 6))
         return false;

      if(!reader.read(buf, 2))
         return false;
      type = (buf[0] << 8) | buf[1];

      if(!reader.read(buf, 2))
         return false;
      exrom = bool(buf[0]);
      game = bool(buf[1]);

      // fix Ocean bad EXROM/GAME
      if(type == 5) {
         exrom = game = 0;
      } else if(type == 32) {   // fix EasyFlash bad EXROM/GAME
         exrom = 1;
         game =0;
      }
      
      // skip reserved bytes
      if(!reader.read(buf, 6))
         return false;

      if(!reader.read(buf, 32))
         return false;
      memcpy(name, buf, 32);
      name[32] = '\0';

      return true;
   }

   uint8_t parseChipBlock(void) {

      if (reader.eof())
         return true;

      uint8_t buf[50];

      if (!reader.read(buf, 4)) 
         return false;
      if (memcmp(buf, "CHIP", 4) != 0)
         return false;

      // skip total packet length
      if (!reader.read(buf, 4))
         return false;

      if (!reader.read(buf, 8)) 
         return false;

      uint16_t chipType = (buf[0] << 8) | buf[1];
      uint8_t number = (buf[2] << 8) | buf[3];
      uint16_t startAddr = (buf[4] << 8) | buf[5];
      uint16_t blockLen = (buf[6] << 8) | buf[7];

      // EasyFlash bank register is 6 bits wide (crt_map size)
      if (number >= crt_map_size && type == 32) {
         printf("E: CRT bank %d out of range\n", number);
         return false;
      }

      // map bank (crt_map is only used by EasyFlash)
      if (number < crt_map_size) {
         if (number == prev_number) {
            crt_map[number] = ram_bank_number;
         } else if (number > ram_bank_number + 1) {
            crt_map[number] = ram_bank_number + 1;
            ram_bank_number++;
         } else if (number == ram_bank_number + 1) {
            crt_map[number] = ram_bank_number + 1;
            ram_bank_number = number;
         }
      }

      prev_number = number;

      // calculate offset inside memory buffer
      uint32_t ofs = 0;
      uint8_t bank = (number < crt_map_size) ? crt_map[number] : 0;

      if(startAddr == 0x8000 && blockLen <= 16*1024) {

         // Support ROML only cartridges with more than 64 banks
         if(blockLen<= 8*1024 && (type == 7) ) {

            // Fun Play
            ofs = (( (number >> 3) & 0x07 ) | ( (number & 0x01) << 3)) * 16*1024;

         } else if(blockLen <= 8*1024 && (type == 19) ) {

            // Magic Desk
            bool odd_bank = number & 1;
            number >>= 1;
            ofs = number * 16*1024;

            // Use ROMH bank location for odd banks
            if(odd_bank)
               ofs += 8*1024;
      
         } else if(blockLen <= 8*1024 && (type == 32) ) {

            // EasyFlash
            ofs = bank * 16*1024;

         } else {

            ofs = number * 16*1024;
         }

      // ROMH bank
      } else if((startAddr == 0xa000 || startAddr == 0xe000) && blockLen <= 8*1024 ) {

         if (type == 5) {
            // Ocean
            ofs = (number % 16) * 16*1024 + 8*1024;
         } else if(type == 32) {
            // EasyFlash
            ofs = bank * 16*1024 + 8*1024;
         } else {
            ofs = number * 16*1024 + 8*1024;
         }

      } else if(startAddr == 0xf000 && blockLen <= 4*1024 ) {

         if(type == 32) {
            // EasyFlash
            ofs = bank * 16*1024 + 8*1024;
         } else {
            ofs = number * 16*1024 + 8*1024;
         }
      }

      //printf("bank %d, load_addr: 0x%X, ofs: %lu, crt_map[%d]: %d\n",
      //      number, startAddr, ofs, number, crt_map[number]);
      
      // 4K images are mirrored to 8K
      uint32_t span = (blockLen <= 4*1024) ? 8*1024 : blockLen;
      if (ofs + span > crt_buf_size) {
         printf("E: CRT too large for buffer (bank %d, offset %lu)\n", number, ofs);
         return false;
      }

      if (!reader.read(crt_buf+ofs, blockLen)) 
         return false;

      // mirror 4K image
      if (blockLen <= 4*1024)
         memcpy(crt_buf+ofs+(4*1024), crt_buf+ofs, 4*1024);

      ChipBlock chip;
      chip.type = chipType;
      chip.number = number;
      chip.startAddress = startAddr;
      chip.length = blockLen;
      
      chips.push_back(chip);
      size += chip.length;

      return NEXT_CHIP;
   }
};

#endif
