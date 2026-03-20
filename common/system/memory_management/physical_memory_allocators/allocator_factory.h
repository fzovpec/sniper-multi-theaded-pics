#pragma once

#include "simulator.h"
#include "config.hpp"

// NOTE!! buddy_policy.h should be included before reserve_thp.h
// common/system/memory_management/policies
#include "memory_management/policies/buddy_policy.h"
#include "memory_management/policies/reserve_thp_policy.h"
#include "memory_management/policies/baseline_policy.h"

// include/memory_management/physical_memory_allocators/
#include "../../include/memory_management/physical_memory_allocators/reserve_thp.h"
#include "../../include/memory_management/physical_memory_allocators/baseline.h"
// PhysicalMemoryAllocator*
#include "../../include/memory_management/physical_memory_allocators/physical_memory_allocator.h"

#include <sstream>


using SniperBaselineAllocator = BaselineAllocator<Sniper::Baseline::MetricsPolicy>;
using SniperReserveTHPAllocator = ReservationTHPAllocator<Sniper::ReserveTHP::MetricsPolicy>;

class AllocatorFactory
{
public:
    static PhysicalMemoryAllocator *createAllocator(String mimicos_name)
    {

        String allocator_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/memory_allocator_name");
        String allocator_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/memory_allocator_type");
        std::cout << "[MimicOS] [createAllocator] Creating physical memory allocator for " << mimicos_name <<
                     " - allocator_name = " << allocator_name <<
                    "  - allocator_type = " << allocator_type <<  std::endl;

        UInt64 memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/memory_size");
        UInt64 kernel_size = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/kernel_size");

        std::cout << "[MimicOS] Kernel size in MB: " << kernel_size << std::endl;

        if (allocator_type == "reserve_thp")
        {
            // Based on FreeBSD's reservation-based THP allocator
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            float threshold_for_promotion = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/threshold_for_promotion");
            return new SniperReserveTHPAllocator(allocator_type, memory_size, max_order, kernel_size, frag_type, threshold_for_promotion);
        }
        else if (allocator_type == "baseline")
        {
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            return new SniperBaselineAllocator(allocator_type, memory_size, max_order, kernel_size, frag_type);
        }
        else
        {
            std::cout << "[Sniper] Allocator type '" << allocator_type << "' not found" << std::endl;
            return nullptr;
        }
    }
};
