#include "spin_loop_detector.h"
#include "spin_loop_detector_li_sorin.h"
#include "spin_loop_detector_tian.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

SpinLoopDetector* SpinLoopDetector::create()
{
   bool enabled = Sim()->getCfg()->getBool("core/spin_loop_detection");
   if (!enabled)
      return NULL;

   String algo = Sim()->getCfg()->getString("core/spin_loop_detector/algorithm");
   if (algo == "tian")
   {
      UInt32 threshold = Sim()->getCfg()->getInt("core/spin_loop_detector/tian/threshold");
      printf("[SPIN_DETECTOR] Initialized Tian et al. (ISSTA'08) algorithm with threshold %u\n", threshold);
      return new SpinLoopDetectorTian(threshold);
   }
   else if (algo == "li_sorin")
   {
      printf("[SPIN_DETECTOR] Initialized Li & Sorin (TPDS'06) algorithm\n");
      return new SpinLoopDetectorLiSorin();
   }
   else
   {
      LOG_PRINT_ERROR("Unknown spin loop detector algorithm: %s", algo.c_str());
      return NULL;
   }
}

