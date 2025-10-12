#ifndef __SIFT_WARMUP
#define __SIFT_WARMUP

#include "sift_writer.h"

namespace Sift {
   class Writer;
};

namespace Sift
{
   class Warmup
   {
   public:
      Warmup(const char *filename, int id, Sift::Writer *_output = NULL);
      ~Warmup();

      void MemoryAccess(uint64_t addr, bool isread = true);
      void NewThread();
      static bool getCode(uint8_t *dst, const uint8_t *src, uint32_t size);
      static void _getCode(uint8_t *dst, const uint8_t *src, uint32_t size) {getCode(dst,src,size);}
      static const int page_size = 0x1000;
      static const uint64_t startaddr = 0x5349465400000000;

   private:
      Writer *output;
      bool m_output_owner;

      static void initCode(uint8_t *dst, uint32_t size);
      void getNextInsn(uint64_t &insnaddr,uint32_t &size, bool &is_branch, bool &taken);

      static uint8_t codePage[page_size];
      uint64_t curraddr;

   };
}

#endif
