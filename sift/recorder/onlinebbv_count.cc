#include "onlinebbv_count.h"
#include "sift/sift_assert.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// RNG parameters, defaults taken from drand48
#define RNG_A __UINT64_C(0x5DEECE66D)
#define RNG_C __UINT64_C(0xB)
#define RNG_M ((__UINT64_C(1) << 48) - 1)

// Same as drand48, but inlined for efficiency
inline uint64_t
rng_next(uint64_t& state)
{
  state = (RNG_A * state + RNG_C) & RNG_M;
  return state >> 16;
}
inline uint64_t
rng_seed(uint64_t seed)
{
  return (seed << 16) + 0x330E;
}

Bbv::Bbv(uint32_t thread_id_para)
  
{
    thread_id = thread_id_para;
    m_instrs = &global_m_bbv_counters[thread_id];
    m_bbv_counts = NUM_BBV * thread_id + (&global_m_bbv_counts[0]);
    memset(m_bbv_counts,0,NUM_BBV*sizeof(uint64_t));
}

Bbv::Bbv(uint32_t thread_id_para,std::vector<uint64_t>& bbv_counts)
{
  // FIXME: m_instr is not correctly initialised, is this the accumulate sum of
  // m_bbv_counts?
  thread_id = thread_id_para;
  m_instrs = &global_m_bbv_counters[thread_id];
  m_instrs[0] = 0;
  thread_id = thread_id_para;
  memcpy( &global_m_bbv_counts[ NUM_BBV*thread_id ] , &bbv_counts[0] , NUM_BBV*sizeof(uint64_t));
  m_bbv_counts = NUM_BBV * thread_id + (&global_m_bbv_counts[0]);
//  m_bbv_counts = bbv_counts;
}

void
Bbv::count(uint64_t address, uint64_t count)
{
  *m_instrs += count;
  // Perform random projection of basic-block vectors onto NUM_BBV dimensions
  // As there are too many BBVs, we cannot store the projection matrix, rather,
  // we re-generate it on request using an RNG seeded with the BBV address.
  // Since this is a hot loop in FAST_FORWARD mode, use an inlined RNG
  // and four parallel code paths to exploit as much ILP as possible.
  uint64_t s0 = rng_seed(address), s1 = rng_seed(address + 1),
           s2 = rng_seed(address + 2), s3 = rng_seed(address + 3);
  for (int i = 0; i < NUM_BBV; i += 4) {
    uint64_t weight = rng_next(s0);
    m_bbv_counts[i] += (weight & 0xffff) * count;
    weight = rng_next(s1);
    m_bbv_counts[i + 1] += (weight & 0xffff) * count;
    weight = rng_next(s2);
    m_bbv_counts[i + 2] += (weight & 0xffff) * count;
    weight = rng_next(s3);
    m_bbv_counts[i + 3] += (weight & 0xffff) * count;
  }
}

void
Bbv::clear()
{
  *m_instrs = 0;
  for (int i = 0; i < NUM_BBV; ++i)
    m_bbv_counts[i] = 0;
}

/*****************************/
/*      MTNG BBV UTIL        */
/*****************************/

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

std::vector<std::vector<Bbv>> Bbv_util::m_BbvProfile;

// return 0 on success, -1 on error
// TODO inefficient due to substr operation, refactor by following loadBbvFv()
int
Bbv_util::loadBbvProfile(std::string filePath)
{
  // Buggy, stat symbol is unlinked(?)
  /*
  struct stat st;
  if (stat(filePath.c_str() , &st) != 0)
  {
      std::cout << "[Bbv  ] Error opening file: " << filePath << std::endl;
      return -1;
  }
  */

  m_BbvProfile.clear();

  std::ifstream fin(filePath.c_str());
  if (!fin.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }

  std::string next; // token
  uint64_t b = 0;   // current barrier number
  std::vector<uint64_t> bbv_counts(NUM_BBV, 0);
  // m_BbvProfile.push_back(std::vector<Bbv>());

  // contro flow
  uint64_t last_thread_num = 0;
  uint32_t first_pass = 1;

  while (getline(fin, next, ' ')) {
    // if at new barrer region

    if (next[0] == '\n')
      next = next.substr(1);
    if (next[0] == 'T') {
      // update bbv of last barrier region
      if (!first_pass) {
        m_BbvProfile[b].push_back(Bbv(last_thread_num,bbv_counts));

        // initialise new bbv_counts for new thread
        bbv_counts = std::vector<uint64_t>(NUM_BBV, 0);
        last_thread_num = 0;
        b++;
      }

      // new bbv region
      // each bbv region contain bbvs from all threads
      m_BbvProfile.push_back(std::vector<Bbv>());
      next = next.substr(2); // remove "T:""
    } else if (next[0] == ':') {
      next = next.substr(1);
    } else {
      // std::cout << "error when parsing: " << next << std::endl;
      continue;
    }

    first_pass = 0;

    uint64_t bbv_num, bbv_value, thread_num;
    auto delim_pos = next.find(':');

    bbv_num = strtoull(next.substr(0, delim_pos).c_str(), 0, 10);
    bbv_value = strtoull(next.substr(delim_pos + 1).c_str(), 0, 10);

    // data format follows barrierPoint
    // see barrierpoint/tool_barrier_bbv/tool_barrier_bbv.cpp
    thread_num = (bbv_num - 1) / NUM_BBV;
    bbv_num = (bbv_num - 1) % NUM_BBV;

    if (thread_num > last_thread_num) // new thread
    {
      if (bbv_num != 0) {
        std::cout << "[Bbv  ] Error when parsing file: " << filePath
                  << std::endl;
        return -1;
      }
      m_BbvProfile[b].push_back(Bbv(thread_num,bbv_counts));
      // initialise new bbv_counts for new thread
      bbv_counts = std::vector<uint64_t>(NUM_BBV, 0);
    }

    bbv_counts[bbv_num] = bbv_value;
    last_thread_num = thread_num;
  }
  // update last BBV of last thread
  m_BbvProfile[b].push_back(Bbv(last_thread_num,bbv_counts));
  // Bbv_util::validateBbvProfile(filePath);

  return 0;
}

// return 0 on success, -1 on error
int
Bbv_util::validateBbvProfile(std::string filePath)
{
  std::ofstream fout((filePath + "_tmp").c_str());

  // iterate through barrier regions
  for (unsigned int b = 0; b < m_BbvProfile.size(); b++) {
    fout << 'T';
    // iterate through all threads
    for (uint64_t t = 0; t < (uint64_t)m_BbvProfile[b].size(); t++) {
      uint64_t * bbv = m_BbvProfile[b][t].getBbvPtr();

      // iterate through all entries in BBV
      for (uint64_t bb = 0; bb < (uint64_t)NUM_BBV; bb++) {
        // : global bbv entry id :  global bbv entry count
        fout << ":" << (1 + bb + (t * NUM_BBV)) << ":" << bbv[bb] << " ";
      }
    }
    fout << '\n';
  }
  fout.close();
  return 0;
}

uint64_t Bbv_util::m_bbvDim = 0;
std::vector<std::vector<double>> Bbv_util::m_randomProjectionMatrix;
std::vector<double> Bbv_util::m_weights;

int
Bbv_util::loadRandomProjectionMatrix(std::string filePath)
{
  std::ifstream is(filePath.c_str());
  if (!is.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }

  unsigned int matsize;
  is >> matsize;

  m_bbvDim = matsize;
  m_randomProjectionMatrix.resize(matsize);
  m_weights.resize(matsize);

  char c = is.get();
  sift_assert(c == ':');

  // check for weights (backwards compatibility)
  c = is.get();
  bool hasWeights = (c == 'w');

  double weight;
  if (!hasWeights) {
    is.putback(c);
  }

  for (unsigned int i = 0; i < matsize; i++) {
    if (hasWeights) {
      is >> weight;
      m_weights[i] = weight;
    } else {
      m_weights[i] = 1.0 / matsize;
    }

    unsigned int reduced_dim;
    is >> reduced_dim;
    char c = is.get();
    sift_assert(c == ':');

    m_randomProjectionMatrix[i].resize(reduced_dim);

    for (unsigned int j = 0; j < reduced_dim; j++) {
      double val;
      is >> val;
      m_randomProjectionMatrix[i][j] = val;
    }
  }
  return 0;
}

std::vector<std::vector<std::vector<std::pair<uint64_t, uint64_t>>>>
  Bbv_util::m_BbvStandardProfile; // store barrier_bbv.count_per_thread, for
                                  // querying standard bbv at runtime
uint64_t Bbv_util::m_maxBbvId = 0;

int
Bbv_util::loadBbvStandardProfile(std::string filePath)
{
  using namespace std;

  std::ifstream is(filePath.c_str());
  if (!is.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }

  std::string line;
  while (getline(is, line, '\n')) {
    // each pass is a new barrier region
    std::istringstream ss(line);
    char c = '.';
    ss >> c;
    if (c == 'W')
      continue; // discard weights, used by simpoint to compute random vector
                // matrix
    sift_assert(c == 'T');

    std::string next; // get :<bbvid>:<threadid>:<bbvcount>
    // bbv_allthreads[i] => list of [bblid,bblcount] for thread i
    vector<vector<pair<uint64_t, uint64_t>>> bbv_allthreads;

    while (getline(ss, next, ' ')) {
      sift_assert(next[0] == ':');
      unsigned int delim1 = next.find(':', 1);
      unsigned int delim2 = next.find(':', delim1 + 1);
      // sift_assert(delim1 != std::string::npos && delim2 !=
      // std::string::npos); // this check is erroneous

      // cout << delim1 << " " << delim2 << endl;
      // cout <<  next.substr(1,delim1-1).c_str() << endl;
      // cout <<  next.substr(delim1+1,delim2-delim1-1).c_str() << endl;
      // cout <<  next.substr(delim2+1).c_str() << endl;

      uint64_t bbvid = strtoull(next.substr(1, delim1 - 1).c_str(), 0, 10);
      uint64_t threadid =
        strtoull(next.substr(delim1 + 1, delim2 - delim1 - 1).c_str(), 0, 10);
      uint64_t bbvcount = strtoull(next.substr(delim2 + 1).c_str(), 0, 10);

      if (threadid >= bbv_allthreads.size()) {
        bbv_allthreads.push_back(vector<pair<uint64_t, uint64_t>>());
      }

      bbv_allthreads[threadid].push_back({ bbvid, bbvcount });
      m_maxBbvId = std::max<uint64_t>(m_maxBbvId, bbvid);
    }

    // move semantics here??
    m_BbvStandardProfile.push_back(bbv_allthreads);
  }

  // validation
  /*
  for (auto & threads : m_BbvStandardProfile)
  {
      for (auto & thread: threads)
      {
          for (auto & bbvpair : thread)
              cout << bbvpair.first << ":" << bbvpair.second << " ";
      }
      cout << endl;
  }
  cout << endl;
  */

  return 0;
}

std::vector<std::vector<double>> Bbv_util::m_BbvClusterCenters;
std::vector<double> Bbv_util::m_BbvClusterCentersWeights;

int
Bbv_util::loadBbvClusterCenters(std::string filePath)
{
  std::ifstream is(filePath.c_str());
  if (!is.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }

  unsigned int cluster_size;
  is >> cluster_size;

  m_BbvClusterCenters.resize(cluster_size);
  m_BbvClusterCentersWeights.resize(cluster_size);

  char c = is.get();
  sift_assert(c == ':');

  // check for weights (backwards compatibility)
  c = is.get();
  bool hasWeights = (c == 'w');

  double weight;
  if (!hasWeights) {
    is.putback(c);
  }

  for (unsigned int i = 0; i < cluster_size; i++) {
    if (hasWeights) {
      is >> weight;
      m_BbvClusterCentersWeights[i] = weight;
    } else {
      m_BbvClusterCentersWeights[i] = 1.0 / cluster_size;
    }

    unsigned int reduced_dim;
    is >> reduced_dim;
    sift_assert(reduced_dim == NUM_BBV);

    // store entries of BBV cluster center
    m_BbvClusterCenters[i].resize(reduced_dim);

    char c = is.get();
    sift_assert(c == ':');

    for (unsigned int j = 0; j < reduced_dim; j++)
      is >> m_BbvClusterCenters[i][j];
  }

  // validation code
  /*
  for (auto& i : m_BbvClusterCentersWeights )
      std::cout << i << " " ;
  std::cout << std::endl;

  for (auto & i : m_BbvClusterCenters)
  {
      for (auto& j: i )
          std::cout << j << " ";
      std::cout << std::endl;
  }
  */

  return 0;
}

std::map<uint64_t, uint64_t> Bbv_util::m_BbvSimpoints;

int
Bbv_util::loadBbvSimpoints(std::string filePath)
{
  std::ifstream is(filePath.c_str());
  if (!is.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }
  uint64_t barrier_id = -1, cluster_id = -1;
  is >> barrier_id >> cluster_id;
  while (!is.eof()) {
    sift_assert(m_BbvSimpoints.find(barrier_id) == m_BbvSimpoints.end());
    m_BbvSimpoints[barrier_id] = cluster_id;
    is >> barrier_id >> cluster_id;
  }
  return 0;
}

// FIXME: Since the input is a single BBV vector, there is no computation of
// "weight" of Bbv. Does this matter bbv is treated as rows, wich is of dim (1 x
// no. of unique bbls) proj matrix has dim ( no. of unique bbls x resulting dim
// (16) ) hence this code is just a matrix multiplication to produce (1 x 16)
std::vector<double>
Bbv_util::randomProjection(
  const std::vector<std::pair<uint64_t, uint64_t>>& standardBbv)
{

  std::vector<double> bbv_dim_reduced(NUM_BBV, 0.0);
  double sumVector = 0.0;

  // normalise
  // each entry in standardBbv is {bbvid , bbvcount} pair
  for (auto& bbpair : standardBbv)
    sumVector += bbpair.second;

  // multiply the row we just pulled from the parser by *each column*
  // of the projection matrix to obtain the row in the result.
  // over all original dimensions (columns of the original vector, rows of
  // the projection matrix)
  for (auto& bbpair : standardBbv) {
    // FIXME: confirm if dim is the GLOBAL bbvid? (bbvid + threadid offset)
    unsigned int dim = bbpair.first; // bbvid corresponds to the dimension
    double bbcount = (double)bbpair.second / sumVector;
    // if (dim > largestDimensionSeen) { largestDimensionSeen = dim; }

    dim--;                                   // 0-indexing
    sift_assert(dim < m_bbvDim && dim >= 0); // dim < m_bbvDim * thread number

    // project the row
    for (unsigned int projCol = 0; projCol < NUM_BBV; projCol++) {
      // note that dimension is changed to be offset 0
      bbv_dim_reduced[projCol] +=
        bbcount * m_randomProjectionMatrix[dim][projCol]; // * m_weights[dim];
    }
  }
  /*
  std::cout << "bbv_dim_reduced: " << std::endl;
  for (auto & i : bbv_dim_reduced)
  {
      std::cout << i << " ";
  }
  std::cout << std::endl;
  */

  return bbv_dim_reduced;
}

int
Bbv_util::findBbvCluster(
  const std::vector<std::pair<uint64_t, uint64_t>>& standardBbv)
{
  auto dimReducedBbv = randomProjection(standardBbv);

  double best = std::numeric_limits<double>::max();
  int best_cluster = 0;
  int current_cluster = 0;

  for (auto& clusterCenter : m_BbvClusterCenters) {
    sift_assert(dimReducedBbv.size() == clusterCenter.size());
    double result = 0.0;
    for (unsigned int i = 0; i < dimReducedBbv.size(); i++)
      // result += abs(dimReducedBbv[i] - clusterCenter[i]);
      result += pow((dimReducedBbv[i] - clusterCenter[i]), 2);
    result = sqrt(result);

    // result *= m_BbvClusterCentersWeights[current_cluster]; // is weight used
    // this way? std::cout << "current score: "<< result  << std::endl;
    // std::cout << "current weight: " <<
    // m_BbvClusterCentersWeights[current_cluster] << std::endl;
    if (result < best) {
      best = result;
      best_cluster = current_cluster;
    }
    current_cluster++;
  }
  std::cout << "Best score: " << best << "\n";
  return best_cluster;
}

std::vector<uint64_t> Bbv_util::m_BbvLabels;

int
Bbv_util::loadBbvLabels(std::string filePath)
{
  std::ifstream is(filePath.c_str());
  if (!is.is_open()) {
    std::cerr << "[BBV ERROR] File: " << filePath << " does not exist! \n";
    return -1;
  }

  std::string line;
  while (getline(is, line, '\n')) {
    unsigned int delim = line.find(' ');
    uint64_t label = strtoull(line.substr(delim - 1).c_str(), 0, 10);
    m_BbvLabels.push_back(label);
  }
  return 0;
}
