from multiprocessing import Process
import time

DEFAULT_ARG = 10

def piWorker(n):
    pi_by_4 = 0
    max_denominator = 10**(n+3)
    max_iter = int((max_denominator-1)/2) #denominator = 2*iter+1
    for i in range(max_iter):
        pi_by_4 += ((-1)**i)*(1/(2*i+1))*(10**(n+3))
    return int(pi_by_4 * 4/1000)


def permWorker(n):
    count = list(range(1, n + 1))
    max_flips = 0
    m = n - 1
    r = n
    check = 0
    perm1 = list(range(n))
    perm = list(range(n))
    perm1_ins = perm1.insert
    perm1_pop = perm1.pop

    while 1:
        if check < 30:
            check += 1

        while r != 1:
            count[r - 1] = r
            r -= 1

        if perm1[0] != 0 and perm1[m] != m:
            perm = perm1[:]
            flips_count = 0
            k = perm[0]
            while k:
                perm[:k + 1] = perm[k::-1]
                flips_count += 1
                k = perm[0]

            if flips_count > max_flips:
                max_flips = flips_count

        while r != n:
            perm1_ins(r, perm1_pop(0))
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        else:
            return max_flips

def handler(event, context):
    workers = event.get('workers', 1)
    perm = event.get('perm', DEFAULT_ARG)

    procs = [] 
    for i in range (workers):
        procs.append (Process (target=permWorker, args=[perm]))

    start_create = time.time()*1000
    for idx, proc in enumerate(procs):
        proc.start() 
    end_create = time.time()*1000

    start_join = time.time()*1000
    for idx, proc in enumerate(procs):
        proc.join()
    end_join = time.time()*1000

    resp = {}
    resp['start_create'] = start_create
    resp['end_create'] = end_create
    resp['start_join'] = start_join
    resp['end_join'] = end_join

    return resp
