import json

PROFILER_APPDIR = "/mnt/DATA1/ajayn/shm/profiler/faastlane/composer/profiler"
PROFILER_FUNC_NAME = "main"
PROFILER_HANDLER = "handler"


def getJson(fileName):
    content = None
    try:
        f = open(fileName, "r")
        content = json.loads(f.read())
        f.close()
    except Exception as e:
        pass
    return content


def putJson(fileName, content):
    f = open(fileName, "w")
    f.write(json.dumps(content))
    f.close()


def getStats(data):
    data.sort()
    items = len(data)
    median = data[int(0.5*items)]
    later = data[int(0.99*items)]
    return (median, later)
