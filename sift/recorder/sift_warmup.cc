#include "sift_warmup.h"
#include "sim_api.h"

#include <cstring>

uint8_t Sift::Warmup::codePage[Sift::Warmup::page_size] __attribute__((aligned(0x1000)));

// 0x8b 0x07 -> Load    mov (%rdi),%eax
// 0x89 0x17 -> Store   mov %edx,(%rdi)
// 0x75 0x4c -> Branch  jne
void Sift::Warmup::initCode(uint8_t *dst, uint32_t size)
{
   uint32_t i = 0;
   // Create a stream of read instructions
   while (i < size-2)
   {
      dst[i+0] = 0x8b;
      dst[i+1] = 0x07;
      i+=2;
   }
   // Create a single branch instruction to loop back to the beginning
   dst[i+0] = 0x75;
   dst[i+1] = 0x4c;
}

bool Sift::Warmup::getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   uint64_t codePage_u = reinterpret_cast<uint64_t>(codePage);
   uint64_t src_u = reinterpret_cast<uint64_t>(src);
   uint64_t startaddr_u = startaddr;
   uint64_t code_start_u = codePage_u + (src_u - startaddr_u);
   if ((code_start_u < codePage_u) || (code_start_u > (codePage_u+page_size)) || ((code_start_u+size) < codePage_u) || ((code_start_u+size) > (codePage_u+page_size)))
   {
      return false;
   }
   memcpy(dst, reinterpret_cast<void*>(code_start_u), size);
   return true;
}

Sift::Warmup::Warmup(const char *filename, int id, Sift::Writer *_output)
{
   initCode(codePage, sizeof(codePage));
   curraddr = startaddr;

   if (_output)
   {
      output = _output;
      m_output_owner = false;
   }
   else
   {
      //Writer(const char *filename, GetCodeFunc getCodeFunc, bool useCompression = false, const char *response_filename = "", uint32_t id = 0, bool arch32 = false, bool requires_icache_per_insn = false, bool send_va2pa_mapping = false, GetCodeFunc2 getCodeFunc2 = NULL, void *GetCodeFunc2Data = NULL);
      output = new Sift::Writer(filename, Sift::Warmup::_getCode, false /*compression*/, "", id, false /*arch32*/, false /*icache-per-insn*/, false /*physical addresses?*/);
      m_output_owner = true;
   }
}

Sift::Warmup::~Warmup()
{
   if (m_output_owner && output)
      delete output;
}

void Sift::Warmup::getNextInsn(uint64_t &insnaddr,uint32_t &size, bool &is_branch, bool &taken)
{
   size = 2;
   insnaddr = curraddr;
   // Is it time for a branch instruction? (Have we reached the end of the page?)
   if (insnaddr >= (startaddr+page_size-size))
   {
      is_branch = true;
      taken = true;
      curraddr = startaddr;
   }
   else
   {
      is_branch = false;
      taken = false;
      curraddr += size;
   }
   return;
}

void Sift::Warmup::MemoryAccess(uint64_t dataaddr, bool isread)
{
   bool is_branch = true, taken;
   uint32_t size;
   uint64_t insnaddr;
   uint64_t addresses[Sift::MAX_DYNAMIC_ADDRESSES] = { 0 };

   while(is_branch)
   {
      getNextInsn(insnaddr,size,is_branch,taken);
      uint8_t num_addr;
      if (is_branch)
      {
         num_addr = 0;
         addresses[0] = 0;
      }
      else
      {
         num_addr = 1;
         addresses[0] = dataaddr;
      }
      output->Instruction(insnaddr, size, num_addr, addresses, is_branch, taken, false /*is_predicate*/, true /*executing*/);
   }
}

void Sift::Warmup::NewThread()
{
    output->NewThread();
}
