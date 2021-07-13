import threading
import sys
import os
import pdb
from optparse import OptionParser
import gc

def main(params):
    data = open("bm_alloc.out", "r")
    contents = data.readlines()
    pdb.set_trace()
    addr_pkeys_map = {}
    for line in contents:
        try:
            [addr, pkey] = line.split(',')
            if addr in addr_pkeys_map:
                if pkey not in addr_pkeys_map[addr]:
                    addr_pkeys_map[addr].append(pkey)
            else:
                addr_pkeys_map[addr] = [pkey]
        except:
            print(line)

    pdb.set_trace()
    for addr, pkeys in addr_pkeys_map:
        if len(pkeys) > 2:
            print(addr, pkeys)


if __name__ == '__main__':
    main({'workers':2})
