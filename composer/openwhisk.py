import os
import json
import pdb
import subprocess
import faasplatform

class Openwhisk(faasplatform.Faasplatform):
    def __init__(self, debug=False):
        APIHOST = 'https://172.17.0.1'
        self.WSK_PATH = "/usr/bin/wsk"
        AUTH_KEY = subprocess.check_output(self.WSK_PATH + " property get --auth", shell=True).split()[2]
        AUTH_KEY = AUTH_KEY.decode("utf-8")
        self.user_pass = AUTH_KEY.split(':')
        NAMESPACE = '_'
        RESULT = 'false'
        self.action_url = APIHOST + '/api/v1/namespaces/' + NAMESPACE + '/actions/'
        self.default_action_kind = 'python:3'
        self.version_map = {'python:3':'python3.6', 'python:ai':'python3.5'}
        self.functionsToRegister = []
        self.debug = debug

    def __functionName(self, megaFnFile, function):
        return '{}-{}'.format(megaFnFile, function)

    def __deploy(self, appDir, megaFnFile, function):
        fnName = self.__functionName(megaFnFile, function)

        #function should be main() in __main__.py, for OW to use it as starting point
        os.system('cd {} && cp {}.py __main__.py'.format(appDir, megaFnFile))
        os.system("cd {} && sed -i \'s/def main/def megaMain/g\' __main__.py".format(appDir))
        os.system("cd {} && sed -i \'s/def {}/def main/g\' __main__.py".format(appDir, function))

        #Install Faastlane requirements
        #Check if faastlane.json exists, choose action_kind accordingly
        configFile = '{}/faastlane.json'.format(appDir)
        config = {}
        actionKind = self.default_action_kind

        if os.path.exists(configFile):
            config = json.loads(open(configFile,'r').read())
            if 'kind' in config:
                actionKind = config['kind']

        #Fetch python version based on the action kind
        pyVersion = self.version_map[actionKind]

        #Check if virtualenv exists?
        venv = '{}/virtualenv'.format(appDir)
        # pdb.set_trace()
        if os.path.exists(venv):
            #activate and install requests_futures
            os.system('cd {} && . virtualenv/bin/activate &&'\
                      'pip install requests_futures'\
                      'deactivate'.format(venv))
        else:
            cmd = 'cd {} && virtualenv -p {} virtualenv &&'\
              ' . virtualenv/bin/activate &&'\
              ' pip install requests_futures &&'\
              ' deactivate'.format(appDir, pyVersion)
            os.system(cmd)

        #Install any requirements if venv was not provided
        requirements = '{}/requirements.txt'.format(appDir)
        if os.path.exists(requirements):
            os.system('cd {} && . virtualenv/bin/activate &&'\
                      'pip install -r requirements.txt'.format(appDir))


        #Zip the application
        os.system("cd {} && zip -r {}.zip *".format(appDir, fnName))

        #TODO: action kind is default?
        create = subprocess.check_output('cd {} && {} -i action update {} {}.zip --kind {}'.format(appDir, self.WSK_PATH, fnName, fnName, actionKind), shell=True)

        #Revert back temporary changes
        os.system("cd {} && sed -i \'s/def main/def {}/g\' __main__.py".format(appDir, function))
        os.system("cd {} && sed -i \'s/def megaMain/def main/g\' __main__.py".format(appDir))
        metadata = {'functionName':fnName}
        return metadata

    def profile(self, inTime, size=128, useCache=False):
        print('Hello! Profiling comes here')

    def register(self, appDir, megaFnFile, function):
        self.functionsToRegister.append([appDir, megaFnFile, function])
        fnName = self.__functionName(megaFnFile, function)
        return {'functionName': fnName}

    def deploy(self):
        deployments = []
        for registration in self.functionsToRegister:
            appDir, megaFnFile, function = registration
            deployments.append(self.__deploy(appDir, megaFnFile, function))
        return deployments

    def invoke(self, metadata):
        raise NotImplementedError("Platform:invoke")

    def invokeCode(self, metadata, inputObj, requestsObj):
        action = metadata['functionName']
        url = self.action_url + action
        code = []
        code.append('session = FuturesSession(max_workers=len({}))'.format(inputObj))
        code.append('for i in range(len({})):'.format(inputObj))
        code.append('\t{}.append(session.post(\'{}\', params={{\'blocking\': True, \'result\': False}}, auth=(\'{}\',\'{}\'), json={}[i], verify=False))'.format(requestsObj, url, self.user_pass[0], self.user_pass[1], inputObj))
        return code

    def responseCode(self, outputObj, key, requestsObj):
        code = []
        code.append('for future in {}:'.format(requestsObj))
        code.append('\tlambdaOut=json.loads(future.result().content.decode())')
        code.append('\tfor output in lambdaOut[\'response\'][\'result\'][\'{}\']:'.format(key))
        code.append('\t\t{}.append(output)'.format(outputObj))
        return code


    def importCode(self):
        return ['from requests_futures.sessions import FuturesSession']

    def inputs(self):
        return ['params']
