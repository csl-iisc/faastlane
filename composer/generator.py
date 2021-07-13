import argparse
import math
import json
import sys
import pdb
import os

FAASTLANE_WORK_DIR = '{}/.faastlane'.format('/'.join(os.path.realpath(__file__).split('/')[:-1]))
sys.path = ['.', FAASTLANE_WORK_DIR] + sys.path
SUPPORTED_PLATFORMS = ['aws', 'ow']

def parseWorkflowJSON():
    #parse json
    try:
        return json.loads(open('{}/workflow.json'.format(FAASTLANE_WORK_DIR)).read())
    except IOError:
        raise "workflow.json not provided in the package!"

def getTaskFnMap(workflow):
    taskFnMap = {}
    states = workflow['States']
    for state, defn in states.items():
        if defn['Type'] == 'Task':
            taskFnMap[state] = defn['Resource']
        if defn['Type'] == 'Parallel':
            for branch in defn['Branches']:
                taskFnMap.update(getTaskFnMap(branch))
        if defn['Type'] == 'Map':
            taskFnMap.update(getTaskFnMap(defn['Iterator']))
    return taskFnMap

def importModules(taskFnMap, platform, mpk):
    #Tasks return dicts and parallel states return array
    imports = []
    generic = ['import threading',
               'import multiprocessing as mp',
               'import json',
               'import math']

    imports.extend(generic)

    #Platform specific imports, needed for generating MapState
    imports.extend(platform.importCode())

    #import functions
    fns = list(set(taskFnMap.values()))
    for fn in fns:
        imports.append('from {} import main as {}'.format(fn, fn))

    #Import faastlane's memory allocator when enabled
    if mpk:
        imports.append('from mpkmemalloc import *')

    importBlock = '\n'.join(imports)
    return importBlock

def indentCode(loc, level):
    indent = ''
    for l in range(level):
        indent += '\t'

    lines = [indent + l for l in loc]

    return '\n'.join(lines)

def getTaskOutDict(fn):
    return '{}Out'.format(fn)

def getWorkerName(fn):
    return '{}Wrapper'.format(fn)

def getLaneName(fn):
    return '{}Lane'.format(fn)

def getLocalOrchName(wf):
    return 'localOrchAt{}'.format(wf['StartAt'])

def getMainOrchName(state):
    return 'orchAt{}'.format(state)

def createOutputDicts(workflow):
    outputs = []
    states = workflow['States']
    for state, defn in states.items():
        if defn['Type'] == 'Task':
            outputs.append('{} = {{}}'.format(getTaskOutDict(state)))
        elif defn['Type'] == 'Parallel':
            outputs.append('{} = []'.format(getTaskOutDict(state)))
            for branch in defn['Branches']:
                outputs.append(createOutputDicts(branch))
        elif defn['Type'] == 'Map':
            outputs.append('{} = []'.format(getTaskOutDict(state)))
            outputs.append(createOutputDicts(defn['Iterator']))

    outBlock = '\n'.join(outputs)
    return outBlock

def identifyStartingTasks(stateName,workflow):
    resources = []
    state = workflow['States'][stateName]

    if state['Type'] == 'Task':
        return [stateName]
    elif state['Type'] == 'Choice':
        for choiceState in state['Choices']:
            resources.extend(identifyStartingTasks(choiceState['Next'], workflow))
        #Input should flow into default state too
        resources.extend(identifyStartingTasks(state['Default'], workflow))
    elif state['Type'] == 'Parallel':
        for branch in state['Branches']:
            resources.extend(identifyStartingTasks(branch['StartAt'],branch))
    elif state['Type'] == 'Map':
        branch = state['Iterator']
        resources.extend(identifyStartingTasks(branch['StartAt'],branch))

    return resources

def getEndState(workflow):
    states = workflow['States']
    for stateName, state in states.items():
        if 'End' in state and state['End'] == True:
            return {'stateName': stateName, 'state': state}

def generateStateInputsMap(taskFnMap, workflow, workflowInput):
    #Identify first function, event will be the input
    start  = workflow['StartAt']
    states = workflow['States']

    #Intialize map, function can have multiple inputs
    taskInputMap = {}
    choiceInputMap   = {}
    parallelInputMap = {}

    startState = states[start]

    for stateName, state in states.items():
        if state['Type'] != 'Choice':
            stateOut     = getTaskOutDict(stateName)
            if 'Next' in state:
                nextStateName = state['Next']
                resources     = identifyStartingTasks(nextStateName, workflow)
                for resource in resources:
                    taskInputMap[resource] = stateOut
                if workflow['States'][nextStateName]['Type'] == 'Choice':
                    choiceInputMap[nextStateName] = stateOut
                if workflow['States'][nextStateName]['Type'] in ['Parallel', 'Map']:
                    parallelInputMap[nextStateName] = stateOut

                    if workflow['States'][nextStateName]['Type'] == 'Parallel':
                        subworkflows = states[nextStateName]['Branches']
                    else:
                        subworkflows = [states[nextStateName]['Iterator']]

                    for sub in subworkflows:
                        itaskInputMap, ichoiceInputMap, iparallelInputMap = generateStateInputsMap(taskFnMap, sub, stateOut)
                    taskInputMap.update(itaskInputMap)
                    choiceInputMap.update(ichoiceInputMap)
                    parallelInputMap.update(iparallelInputMap)
    #The starting functions in the workflow should receive the input event from user
    workflowStartResources = identifyStartingTasks(start, workflow)
    for resource in workflowStartResources:
        taskInputMap[resource] = workflowInput

    # Case: Workflow starts with a choice
    if workflow['States'][start]['Type'] == 'Choice':
        choiceInputMap[start] = workflowInput

    if workflow['States'][start]['Type'] in ['Parallel', 'Map']:
        parallelInputMap[start] = workflowInput

    return taskInputMap, choiceInputMap, parallelInputMap

def generateTaskWorkerBlock(task, fn, taskInput, mpk):
    #Fetch all the required shared memory inputs
    getIn = 'global {}'.format(taskInput)

    #Input string for fn, if multiple inputs, send array
    fnIn = '{}'.format(taskInput)

    #Execute fn
    fnexec = 'result = {}({})'.format(fn, fnIn)

    # Fetch shared memory output
    fnOut = getTaskOutDict(task)
    getOut = 'global {}'.format(fnOut)

    # Assign result to shared memory output
    putOut = '{} = result'.format(fnOut)

    # MPK Thread Gates
    mpkGateIn  = 'pkey_thread_mapper()'
    mpkGateOut = 'pymem_reset()'

    fnHeader = 'def {}():'.format(getWorkerName(task))

    if mpk:
        fnDefn   = '\n\t'.join([mpkGateIn, getIn, fnexec, getOut, putOut, mpkGateOut])
    else:
        fnDefn   = '\n\t'.join([getIn, fnexec, getOut, putOut])

    return '\n\t'.join([fnHeader, fnDefn])

def generateTaskWorkers(taskFnMap, taskInputMap, workflow, mpk):
    workerBlocks = []
    for task, inputs in taskInputMap.items():
        workerBlocks.append(generateTaskWorkerBlock(task, taskFnMap[task], inputs, mpk))

    return '\n\n'.join(workerBlocks)

def generateParallelOrchestrator(stateName, taskInputMap, choiceInputMap, parallelInputMap, workflow, platform):
    lanes = []
    names = []
    execs = []
    code  = []

    fnHeader = 'def {}({}):'.format(stateName, ','.join(platform.inputs()))

    #Gather input
    inputObj = parallelInputMap[stateName]
    code.append('global {}'.format(inputObj))
    code.append('{} = params[\'faastlane-input\']'.format(inputObj))
    #Register Worker
    code.append('parent_conns = []')
    code.append('lanes = []')

    for branch in workflow['Branches']:
        laneName = getLaneName(getLocalOrchName(branch))
        lanes.append('parent_conn, child_conn = mp.Pipe()')
        lanes.append('parent_conns.append(parent_conn)')
        lanes.append('lanes.append(mp.Process(target={}, args=[{}, child_conn]))'.format(getLocalOrchName(branch), inputObj))
        names.append(laneName)

    execs.append('if \'launchMap\' in params:')
    execs.append("\tlanes = lanes[params['launchMap']['start']:params['launchMap']['end']]")
    execs.append("\tparent_conns = parent_conns[params['launchMap']['start']:params['launchMap']['end']]")

    execs.append('for lane in lanes:')
    execs.append('\tlane.start()')

    execs.append('for lane in lanes:')
    execs.append('\tlane.join()')

    code.extend(lanes)
    code.extend(execs)

    outputObj = getTaskOutDict(stateName)

    #Bring output into scope
    code.append('global {}'.format(outputObj))

    #(Re)Initialize it, since its a global list
    code.append('{} = []'.format(outputObj))

    #Update the output with responses from sub-workflows
    code.append('for parent_conn in parent_conns:')
    code.append('\t{}.append(parent_conn.recv())'.format(outputObj))
    code.append('return {{\'faastlane-output\':{}}}'.format(outputObj))

    fnDefn = indentCode(code, 1)

    return '\n'.join([fnHeader, fnDefn])

def generateParallelWorker(stateName, state, parallelInputMap, platform, megaFunctionFile):
    metadata = platform.register(FAASTLANE_WORK_DIR, 'megaFunction',
                               stateName)

    #Retrieve Map Input
    code = []
    inputObj = parallelInputMap[stateName]
    code.append('global {}'.format(inputObj))

    #TODO: Identify available parallelism within the container here
    withinContainer = 2
    parallelLegs = len(state['Branches'])

    #Code generation begins
    fnHeader = 'def {}():'.format(getWorkerName(stateName))

    #TODO: Make this static
    numProcesses = min(withinContainer, parallelLegs)
    numContainers = max(math.ceil((parallelLegs-withinContainer)/withinContainer), 0)

    #Gather input and output
    outputObj = getTaskOutDict(stateName)
    code.append('global {}'.format(outputObj))
    #(Re)Initialize it, since its a global list

    code.append('parallelInputs=[]')
    code.append('futs=[]')
    code.append('for i in range(1,{}):'.format(numContainers+1))
    code.append("\tparallelInputs.append({{'faastlane-input':{}, 'launchMap':{{'start':{}*i, 'end':min({}*(i+1),{})}}}})".format(inputObj, withinContainer, withinContainer, parallelLegs))

    code.append('if(len(parallelInputs)):')
    code.extend(indentCode(platform.invokeCode(metadata, 'parallelInputs', 'futs'), 1).split('\n'))
    code.append("outputs = {}({{'faastlane-input':{}, 'launchMap':{{'start':0, 'end':{}}}}}{})".format(
        stateName, inputObj, numProcesses, ''.join([', {}' for i in range(len(platform.inputs())-1)])))
    code.append('{} = []'.format(outputObj))
    code.append("for output in outputs['faastlane-output']:")
    code.append('\t{}.append(output)'.format(outputObj))
    code.append('if(len(parallelInputs)):')
    code.extend(indentCode(platform.responseCode(outputObj,'faastlane-output', 'futs'), 1).split('\n'))
    # pdb.set_trace()

    fnDefn = indentCode(code, 1)
    workerCode = '\n'.join([fnHeader, fnDefn])

    #Write after every map worker to handle nested
    megaFunctionFile.write(workerCode)
    megaFunctionFile.write('\n')
    megaFunctionFile.flush()
    os.fsync(megaFunctionFile.fileno())
    return workerCode

def generateMapWorker(stateName, state, parallelInputMap, platform, megaFunctionFile):
    metadata = platform.register(FAASTLANE_WORK_DIR, 'megaFunction',
                               getMainOrchName(state['Iterator']['StartAt']))

    #Retrieve Map Input
    code = []
    inputObj = parallelInputMap[stateName]
    code.append('global {}'.format(inputObj))

    if 'ItemsPath' in state:
        inputObj = resolveMapItemsPath(inputObj, state['ItemsPath'])

    #TODO: Identify available parallelism within the container here
    withinContainer = 1

    #Code generation begins
    fnHeader = 'def {}():'.format(getWorkerName(stateName))

    #Identify number of processes and lambdas to run
    code.append('numProcesses = min({}, len({}))'.format(withinContainer, inputObj))
    code.append('numContainers = max(math.ceil((len({})-{})/{}), 0)'.format(inputObj, withinContainer, withinContainer))

    #Gather input and output
    outputObj = getTaskOutDict(stateName)
    code.append('global {}'.format(outputObj))
    code.append('mapInputs=[]')
    code.append('futs=[]')
    code.append('for i in range(1,numContainers+1):')
    code.append('\tmapInputs.append({{\'faastlane-input\':{}[{}*i:min({}*(i+1),len({}))]}})'.format(inputObj, withinContainer, withinContainer, inputObj))

    code.append('if(len(mapInputs)):')
    code.extend(indentCode(platform.invokeCode(metadata, 'mapInputs', 'futs'), 1).split('\n'))
    code.append('outputs = {}({{\'faastlane-input\':{}[:numProcesses]}}{})'.format(
        getMainOrchName(state['Iterator']['StartAt']), inputObj, ''.join([', {}' for i in range(len(platform.inputs())-1)])))
    code.append('{} = []'.format(outputObj))
    code.append("for output in outputs['faastlane-output']:")
    code.append('\t{}.append(output)'.format(outputObj))
    code.append('if(len(mapInputs)):')
    code.extend(indentCode(platform.responseCode(outputObj,'faastlane-output', 'futs'), 1).split('\n'))

    fnDefn = indentCode(code, 1)
    workerCode = '\n'.join([fnHeader, fnDefn])

    #Write after every map worker to handle nested
    megaFunctionFile.write(workerCode)
    megaFunctionFile.write('\n')
    megaFunctionFile.flush()
    os.fsync(megaFunctionFile.fileno())
    return workerCode

def generateParallelWorkers(workflow, parallelInputMap, platform, megaFunctionFile):
    #Parallel workers wrap a fn that either launches threads or processes for the parallel part
    #Each branch in a parallel state is a workflow, so each thread/process runs an orchestrator within
    workers = []
    for stateName, state in workflow['States'].items():
        if state['Type'] == 'Parallel':
            for branch in state['Branches']:
                workers.append(generateParallelWorkers(branch, parallelInputMap, platform, megaFunctionFile))
            workers.append(generateParallelWorker(stateName, state, parallelInputMap, platform, megaFunctionFile))

    return '\n'.join(workers)

def generateMapWorkers(workflow, parallelInputMap, platform, megaFunctionFile):
    workers = []
    for stateName, state in workflow['States'].items():
        if state['Type'] == 'Map':
            workers.append(generateMapWorkers(state['Iterator'],
                                              parallelInputMap, platform, megaFunctionFile))
            workers.append(generateMapWorker(stateName, state,
                                             parallelInputMap, platform, megaFunctionFile))

    return '\n'.join(workers)

def createLanes(workflow):
    #A lane can either be a task worker or a parallel state worker
    lanes = []
    states = workflow['States']
    for stateName, state in states.items():
        if state['Type'] == 'Task' or state['Type'] == 'Parallel' or state['Type'] == 'Map':
            lanes.append('{} = threading.Thread(target={})'.format(getLaneName(stateName),
                                                                   getWorkerName(stateName)))
    return indentCode(lanes,1)

def executeLane(laneName, level):
    start  = '{}.start()'.format(laneName)
    finish = '{}.join()'.format(laneName)

    return indentCode([start,finish], level)

def returnStatement(out, isMain, l):
    returnBlock = []
    if not isMain:
        returnBlock.append('conn.send({})'.format(out))
        returnBlock.append('conn.close()'.format(out))
    returnBlock.append('return {}'.format(out))
    return indentCode(returnBlock, l)

def emptyElse(l):
    return indentCode(['else:'],l)

def resolveChoiceCondition(choice, choiceInput, l):
    toks = choice['Variable'].replace('$', choiceInput).split('.')
    obj = toks[0]
    fields = toks[1:]
    lhs = '{}{}'.format(obj,''.join(['[\'{}\']'.format(f) for f in fields]))

    for k, v in choice.items():
        if 'Equals' in k:
            rhs = v
            op  = '=='
            break

    condition = 'if {} {} {}:'.format(lhs, op, rhs)
    return indentCode([condition], l)

def resolveMapItemsPath(choiceInput, itemsPath):
    toks = itemsPath.replace('$', choiceInput).split('.')
    obj = toks[0]
    fields = toks[1:]
    return '{}{}'.format(obj,''.join(['[\'{}\']'.format(f) for f in fields]))

def createExecutionBlocks(workflow, taskInputMap, choiceInputMap, startStateName, isMain, level):
    states = workflow['States']
    executionBlocks = []
    cState = states[startStateName]
    cStateName = startStateName
    while cState is not None:
        if cState['Type'] == 'Choice':
            #Generate code for each choice
            for choice in cState['Choices']:
                executionBlocks.append(resolveChoiceCondition(choice,
                                                          choiceInputMap[cStateName], level))
                executionBlocks.extend(createExecutionBlocks(workflow, taskInputMap,
                                                             choiceInputMap, choice['Next'],
                                                             isMain, level+1))
            #Default choice
            executionBlocks.append(emptyElse(level))
            executionBlocks.extend(createExecutionBlocks(workflow, taskInputMap, choiceInputMap,
                                                         cState['Default'], isMain, level+1))
            cState = None
        else:
            # print('Recurring here?')
            executionBlocks.append(executeLane(getLaneName(cStateName), level))
            if 'Next' in cState:
                cStateName = cState['Next']
                cState = states[cStateName]
            elif 'End' in cState and cState['End'] == True:
                #Append ret statement here
                out = getTaskOutDict(cStateName)
                executionBlocks.append(returnStatement(out, isMain, level))
                cState = None

    return executionBlocks

def getOrchInput(taskInputMap, choiceInputMap, parallelInputMap, workflow):
    # Get inputs to the starting fn(s) in the orchestrator
    startStateName = workflow['StartAt']
    state = workflow['States'][startStateName]

    inputMap = {
        'Task':     taskInputMap,
        'Choice':   choiceInputMap,
        'Parallel': parallelInputMap
    }

    return inputMap.get(state['Type'])[startStateName]

def generateOrchestrator(taskInputMap, choiceInputMap, parallelInputMap, workflow, platform, isMain):
    orchCode = []
    orchInput = getOrchInput(taskInputMap, choiceInputMap, parallelInputMap, workflow)
    if isMain:
        orchCode.append('def {}({}):'.format(getMainOrchName(workflow['StartAt']), ','.join(platform.inputs())))
    else:
        orchCode.append('def {}(params, conn):'.format(getLocalOrchName(workflow)))
    orchCode.append(indentCode(['global {}'.format(orchInput)], 1))
    orchCode.append(indentCode(['{} = params'.format(orchInput)], 1))

    lanesBlock = createLanes(workflow)
    orchCode.append(lanesBlock)

    #Worker starts and joins
    executionBlocks = createExecutionBlocks(workflow, taskInputMap, choiceInputMap,
                                            workflow['StartAt'], isMain, 1)
    orchCode.extend(executionBlocks)

    #Happy ending!
    return '\n'.join(orchCode)

def generateMapOrchestrator(stateName, taskInputMap, choiceInputMap, parallelInputMap, workflow, platform):
    orchInput = getOrchInput(taskInputMap, choiceInputMap, parallelInputMap, workflow)
    outputObj = getTaskOutDict(stateName)
    fnHeader = 'def {}({}):'.format(getMainOrchName(workflow['StartAt']), ','.join(platform.inputs()))

    code = []
    code.append('global {}'.format(orchInput))
    code.append('{} = params[\'faastlane-input\']'.format(orchInput))

    #Spawn as many processes as available parallelism
    code.append('processes = []')
    code.append('parent_conns = []')
    #Bring output into scope
    code.append('global {}'.format(outputObj))
    #(Re)Initialize it, since its a global list
    code.append('{} = []'.format(outputObj))
    code.append('for i in range(len({})):'.format(orchInput))
    code.append('\tparent_conn, child_conn = mp.Pipe()')
    code.append('\tparent_conns.append(parent_conn)')
    code.append('\tprocesses.append(mp.Process(target={}, args=[{}[i], child_conn]))'.format(getLocalOrchName(workflow), orchInput))
    code.append('for process in processes:')
    code.append('\tprocess.start()')
    #join processes
    code.append('for process in processes:')
    code.append('\tprocess.join()')
    #Update the output with responses from sub-workflows
    code.append('for parent_conn in parent_conns:')
    code.append('\t{}.append(parent_conn.recv())'.format(outputObj))
    code.append('return {{\'faastlane-output\':{}}}'.format(outputObj))
    fnDefn = indentCode(code, 1)

    #Happy ending!
    return '\n'.join([fnHeader, fnDefn])

def generateAllOrchestrators(taskInputMap, choiceInputMap,parallelInputMap, workflow, platform, isMain):
    #A workflow can have multiple orchestrators, for example, in a parallel state, each branch is a workflow
    #We have to write an orchestrator for each of the workflows
    orchs = []
    for stateName, state in workflow['States'].items():
        if state['Type'] == 'Parallel':
            orchs.append(generateParallelOrchestrator(stateName, taskInputMap, choiceInputMap,
                                                      parallelInputMap, state, platform))
            for branch in state['Branches']:
                orchs.extend(generateAllOrchestrators(taskInputMap, choiceInputMap,
                                                      parallelInputMap, branch, platform, 0))
        elif state['Type'] == 'Map':
            #Map State should create 2 orchs, one for local, one for remote execution
            orchs.append(generateMapOrchestrator(stateName, taskInputMap, choiceInputMap,
                                                      parallelInputMap, state['Iterator'], platform))
            orchs.extend(generateAllOrchestrators(taskInputMap, choiceInputMap,
                                                      parallelInputMap, state['Iterator'], platform, 0))

    orchs.append(generateOrchestrator(taskInputMap, choiceInputMap, parallelInputMap,
                                      workflow, platform, isMain))
    return orchs

def orchPlug(mpk):
    # Adhoc orchestrator requirements
    plug = ['event = {}']
    return '\n'.join(plug)

def generateMain(workflow, mpk, platform):
    #Platform dependent
    header = 'def main(userInput):'

    uInput = 'event=userInput'
    inputs = ['{}' for x in range(len(platform.inputs()))]
    execution = 'result = {}({})'.format(getMainOrchName(workflow['StartAt']), ', '.join(inputs))
    ret = 'return result'

    #MPK stuff
    mpkInit = 'pymem_setup_allocators(0)'
    mpkExit = 'pymem_reset_pkru()'

    mainBlock = [uInput, execution, ret]

    if mpk:
        mainBlock.insert(0, mpkInit)
        mainBlock.insert(len(mainBlock)-2, mpkExit)

    body = '\n\t'.join(mainBlock)
    return '\n\t'.join([header, body])

def generateRunner():
    runner = 'if __name__ == "__main__":\n\tprint(main({}))'
    return runner

def setupWorkingDir(inputDir):
    #Clean the working dir
    out = os.system('mkdir -p {} && cd {} && rm -r *'.format(FAASTLANE_WORK_DIR, FAASTLANE_WORK_DIR))
    out = os.system('cd {} && cp -r * {}'.format(inputDir, FAASTLANE_WORK_DIR))

def generateMegaFunction(args):
    setupWorkingDir(args.input)
    platform = getPlatform(args.platform)
    workflow = parseWorkflowJSON()

    #List all the functions in the workflow
    taskFnMap = getTaskFnMap(workflow)

    #Import all functions in the workflow
    importBlock = importModules(taskFnMap, platform, args.mpk)

    adhocBlock  = orchPlug(args.mpk)

    #Create global output dictionaries for each function
    outBlock    = createOutputDicts(workflow)

    #Identify inputs that should be passed to each task
    taskInputMap, choiceInputMap, parallelInputMap = generateStateInputsMap(taskFnMap, workflow, 'event')

    #Generate task workers
    workerBlocks = generateTaskWorkers(taskFnMap, taskInputMap, workflow, args.mpk)

    #Generate Orchestrator
    orchestrators = generateAllOrchestrators(taskInputMap, choiceInputMap, parallelInputMap,
                                             workflow, platform, 1)

    #Generate Main fn
    mainBlock = generateMain(workflow, args.mpk, platform)

    #Generate Main fn
    runnerBlock = generateRunner()

    f = open('{}/megaFunction.py'.format(FAASTLANE_WORK_DIR), 'w')
    f.writelines(importBlock)
    f.write('\n')
    f.writelines(adhocBlock)
    f.write('\n')
    f.writelines(outBlock)
    f.write('\n\n')
    f.writelines(workerBlocks)
    f.write('\n\n')
    f.writelines('\n\n'.join(orchestrators))
    f.write('\n\n')
    f.writelines(mainBlock)
    f.write('\n\n')
    f.flush()
    os.fsync(f.fileno())

    generateMapWorkers(workflow, parallelInputMap, platform, f)
    generateParallelWorkers(workflow, parallelInputMap, platform, f)

    f.writelines(runnerBlock)
    f.write('\n')
    f.close()

    metadata = platform.register(FAASTLANE_WORK_DIR, 'megaFunction',
                               getMainOrchName(workflow['StartAt']))
    return platform.deploy()

def getArgs():
    """Parse commandline."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True,  help="Input dir containing fns and workflow")
    parser.add_argument("--platform",
                        choices=SUPPORTED_PLATFORMS, default = 'aws', help="platform to deploy")
    parser.add_argument("--mpk", type=int, default=0, help="Use MPK for thread protection")
    args = parser.parse_args()
    return args

def getPlatform(platform):
    if platform == "aws":
        from aws import AWS
        platform = AWS()
    elif platform == "ow":
        from openwhisk import Openwhisk
        platform= Openwhisk()
    else:
        raise ValueError("unknown platform: " + platform)
    return platform

if __name__=="__main__":
    args = getArgs()
    generateMegaFunction(args)
