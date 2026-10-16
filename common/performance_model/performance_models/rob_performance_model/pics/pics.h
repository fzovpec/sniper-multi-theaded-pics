#ifndef PICS_H_
#define PICS_H_
#include "subsecond_time.h"
#include "simulator.h"
#include "dvfs_manager.h"

// #define DEBUG_PICS

class PICS_base
{
    protected:
        static float to_cycles(SubsecondTime t)
        {
            return static_cast<float>(t.getFS()) /
                static_cast<float>(Sim()->getDvfsManager()->getGlobalDomain()->getPeriod().getFS());
        }


        static void add_time(SubsecondTime& member, SubsecondTime t, const char* label)
        {
    #ifdef DEBUG_PICS
            std::cout << "Adding " << label << ": " << to_cycles(t) << "\n";
    #endif
            member += t;
        }
};


class PICS_d : private PICS_base
{
    private:
        SubsecondTime base {SubsecondTime::Zero()};
        SubsecondTime fe_stall{SubsecondTime::Zero()};
        SubsecondTime be_stall{SubsecondTime::Zero()};
        SubsecondTime mispred {SubsecondTime::Zero()};
        SubsecondTime mt_wait {SubsecondTime::Zero()};

    public:
        void add_base(SubsecondTime t) { add_time(base, t, "base"); }
        void add_fe_stall(SubsecondTime t) { add_time(fe_stall, t, "fe_stall"); }
        void add_be_stall(SubsecondTime t) { add_time(be_stall, t, "be_stall"); }
        void add_mispred(SubsecondTime t) { add_time(mispred, t, "mispred"); }
        void add_mt_wait(SubsecondTime t) { add_time(mt_wait, t, "mt_wait"); }

        float get_base_cyc() const { return to_cycles(base); }
        float get_fe_stall_cyc() const { return to_cycles(fe_stall); }
        float get_be_stall_cyc() const { return to_cycles(be_stall); }
        float get_mispred_cyc() const { return to_cycles(mispred); }
        float get_mt_wait_cyc() const { return to_cycles(mt_wait); }
        SubsecondTime get_total_time() const { return base + fe_stall + be_stall + mispred; }
};

class PICS_c : private PICS_base
{
    private:
        SubsecondTime compute{SubsecondTime::Zero()};
        SubsecondTime drained{SubsecondTime::Zero()};
        SubsecondTime stalled{SubsecondTime::Zero()};
        SubsecondTime flushed{SubsecondTime::Zero()};
        SubsecondTime mt_wait{SubsecondTime::Zero()};

    public:
        void add_compute(SubsecondTime t) { add_time(compute, t, "compute"); }
        void add_drained(SubsecondTime t) { add_time(drained, t, "drained"); }
        void add_stalled(SubsecondTime t) { add_time(stalled, t, "stalled"); }
        void add_flushed(SubsecondTime t) { add_time(flushed, t, "flushed"); }
        void add_mt_wait(SubsecondTime t) { add_time(mt_wait, t, "mt_wait"); }

        float get_compute_cyc() const { return to_cycles(compute); }
        float get_drained_cyc() const { return to_cycles(drained); }
        float get_stalled_cyc() const { return to_cycles(stalled); }
        float get_flushed_cyc() const { return to_cycles(flushed); }
        float get_mt_wait_cyc() const { return to_cycles(mt_wait); }
        SubsecondTime get_total_time() const { return compute + drained + stalled + flushed; }
    };

#endif