# Faastlane: Accelerating Function-as-a-Service Workflows
Faastlane is a FaaS tool that accelerates workflows by minimizing function interaction latency. The key contributor to this latency is the transient state that needs to be shared amongst functions within a single workflow instance. Arguments passed between functions in a workflow instance are examples of such transient state. Faastlane strives to execute functions of a workflow instance as separate threads within a single process.  Functions communicate simply via load/store instructions, thus minimizing function interaction latency. The shared virtual address space of the encompassing process provides the simplest form of namespace for sharing state — it does not necessitate a new API. To ensure security, each thread executes in its own memory domain and each domain is protected using Intel's MPK (Memory Protection Keys).

## Folder Structure
### mpk-memalloc-module 
This folder contains a custom memory allocator that allocates from different memory pools based on the thread id of the incoming memory request. Each thread is assigned a protection key (pkey) and all pages serving requests for that thread are protected with the corresponding pkey.

### composer 
This folder contains Faastlane's client-side composer. The composer generates an orchestration function that provides an illusion of single function to a FaaS platform.

Usage:
python3.6 generator.py workflow.json

### python36venv
Virtualenv that contains a working version of the memory allocator module.

### utils
Util scripts to test memory allocator 
