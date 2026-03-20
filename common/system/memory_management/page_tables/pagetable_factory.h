#pragma once
#include "pagetable.h"
#include "pagetable_radix.h"
#include "config.hpp"
#include "mimicos.h"

namespace ParametricDramDirectoryMSI
{
	class PageTableFactory
	{
	public:
		static PageTable *createRadixPageTable(int app_id, String name, String type, int page_sizes, int *page_size_list, int levels, int frame_size, bool is_guest)
		{
			std::cout << "[Radix Page Table] Creating 4-level Radix table with name: " << name << "for app id: " << app_id << std::endl;
			std::cout << "[Radix Page Table] Page sizes: " << page_sizes << std::endl;
			for (int i = 0; i < page_sizes; i++)
			{
				std::cout << "[Page Table] Page size: " << page_size_list[i] << std::endl;
			}
			std::cout << "[Radix Page Table] Levels: " << levels << std::endl;
			std::cout << "[Radix Page Table] Frame size: " << frame_size << " entries" << std::endl;

			auto* pt = new PageTableRadix(app_id, name, type, page_sizes, page_size_list, levels, frame_size, is_guest);

			std::cout << "[Radix Page Table] createRadixPageTable created new PageTableRadix object with name: " << pt->getName() << " and type: " << pt->getType() << std::endl;
			return pt;
		}

		static PageTable *createPageTable(String type, String name, UInt64 app_id, bool is_guest = false)
		{
			if (type == "radix")
			{
				String mimicos_name;
				if (is_guest == true)
					mimicos_name = Sim()->getMimicOS_VM()->getName();
				else
					mimicos_name = Sim()->getMimicOS()->getName();

				int page_sizes = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/page_sizes");
				int *page_size_list = new int[page_sizes];

				for (int i = 0; i < page_sizes; i++)
				{
					page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mimicos_name + "/" + name + "/page_size_list", i);
				}

				int levels = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/levels");
				int frame_size = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/frame_size");

				return createRadixPageTable(app_id, name, type, page_sizes, page_size_list, levels, frame_size, is_guest);
			}
			else
				assert(0 && "Invalid page table type (only 'radix' supported in this build)");
		}
	};
}
