#include "cxl_memory_tier_manager.h"

CXLMemoryTierManager::CXLMemoryTierManager()
    : m_enabled(false)
    , m_policy(PlacementPolicy::ALL_LOCAL)
    , m_local_dram_start(0)
    , m_local_dram_end(0)
    , m_cxl_start(0)
    , m_cxl_end(0)
    , m_local_dram_capacity(0)
    , m_cxl_capacity(0)
    , m_hot_threshold(100)
    , m_cold_threshold(10)
    , m_promotion_interval(SubsecondTime::Zero())
    , m_interleave_granularity(1)
    , m_interleave_counter(0)
    , m_current_interleave_tier(Tier::LOCAL_DRAM)
    , m_pages_promoted(0)
    , m_pages_demoted(0)
    , m_promotion_failures(0)
{
    m_tier_info.resize(static_cast<size_t>(Tier::NUM_TIERS));
}

CXLMemoryTierManager::~CXLMemoryTierManager()
{
}

CXLMemoryTierManager::Tier
CXLMemoryTierManager::getTier(IntPtr address)
{
    switch (m_policy)
    {
        case PlacementPolicy::ADDRESS_RANGE:
            return getTierAddressRange(address);
        case PlacementPolicy::FIRST_TOUCH:
            return getTierFirstTouch(getPageNumber(address));
        case PlacementPolicy::HOT_COLD:
            return getTierHotCold(getPageNumber(address));
        case PlacementPolicy::INTERLEAVE:
            return getTierInterleave(getPageNumber(address));
        case PlacementPolicy::ALL_LOCAL:
            return Tier::LOCAL_DRAM;
        case PlacementPolicy::ALL_CXL:
            return Tier::CXL_NEAR;
        default:
            return Tier::LOCAL_DRAM;
    }
}

void
CXLMemoryTierManager::recordAccess(IntPtr address, SubsecondTime time, bool is_write)
{
    ScopedLock sl(m_lock);
    IntPtr page_num = getPageNumber(address);
    updatePageAccess(page_num, time);

    Tier tier = getTier(address);
    size_t tier_idx = static_cast<size_t>(tier);
    if (tier_idx < m_tier_info.size())
    {
        m_tier_info[tier_idx].access_count++;
    }
}

void
CXLMemoryTierManager::checkMigration(SubsecondTime current_time)
{
    // Stub: migration logic for HOT_COLD policy
    if (m_policy != PlacementPolicy::HOT_COLD)
        return;
}

void
CXLMemoryTierManager::setAddressRanges(IntPtr local_start, IntPtr local_end,
                                        IntPtr cxl_start, IntPtr cxl_end)
{
    m_local_dram_start = local_start;
    m_local_dram_end = local_end;
    m_cxl_start = cxl_start;
    m_cxl_end = cxl_end;
}

double
CXLMemoryTierManager::getTierUtilization(Tier tier) const
{
    size_t idx = static_cast<size_t>(tier);
    if (idx < m_tier_info.size() && m_tier_info[idx].capacity_bytes > 0)
        return (double)m_tier_info[idx].used_bytes / (double)m_tier_info[idx].capacity_bytes;
    return 0.0;
}

CXLMemoryTierManager::Tier
CXLMemoryTierManager::getTierAddressRange(IntPtr address) const
{
    if (address >= m_cxl_start && address < m_cxl_end)
        return Tier::CXL_NEAR;
    return Tier::LOCAL_DRAM;
}

CXLMemoryTierManager::Tier
CXLMemoryTierManager::getTierFirstTouch(IntPtr page_num)
{
    auto it = m_page_table.find(page_num);
    if (it != m_page_table.end())
        return it->second.tier;

    // New page: allocate to local DRAM if capacity available
    PageInfo info;
    if (m_tier_info[0].used_bytes + PAGE_SIZE <= m_local_dram_capacity || m_local_dram_capacity == 0)
    {
        info.tier = Tier::LOCAL_DRAM;
        m_tier_info[0].used_bytes += PAGE_SIZE;
    }
    else
    {
        info.tier = Tier::CXL_NEAR;
        m_tier_info[1].used_bytes += PAGE_SIZE;
    }
    m_page_table[page_num] = info;
    return info.tier;
}

CXLMemoryTierManager::Tier
CXLMemoryTierManager::getTierHotCold(IntPtr page_num)
{
    auto it = m_page_table.find(page_num);
    if (it != m_page_table.end())
        return it->second.tier;

    // Default: local DRAM for new pages
    PageInfo info;
    info.tier = Tier::LOCAL_DRAM;
    m_page_table[page_num] = info;
    return Tier::LOCAL_DRAM;
}

CXLMemoryTierManager::Tier
CXLMemoryTierManager::getTierInterleave(IntPtr page_num)
{
    auto it = m_page_table.find(page_num);
    if (it != m_page_table.end())
        return it->second.tier;

    // Round-robin between tiers
    PageInfo info;
    info.tier = m_current_interleave_tier;
    m_page_table[page_num] = info;

    m_interleave_counter++;
    if (m_interleave_counter >= m_interleave_granularity)
    {
        m_interleave_counter = 0;
        if (m_current_interleave_tier == Tier::LOCAL_DRAM)
            m_current_interleave_tier = Tier::CXL_NEAR;
        else
            m_current_interleave_tier = Tier::LOCAL_DRAM;
    }
    return info.tier;
}

void
CXLMemoryTierManager::updatePageAccess(IntPtr page_num, SubsecondTime time)
{
    auto it = m_page_table.find(page_num);
    if (it == m_page_table.end())
    {
        PageInfo info;
        info.first_access = time;
        info.last_access = time;
        info.access_count = 1;
        m_page_table[page_num] = info;
    }
    else
    {
        it->second.last_access = time;
        it->second.access_count++;

        if (it->second.access_count >= m_hot_threshold)
            it->second.is_hot = true;
    }
}

bool
CXLMemoryTierManager::tryPromotePage(IntPtr page_num)
{
    auto it = m_page_table.find(page_num);
    if (it == m_page_table.end() || it->second.tier == Tier::LOCAL_DRAM)
        return false;

    if (m_local_dram_capacity > 0 &&
        m_tier_info[0].used_bytes + PAGE_SIZE > m_local_dram_capacity)
    {
        m_promotion_failures++;
        return false;
    }

    // Move page from CXL to local DRAM
    size_t old_tier = static_cast<size_t>(it->second.tier);
    m_tier_info[old_tier].used_bytes -= PAGE_SIZE;
    m_tier_info[0].used_bytes += PAGE_SIZE;
    it->second.tier = Tier::LOCAL_DRAM;
    m_pages_promoted++;
    return true;
}

bool
CXLMemoryTierManager::tryDemotePage(IntPtr page_num)
{
    auto it = m_page_table.find(page_num);
    if (it == m_page_table.end() || it->second.tier != Tier::LOCAL_DRAM)
        return false;

    // Move page from local DRAM to CXL
    m_tier_info[0].used_bytes -= PAGE_SIZE;
    m_tier_info[1].used_bytes += PAGE_SIZE;
    it->second.tier = Tier::CXL_NEAR;
    m_pages_demoted++;
    return true;
}
