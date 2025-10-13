#!/usr/bin/env python3
#import sys
import os
from pacsimscripts import toolset,mtng_dumpjson

def get_sim_time(filepath="./tmp/"):
    time_mtng, wall_mtng,l2missrate = mtng_dumpjson.get_full_time( filepath,os.environ.get("SNIPER_ROOT"),False )
    print_info = "[PacSim] Estimated Sim Time %.3fs"%(time_mtng)
    return print_info
if __name__ == "__main__":
    time_mtng= get_sim_time()
    print(time_mtng)
