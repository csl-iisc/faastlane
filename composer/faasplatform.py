"""
abstract FaaS Platform class
"""

class Faasplatform():
    def __init__(self, debug):
        None

    """ Profiling based on function execution time
    inTime  : time (in milliseconds) of user provided function
    size    : container memory limit (in MBs) if needed by the platform
    useCache: use local stored information if True, else force profiler run
    """
    def profile(self, inTime, size, useCache):
        raise NotImplementedError("Platform:profile")

    def register(self, appDir, megaFnFile, function):
        raise NotImplementedError("Platform:register")

    def deploy(self):
        raise NotImplementedError("Platform:deploy")

    def invoke(self, metadata):
        raise NotImplementedError("Platform:invoke")

    def invokeCode(self, metadata):
        raise NotImplementedError("Platform:invokeCode")

    def importCode(self):
        raise NotImplementedError("Platform:importCode")

    def inputs(self):
        raise NotImplementedError("Platform:inputs")
