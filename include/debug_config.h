/* 0 -> no debug, 1 -> debug, 2 -> detailed debug */

#pragma once

/* DEBUG levels */
#define DEBUG_NONE     0
#define DEBUG_BASIC    1
#define DEBUG_DETAILED 2

/* Unified log file - when enabled, all components also write to a single file for timing analysis */
#define UNIFIED_LOG_ENABLED  0  /* 0 = disabled, 1 = enabled */
#define UNIFIED_LOG_FILENAME "unified_debug.log"

// DRAM Performance Model
#define DEBUG_DRAM_DETAILED         DEBUG_NONE /* 0, 1 or 2 */ // Detailed DRAM timing model debug messages
#define DEBUG_DRAM_CSV              0          /* 0 = disabled, 1 = enabled */ // DRAM latency and PTW CSV logging
