import pdb
import json
import sys
import threading
from mpkmemalloc import *
import tracemalloc
import gc
import os
# import psutil
import time
from random import randint
import ctypes
import copy
from ctypes import *
from copy import deepcopy
import threading

testOut = {}
def allocatedMemoryForOutputs():
    global testOut
    dummyData = ''
    for i in range(2*1024*1024):
        dummyData += '0'
    testOut['statusCode'] = 0
    testOut['body'] = dummyData

def function():
    dummyData = ''
    print('Address of dummyData: ', hex(id(dummyData)))
    for i in range(8192):
        dummyData += str(5)
    print('Reached python')
    print('Finished adding stuff to dummyData: ', hex(id(dummyData)))

    return {'body':dummyData, 'statusCode':200}

def functionWorker(): # tname = threading.currentThread().getName()
    pkey_thread_mapper()

    #Actual function goes here
    result = function()
    #Actual function ends here

    pdb.set_trace()
    pymem_allocate_from_shmem()
    result['statusCode'] = result['statusCode'] + 0
    result['body'] = result['body'] + '0'
    # gold = {}
    # copy_dict(result, gold)
    # rdpkru()
    pymem_reset()

def orchestrator():
    workers = 1
    threads = []
    for i in range(workers):
        threads.append(threading.Thread(target=functionWorker))

    for idx, thread in enumerate(threads):
        print("Before thread start")
        thread.start()
        thread.join()
        print("After thread start")

def main(params):
    print("before setup")
    # rdpkru()
    pymem_setup_allocators(0)
    # pymem_reset_pkru()
    print("after setup")
    # rdpkru()
    orchestrator()

    # rdpkru()

    result = {'statusCode':'Faastlane is cool!'}
    pymem_reset_pkru()
    return result

if __name__ == '__main__':
    gc.disable()
    # process = psutil.Process(os.getpid())
    print('PID: ', os.getpid())
    main({'workers':1})
    #snapshot = tracemalloc.take_snapshot()
    # print((process.memory_info().rss)/1024)  # in bytes
    #top_stats = snapshot.statistics('lineno')

# print("[ Top 10  ]")
# for stat in top_stats[:10]:
#     print(stat)
