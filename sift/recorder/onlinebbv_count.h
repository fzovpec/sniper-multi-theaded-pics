#ifndef BBV_COUNT_H
#define BBV_COUNT_H

#define __STDC_LIMIT_MACROS
#include <map>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "globals.h"
class Bbv_util;

// possibly change to template class?
// normalised BBV cluster centers are doubles
class Bbv
{
private:
  uint64_t * m_instrs;
  uint64_t * m_bbv_counts;
  uint32_t thread_id;

public:
  // Number of dimensions to project BBVs to
  // Make sure this remains a multiple of four, or update the unrolled loop in
  // BbvCount::count
 // static const int NUM_BBV = 16;

  Bbv(uint32_t tid);
  Bbv(uint32_t tid,std::vector<uint64_t>& bbv_counts);

  void count(uint64_t address, uint64_t count);
  void clear();

  uint64_t getDimension(int dim) const { return m_bbv_counts[dim]; }
  uint64_t getInstructionCount(void) const { return *m_instrs; }
//  const std::vector<uint64_t>& getBbv(void) const { 
//      std::vector<uint64_t> tmp = std::vector<uint64_t>(NUM_BBV);
//      memcpy(&tmp[0],m_bbv_counts,NUM_BBV*sizeof(uint64_t));
//      return tmp; 
//  
//  }

  uint64_t* getBbvPtr(void) const {
      return m_bbv_counts;
  } 
};

class Bbv_util
{
private:
  // barrier_bbv.txt
  // m_BbvProfile[b][t] => bbv at barrier b for thread t
  static std::vector<std::vector<Bbv>> m_BbvProfile; // store barrier_bbv.txt
  static int validateBbvProfile(std::string filePath);

  // t.projMatrix
  // precomputed random projection matrix used in SimPoint dim reduction routine
  // dim: total unique bbls (dim before reduction) : 16 (dim after reduction)
  static uint64_t m_bbvDim; // bbv dime before reduction ( total unique bbls)
  static std::vector<std::vector<double>> m_randomProjectionMatrix;
  static std::vector<double> m_weights;

  // barrier_bbv.txt_count_per_thread
  // mBbvStandardProfile[b][t] => list of [bblid, bblcount] at barrier b for
  // thread t
  static uint64_t m_maxBbvId;
  static std::vector<std::vector<std::vector<std::pair<uint64_t, uint64_t>>>>
    m_BbvStandardProfile; // store barrier_bbv.count_per_thread, for querying
                          // standard bbv at runtime

  // t.finalctr
  // holding cluster centers computed by simpoint
  static std::vector<std::vector<double>> m_BbvClusterCenters;
  static std::vector<double> m_BbvClusterCentersWeights;

  // t.simpoint
  // [barrier region id, cluster id]
  static std::map<uint64_t, uint64_t> m_BbvSimpoints;

  // t.labels
  static std::vector<uint64_t> m_BbvLabels;

public:
  // BBV after dim reduction by tool_barrier_bbv, barrier_bbv.txt
  static int loadBbvProfile(std::string filePath);
  // barrier_bbv.txt_count_per_thread
  static int loadBbvStandardProfile(std::string filePath);
  // t.projMatrix
  static int loadRandomProjectionMatrix(std::string filePath);
  // t.finalctr
  static int loadBbvClusterCenters(std::string filePath);
  // t.simpopint
  static int loadBbvSimpoints(std::string filePath);
  // t.label
  static int loadBbvLabels(std::string filePath);
  // random projection follow simpoint
  static std::vector<double> randomProjection(
    const std::vector<std::pair<uint64_t, uint64_t>>& standardBbv);

  static int findBbvCluster(
    const std::vector<std::pair<uint64_t, uint64_t>>& standardBbv);

  // getter
  static const std::vector<
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>>>&
  getBbvStandardProfile()
  {
    return m_BbvStandardProfile;
  }

  static const std::vector<double>& getBbvClusterCenterWeights()
  {
    return m_BbvClusterCentersWeights;
  }

  static uint64_t getBbvDimension() { return m_bbvDim; }

  static uint64_t getMaxBbvId() { return m_maxBbvId; }

  static uint64_t getClusterSize() { return m_BbvClusterCenters.size(); }

  static uint64_t getLabel(uint64_t barrier_id)
  {
    return m_BbvLabels[barrier_id];
  }

  static bool isSimPoint(uint64_t barrier_id)
  {
    return m_BbvSimpoints.find(barrier_id) != m_BbvSimpoints.end();
  }
};

#endif // BBV_COUNT_H
