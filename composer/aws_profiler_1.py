'''A backup of technique which was attempted earlier.

These methods MUST be inside the AWS class in aws.py

Purpose: Check for a given function execution time, map is better or batch
Methodology: 
1. For some arbitrary compute intensive function exec time, launch functions
as map and batch both.
2. Plot a curve, at some point, map should become faster than batch (due to 
limited cores on a single lambda instance)
3. Check where the given input time fits in the curve and decide to launch as
map or batch.
'''

def __batchProfile(self, size, procs, times, ITER):
    appDir = common.PROFILER_APPDIR
    megaFnFile = common.PROFILER_FUNC_NAME
    function = common.PROFILER_HANDLER

    role = self.__manageRole(megaFnFile, function)
    func_name = self.__functionName(megaFnFile, function)
    self.__deploy(appDir, megaFnFile, function, role, size)
    result = {}
    for time in times:
        result[time] = {}
        for proc in procs:
            # WARN! do not change perm to any higher, trouble with boto3!
            metadata = {'workers': proc, 'perm': time, 'name': func_name}
            info = []
            stats = []
            for i in range(ITER):
                try:
                    response = self.invoke(metadata)
                    if not response:
                        continue
                    # Response object, get payload output
                    payload = json.loads(response['Payload'].read())
                    stats.append(json.dumps(payload) + "\n")
                    # These are related to main.py output! change carefully!
                    info.append(payload['end_join'] - payload['start_create'])
                except Exception as e:
                    pass

            # This is for plots (probably)
            result[time][proc] = common.getStats(info)

            # Write raw data for later reference?
            if self.debug:
                name = "batch-" + str(size) + "MB-" + str(proc) + "-" + str(time) + ".out"
                common.putJson(name, stats)

    self.__delete(megaFnFile, function)
    return result


def __mapProfile(self, size, procs, times, ITER):
    client = boto3.client("stepfunctions")
    # Many steps skipped here, ideally want to build this workflow from scratch
    arn = "arn:aws:states:us-west-2:061621106836:stateMachine:MapProfiler"
    data = {}
    lambdaData = {}
    for argTime in times:
        data[argTime] = {}
        lambdaTime = []
        for proc in procs:
            payload = {}
            # Generate payload, it's a map needs an array input
            # WARN: itemsPath here is related to state-machine
            payload["itemsPath"] = []
            for i in range(proc):
                payload["itemsPath"].append({"perm": argTime})
            runtimes = []
            for i in range(ITER):
                try:
                    response = client.start_execution(stateMachineArn=arn,
                        input=json.dumps(payload))
                    exec_arn = response["executionArn"]
                    retries = 20
                    while retries > 0:
                        response = client.describe_execution(executionArn=exec_arn)
                        status = response["status"]
                        if status == "RUNNING":
                            retries -= 1
                            time.sleep(1)
                            continue
                        elif status == "SUCCEEDED":
                            output = json.loads(response["output"])
                            runtimes.append(output["map_time"])
                            lambdaTime.append(output["median"])
                        else:
                            # Some kind of error
                            print(status)
                        break
                except Exception as e:
                    print(str(e))

            data[argTime][proc] = common.getStats(runtimes)

        lambdaData[argTime] = common.getStats(lambdaTime)[0]

    # cleanup, remove the step function and component functions

    return data, lambdaData


def __getBatchCount(self, inTime, times, profileDict):
    batchCount = -1
    for t in times:
        # json.dumps makes keys as strings even if they were originally int
        # workaround this way while reading the json!
        time = str(t)
        if inTime < profileDict[time][0]:
            batchCount = profileDict[time][1]
            break
    # It's safe to return 1 as a -1 means inTime is very large
    # after which advantage of batching diminishes! (empirically verified!)
    return batchCount if (batchCount != -1) else 1


def profile(self, inTime, size=128, useCache=False):
    # For now, one memory size, can do for more, another loop!
    procCount = [2, 4, 8, 12, 16, 20, 24]
    times = [4, 5, 6, 7, 8, 9]
    ITER = 10

    fileName = "profiler-cache/{}.json".format(size)
    if useCache:
        out = common.getJson(fileName)
        if out:
            return self.__getBatchCount(inTime, times, out)

    batchData = self.__batchProfile(size, procCount, times, ITER)
    mapData, lambdaData = self.__mapProfile(size, procCount, times, ITER)
    """Expected Output structures =>
       mapData, batchData:
       For each time in times, a dict of different proc in procCount, with
       corresponding median and 99'ile latencies in milliseconds!
       lambdaData:
       For each time in times, function execution time.
    """
    out = {}
    for time in times:
        key = str(time)
        for i in range(len(procCount)):
            proc = procCount[i]
            if mapData[time][proc][0] > batchData[time][proc][0]:
                continue
            else:
                out[key] = (lambdaData[time], procCount[i-1] if i > 0 else 1)
                break
        if key not in out:
            # Means, let's use highest
            out[key] = (lambdaData[time], procCount[-1])

    # Cache "out" dict, always!
    common.putJson(fileName, out)

    return self.__getBatchCount(inTime, times, out)
