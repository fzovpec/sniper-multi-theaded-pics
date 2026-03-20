#include "metadata_table_base.h"

namespace ParametricDramDirectoryMSI
{
	class MetadataFactory
	{
	public:

		static MetadataTableBase *createMetadataTable(String name, Core *core, ShmemPerfModel *shmem_perf_model, MemoryManagementUnitBase *mmu, MemoryManagerBase *memory_manager)
		{
			if (name == "none")
			{
				return NULL;
			}
			assert(0);
			return NULL;
		}
	};
}
