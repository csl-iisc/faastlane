#define _GNU_SOURCE
#include "Python.h"
#include "marshal.h"
#include "pythread.h"
#include "structmember.h"
#include <float.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mpkobmalloc.h"


int sharedKey;
int orchPrivateKey;
int q;

#ifndef DEBUG_VAR
#define DEBUG_VAR 1  
int DEBUG = 0;
#endif

#ifndef MPK_ENABLED
#define MPK_ENABLED 1
#endif

static PyObject * None()
{
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
    Py_XINCREF(Py_None);
    PyGILState_Release(gstate);
    return Py_None;
}

static PyObject *
pymem_setup_allocators(PyObject *self, PyObject *args)
{
    if(MPK_ENABLED) pkru_set(-1, ENABLE_BOTH);

    int debug, setup;

    if (!PyArg_ParseTuple(args, "i", &debug))
        return NULL;

    DEBUG = debug;
    setup = pymeme_setup_default_allocators();

    return None();
}

static PyObject * rdpkru(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int pkru, sanityCheck;

    asm("mov $0x0, %%ecx\n\t"
	    "RDPKRU\n\t"
        "mov %%eax, %0\n\t"
        "mov %%edx, %1\n\t"
        : "=gm" (pkru), "=gm" (sanityCheck));

    printf("PKRU: %x\n", pkru);
    return None();
}

static PyObject *
pymem_reset_pkru(PyObject *self, PyObject *PyUNUSED(ignored))
{
    unsigned int pkru = 0;
    unsigned int all_rights = 0;

    if(MPK_ENABLED){
        for(int i=1; i<16; i++)
            syscall(SYS_pkey_free, i);
    }

    int retVal = pymem_reset_allocators();
    
    PyThread_delete_key(tssKey);
    return None();
}

static PyObject * 
pymem_reset(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    if(DEBUG) printf("Entered pymem_reset\n");
    int pkey; 
    pkey = get_thread_pkey(0);
    if(DEBUG) printf("Found %d pkey for thread\n", pkey);

    if(MPK_ENABLED) syscall(SYS_pkey_free, pkey);

    if(DEBUG) printf("Reset successfully finished!\n");
    privateOrSharedDomain = false;
    return None();
}

static PyObject *
pkey_thread_mapper(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int pkey = allocate_pkey();
    if(DEBUG) printf("Allocated pkey for thread: %d\n", pkey);
    privateOrSharedDomain = false;
    tssValue[pkey] = pkey;
    int c = PyThread_set_key_value(tssKey, (void *)(&tssValue[pkey]));
    int *p = (int *)PyThread_get_key_value(tssKey);
    
    if (!c && DEBUG) 
        printf("TSS Value pointer: %p, value: %d\n", p,*p);

    //Set pkey value for the thread
    if(MPK_ENABLED){
        pkru_set(pkey, ENABLE_BOTH);
    } 

    return None();
}

static PyObject *
pymem_pkru_set(PyObject *self, PyObject *args)
{
    int key;

    if (!PyArg_ParseTuple(args, "i", &key))
        return NULL;

    if (MPK_ENABLED) pkru_set(key, ENABLE_BOTH);

    return None();
}

static PyObject *
pymem_allocate_from_shmem(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    privateOrSharedDomain = true;
    return None();
}

static PyObject *
pymem_clear_dict(PyObject *self, PyObject *args)
{
    PyObject* dict;

    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &dict))
        return NULL;

    PyObject *key, *value;
    PyObject *zero = PyLong_FromLong(0);
    Py_ssize_t pos = 0;

    while (PyDict_Next(dict, &pos, &key, &value)) {
        memset((void *)value, 0, 8192);
    }

    return None();
}

static PyObject *
pymem_access_address(PyObject *self, PyObject *args)
{
    PyObject* dict;
    int p;

    if (!PyArg_ParseTuple(args, "i", &p))
        return NULL;

    char *ptr;
    ptr = p;

    return None();
}

static PyObject *
create_key(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    tssKey = PyThread_create_key();
    return None();
}

static PyObject *
set_value(PyObject *self, PyObject *args)
{
    int p,c;
    int *d;

    if(!PyArg_ParseTuple(args, "i", &q))
        return NULL;

    c = PyThread_set_key_value(tssKey, (void *)(&q));
    d = (int *)PyThread_get_key_value(tssKey);
    return None();
}

static PyObject *
get_value(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int c;
    c = *((int *)PyThread_get_key_value(tssKey));
    return None();
}

static PyObject *
delete_key(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_delete_key_value(tssKey);
    PyThread_delete_key(tssKey);
    return None();
}

static PyObject *
copy_dict(PyObject *self, PyObject *srcArgs)
{
    PyObject *src, *dest;
    src = PyTuple_GetItem(srcArgs,0);
    dest = PyTuple_GetItem(srcArgs, 1);
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(src, &pos, &key, &value)) {
        PyObject *nKey, *nValue;
        *nKey = *key;
        *nValue = *value;
        PyDict_SetItem(dest, nKey, nValue);
    }
    
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();
    Py_XINCREF(dest);
    PyGILState_Release(gstate);
    return dest;
}

static PyMethodDef mpkMemAllocators[] = {
    {"rdpkru",                      rdpkru,                      METH_NOARGS},
    {"pymem_reset",                 pymem_reset,                 METH_NOARGS},
    {"pymem_setup_allocators",      pymem_setup_allocators,      METH_VARARGS},
    {"pymem_reset_pkru",            pymem_reset_pkru,            METH_NOARGS},
    {"pymem_pkru_set",              pymem_pkru_set,              METH_VARARGS},
    {"pymem_allocate_from_shmem",   pymem_allocate_from_shmem,   METH_NOARGS},
    {"pkey_thread_mapper",          pkey_thread_mapper,          METH_NOARGS},
    {"pymem_clear_dict",            pymem_clear_dict,            METH_VARARGS},
    {"pymem_access_address",        pymem_access_address,        METH_VARARGS},
    {"get_value",                   get_value,                   METH_NOARGS},
    {"set_value",                   set_value,                   METH_VARARGS},
    {"create_key",                  create_key,                  METH_NOARGS},
    {"delete_key",                  delete_key,                  METH_NOARGS},
    {"copy_dict",                   copy_dict,                   METH_VARARGS},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef mpkmemallocmodule = {
    PyModuleDef_HEAD_INIT,
    "mpkmemalloc",
    "Python interface for custom MPK memory allocators",
    -1,
    mpkMemAllocators
};

PyMODINIT_FUNC PyInit_mpkmemalloc(void) {
    return PyModule_Create(&mpkmemallocmodule);
}
