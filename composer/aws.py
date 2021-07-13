import botocore
import boto3
import common
import faasplatform
import json
import os
import pdb
import subprocess
import time
import yaml

class AWS(faasplatform.Faasplatform):
    def __init__(self, debug=False):
        self.lamb = boto3.client('lambda')
        self.iam  = boto3.client('iam')
        self.appDir = None
        self.appName = None
        self.stage = None
        self.functionsToRegister = []
        self.debug = debug

    def __functionName(self, megaFnFile, function):
        return '{}-{}'.format(megaFnFile, function)

    def __handlerName(self, megaFnFile, function):
        return '{}.{}'.format(megaFnFile, function)

    def __manageRole(self, megaFnFile, function):
        roleName = 'faastlane-{}'.format(self.__functionName(megaFnFile, function))

        try:
            role  = self.iam.get_role(RoleName = roleName)
        except self.iam.exceptions.NoSuchEntityException:
            #Role does not exist, create one
            assume_role_policy_document = json.dumps({"Version": "2012-10-17",
                                "Statement": [{"Effect": "Allow",
                                "Principal": {"Service": "lambda.amazonaws.com"},
                                "Action": "sts:AssumeRole"}]})

            role = self.iam.create_role(RoleName = roleName,
                                  AssumeRolePolicyDocument = assume_role_policy_document)

            self.iam.attach_role_policy(RoleName = role['Role']['RoleName'],
            PolicyArn = 'arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole')
        except:
            raise

        return role

    def __delete(self, megaFnFile, function):
        fnName = self.__functionName(megaFnFile, function)
        self.lamb.delete_function(FunctionName=fnName)

    def __deploy(self, appDir, megaFnFile, function, role, size=128):
        fnName = self.__functionName(megaFnFile, function)
        package = '{}/{}.zip'.format(appDir, fnName)
        os.system('cd {} && zip -r {} *'.format(appDir, package))

        try:
            response = self.lamb.get_function(FunctionName = fnName)
            response = self.lamb.update_function_code(FunctionName = fnName,
                                           ZipFile = open(package, 'rb').read(),
                                           Publish = True)

        except self.lamb.exceptions.ResourceNotFoundException:
            #Lambda not found, create one
            response = self.lamb.create_function(FunctionName = fnName,
                                        Runtime ='python3.6', MemorySize=size,
                                        Role = role['Role']['Arn'], Timeout=900,
                                        Handler = '{}.{}'.format(megaFnFile,function),
                                        Code = {'ZipFile': open(package, 'rb').read()})

        os.system('cd {} && rm -r {}'.format(appDir, package))
        return response


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


    def profile(self, inTime, size=128, useCache=False):
        # For now, one memory size, can do for more, another loop!
        # Keep 1 in the list
        procCounts = [1, 2, 4, 8, 16]
        times = [7]
        ITER = 10

        fileName = "profiler-cache/cache.json"
        out = {}
        # The moment useCache is sent False, earlier content will be deleted!
        if useCache:
            out = common.getJson(fileName)
            # json.dumps makes keys as strings, make do here
            key = str(size)
            if key in out:
                return out[key]

        batchData = self.__batchProfile(size, procCounts, times, ITER)
        # Expected Output structures =>
        # batchData: For each time in times, a dict of different proc in procCount,
        # with corresponding median and 99'ile latencies in milliseconds!
        medianIdx = 0
        factor = 1.4
        vcpus = 1
        val = batchData[times[0]]
        # if there is only 1 vcpu, latency increases linearly as we batch more
        # This code below looks at runtimes and choses a fit accordingly
        # TODO: think of a better way. This heuristic can be a problem when vcpu count
        # is odd or not a multiple of 2?
        prevLatency = val[procCounts[0]][medianIdx]
        for procCount in procCounts:
            curLatency = val[procCount][medianIdx]
            if curLatency < prevLatency * factor:
                # We consider a factor of reduction due to possible interleaving
                # or hyperthreading kind of sharing, but it is still significantly
                # less than double
                vcpus = procCount
            prevLatency = curLatency

        # For the memorySize we have a vpu approximation
        out[size] = vcpus

        # Cache "out" dict, always!
        common.putJson(fileName, out)

        return vcpus


    def register(self, appDir, megaFnFile, function):
        if self.appDir is None:
            self.appDir = appDir
        elif self.appDir != appDir:
            raise Exception('Please register functions only belonging to one app')

        self.functionsToRegister.append([appDir, megaFnFile, function])
        fnName = self.__functionName(megaFnFile, function)
        if self.appName is None or self.stage is None:
            with open('{}/serverless.yml'.format(appDir)) as stream:
                config = yaml.safe_load(stream)
                self.appName = config['service']
                self.stage = config['provider']['stage']

        response = {'FunctionName':'{}-{}-{}'.format(self.appName, self.stage, fnName)}
        return response

    def deploy(self, memorySize=3008):
        deployments = []
        config = {}
        functions = {}
        #Update serverless.yml
        with open('{}/serverless.yml'.format(self.appDir)) as stream:
            config = yaml.safe_load(stream)
            #Register all functions
            for registration in self.functionsToRegister:
                appDir, megaFnFile, function = registration
                fnName = self.__functionName(megaFnFile, function)
                handlerName = self.__handlerName(megaFnFile, function)
                functions.update({fnName: {'handler': handlerName, 'timeout':28,
                'memorySize':memorySize,'events':[{'http':{'path':function, 'method':'get'}}]}})
                response = {'FunctionName':'{}-{}-{}'.format(self.appName, self.stage, fnName)}
                deployments.append(response)

            config['functions'] = functions

        with open('{}/serverless.yml'.format(appDir), 'w') as stream:
            yaml.dump(config, stream)

        #Deploy
        os.system('cd {} && '\
                  'serverless plugin install -n serverless-python-requirements && '\
                  'serverless deploy'.format(appDir))

        return deployments

    def invoke(self, metadata):
        # Use boto3 invoke from https://boto3.amazonaws.com/v1/documentation/api/latest/reference/services/lambda.html#Lambda.Client.invoke
        """INFO: Currently expecting functionName as "name" in metadata, can change that later!
           TODO: Only sync request implemented now! Need to implement async request!
        """
        response = None
        try:
            response = self.lamb.invoke(FunctionName=metadata['name'],
                InvocationType='RequestResponse', Payload=json.dumps(metadata))
        except Exception as e:
            print(str(e))
        return response

    def invokeCode(self, metadata, inputObj, requestsObj):
        code = []
        code.append('client = boto3.client(\'lambda\')')
        code.append('with ThreadPoolExecutor(max_workers=len({})) as executor:'.format(inputObj))
        code.append('\tfor x in range(len({})):'.format(inputObj))
        code.append('\t\t{}.append('.format(requestsObj))
        code.append('\t\t\texecutor.submit(client.invoke,')
        code.append('\t\t\tFunctionName   = \'{}\','.format(metadata['FunctionName']))
        code.append('\t\t\tInvocationType = "RequestResponse",')
        code.append('\t\t\tPayload        = json.dumps({}[x]).encode(\'utf-8\')'.format(inputObj))
        code.append('\t\t\t))')
        return code

    def responseCode(self, outputObj, key, requestsObj):
        code = []
        code.append('results = [ fut.result() for fut in {}]'.format(requestsObj))
        code.append('for result in results:')
        code.append('\tlambdaOut = json.loads(result[\'Payload\'].read().decode(\'utf-8\'))[\'{}\']'.format(key))
        code.append('\tfor output in lambdaOut:')
        code.append('\t\t{}.append(output)'.format(outputObj))
        return code

    def importCode(self):
        #Unzip_requirements is for serverless
        return ['try:', '\timport unzip_requirements','except ImportError:', '\tpass',
                'import boto3', 'from concurrent.futures import ThreadPoolExecutor']

    def inputs(self):
        return ['params','context']
