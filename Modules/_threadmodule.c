/* Thread module */
/* Interface to Sjoerd's portable C thread library */

#include "Python.h"
#include "pycore_interp.h"        // _PyInterpreterState.threads.count
#include "pycore_lock.h"
#include "pycore_moduleobject.h"  // _PyModule_GetState()
#include "pycore_modsupport.h"    // _PyArg_NoKeywords()
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"       // _PyThreadState_SetCurrent()
#include "pycore_sysmodule.h"     // _PySys_GetAttr()
#include "pycore_time.h"          // _PyTime_FromSeconds()
#include "pycore_weakref.h"       // _PyWeakref_GET_REF()

#include <stddef.h>               // offsetof()
#ifdef HAVE_SIGNAL_H
#  include <signal.h>             // SIGINT
#endif

// ThreadError is just an alias to PyExc_RuntimeError
#define ThreadError PyExc_RuntimeError

// Forward declarations
static struct PyModuleDef thread_module;

// Module state
typedef struct {
    PyTypeObject *excepthook_type;
    PyTypeObject *lock_type;
    PyTypeObject *local_type;
    PyTypeObject *local_dummy_type;
    PyTypeObject *thread_handle_type;

    // Linked list of handles to all non-daemon threads created by the
    // threading module. We wait for these to finish at shutdown.
    struct llist_node shutdown_handles;
} thread_module_state;

static inline thread_module_state*
get_thread_state(PyObject *module)
{
    void *state = _PyModule_GetState(module);
    assert(state != NULL);
    return (thread_module_state *)state;
}

// _ThreadHandle type

// Handles state transitions according to the following diagram:
//
//     NOT_STARTED -> STARTING -> RUNNING -> DONE
//                       |                    ^
//                       |                    |
//                       +----- error --------+
typedef enum {
    THREAD_HANDLE_NOT_STARTED = 1,
    THREAD_HANDLE_STARTING = 2,
    THREAD_HANDLE_RUNNING = 3,
    THREAD_HANDLE_DONE = 4,
} ThreadHandleState;

// A handle to wait for thread completion.
//
// This may be used to wait for threads that were spawned by the threading
// module as well as for the "main" thread of the threading module. In the
// former case an OS thread, identified by the `os_handle` field, will be
// associated with the handle. The handle "owns" this thread and ensures that
// the thread is either joined or detached after the handle is destroyed.
//
// Joining the handle is idempotent; the underlying OS thread, if any, is
// joined or detached only once. Concurrent join operations are serialized
// until it is their turn to execute or an earlier operation completes
// successfully. Once a join has completed successfully all future joins
// complete immediately.
//
// This must be separately reference counted because it may be destroyed
// in `thread_run()` after the PyThreadState has been destroyed.
typedef struct {
    struct llist_node node;  // linked list node (see _pythread_runtime_state)

    // linked list node (see thread_module_state)
    struct llist_node shutdown_node;

    // The `ident`, `os_handle`, `has_os_handle`, and `state` fields are
    // protected by `mutex`.
    PyThread_ident_t ident;
    PyThread_handle_t os_handle;
    int has_os_handle;

    // Holds a value from the `ThreadHandleState` enum.
    int state;

    PyMutex mutex;

    // Set immediately before `thread_run` returns to indicate that the OS
    // thread is about to exit. This is used to avoid false positives when
    // detecting self-join attempts. See the comment in `ThreadHandle_join()`
    // for a more detailed explanation.
    PyEvent thread_is_exiting;

    // Serializes calls to `join` and `set_done`.
    _PyOnceFlag once;

    Py_ssize_t refcount;
} ThreadHandle;

static inline int
get_thread_handle_state(ThreadHandle *handle)
{
    PyMutex_Lock(&handle->mutex);
    int state = handle->state;
    PyMutex_Unlock(&handle->mutex);
    return state;
}

static inline void
set_thread_handle_state(ThreadHandle *handle, ThreadHandleState state)
{
    PyMutex_Lock(&handle->mutex);
    handle->state = state;
    PyMutex_Unlock(&handle->mutex);
}

static PyThread_ident_t
ThreadHandle_ident(ThreadHandle *handle)
{
    PyMutex_Lock(&handle->mutex);
    PyThread_ident_t ident = handle->ident;
    PyMutex_Unlock(&handle->mutex);
    return ident;
}

static int
ThreadHandle_get_os_handle(ThreadHandle *handle, PyThread_handle_t *os_handle)
{
    PyMutex_Lock(&handle->mutex);
    int has_os_handle = handle->has_os_handle;
    if (has_os_handle) {
        *os_handle = handle->os_handle;
    }
    PyMutex_Unlock(&handle->mutex);
    return has_os_handle;
}

static void
add_to_shutdown_handles(thread_module_state *state, ThreadHandle *handle)
{
    HEAD_LOCK(&_PyRuntime);
    llist_insert_tail(&state->shutdown_handles, &handle->shutdown_node);
    HEAD_UNLOCK(&_PyRuntime);
}

static void
clear_shutdown_handles(thread_module_state *state)
{
    HEAD_LOCK(&_PyRuntime);
    struct llist_node *node;
    llist_for_each_safe(node, &state->shutdown_handles) {
        llist_remove(node);
    }
    HEAD_UNLOCK(&_PyRuntime);
}

static void
remove_from_shutdown_handles(ThreadHandle *handle)
{
    HEAD_LOCK(&_PyRuntime);
    if (handle->shutdown_node.next != NULL) {
        llist_remove(&handle->shutdown_node);
    }
    HEAD_UNLOCK(&_PyRuntime);
}

static ThreadHandle *
ThreadHandle_new(void)
{
    ThreadHandle *self =
        (ThreadHandle *)PyMem_RawCalloc(1, sizeof(ThreadHandle));
    if (self == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    self->ident = 0;
    self->os_handle = 0;
    self->has_os_handle = 0;
    self->thread_is_exiting = (PyEvent){0};
    self->mutex = (PyMutex){_Py_UNLOCKED};
    self->once = (_PyOnceFlag){0};
    self->state = THREAD_HANDLE_NOT_STARTED;
    self->refcount = 1;

    HEAD_LOCK(&_PyRuntime);
    llist_insert_tail(&_PyRuntime.threads.handles, &self->node);
    HEAD_UNLOCK(&_PyRuntime);

    return self;
}

static void
ThreadHandle_incref(ThreadHandle *self)
{
    _Py_atomic_add_ssize(&self->refcount, 1);
}

static int
detach_thread(ThreadHandle *self)
{
    if (!self->has_os_handle) {
        return 0;
    }
    // This is typically short so no need to release the GIL
    if (PyThread_detach_thread(self->os_handle)) {
        fprintf(stderr, "detach_thread: failed detaching thread\n");
        return -1;
    }
    return 0;
}

// NB: This may be called after the PyThreadState in `thread_run` has been
// deleted; it cannot call anything that relies on a valid PyThreadState
// existing.
static void
ThreadHandle_decref(ThreadHandle *self)
{
    if (_Py_atomic_add_ssize(&self->refcount, -1) > 1) {
        return;
    }

    // Remove ourself from the global list of handles
    HEAD_LOCK(&_PyRuntime);
    if (self->node.next != NULL) {
        llist_remove(&self->node);
    }
    HEAD_UNLOCK(&_PyRuntime);

    assert(self->shutdown_node.next == NULL);

    // It's safe to access state non-atomically:
    //   1. This is the destructor; nothing else holds a reference.
    //   2. The refcount going to zero is a "synchronizes-with" event; all
    //      changes from other threads are visible.
    if (self->state == THREAD_HANDLE_RUNNING && !detach_thread(self)) {
        self->state = THREAD_HANDLE_DONE;
    }

    PyMem_RawFree(self);
}

void
_PyThread_AfterFork(struct _pythread_runtime_state *state)
{
    // gh-115035: We mark ThreadHandles as not joinable early in the child's
    // after-fork handler. We do this before calling any Python code to ensure
    // that it happens before any ThreadHandles are deallocated, such as by a
    // GC cycle.
    PyThread_ident_t current = PyThread_get_thread_ident_ex();

    struct llist_node *node;
    llist_for_each_safe(node, &state->handles) {
        ThreadHandle *handle = llist_data(node, ThreadHandle, node);
        if (handle->ident == current) {
            continue;
        }

        // Mark all threads as done. Any attempts to join or detach the
        // underlying OS thread (if any) could crash. We are the only thread;
        // it's safe to set this non-atomically.
        handle->state = THREAD_HANDLE_DONE;
        handle->once = (_PyOnceFlag){_Py_ONCE_INITIALIZED};
        handle->mutex = (PyMutex){_Py_UNLOCKED};
        _PyEvent_Notify(&handle->thread_is_exiting);
        llist_remove(node);
        remove_from_shutdown_handles(handle);
    }
}

// bootstate is used to "bootstrap" new threads. Any arguments needed by
// `thread_run()`, which can only take a single argument due to platform
// limitations, are contained in bootstate.
struct bootstate {
    PyThreadState *tstate;
    PyObject *func;
    PyObject *args;
    PyObject *kwargs;
    ThreadHandle *handle;
    PyEvent handle_ready;
};

static void
thread_bootstate_free(struct bootstate *boot, int decref)
{
    if (decref) {
        Py_DECREF(boot->func);
        Py_DECREF(boot->args);
        Py_XDECREF(boot->kwargs);
    }
    ThreadHandle_decref(boot->handle);
    PyMem_RawFree(boot);
}

static void
thread_run(void *boot_raw)
{
    struct bootstate *boot = (struct bootstate *) boot_raw;
    PyThreadState *tstate = boot->tstate;

    // Wait until the handle is marked as running
    PyEvent_Wait(&boot->handle_ready);

    // `handle` needs to be manipulated after bootstate has been freed
    ThreadHandle *handle = boot->handle;
    ThreadHandle_incref(handle);

    // gh-108987: If _thread.start_new_thread() is called before or while
    // Python is being finalized, thread_run() can called *after*.
    // _PyRuntimeState_SetFinalizing() is called. At this point, all Python
    // threads must exit, except of the thread calling Py_Finalize() whch holds
    // the GIL and must not exit.
    //
    // At this stage, tstate can be a dangling pointer (point to freed memory),
    // it's ok to call _PyThreadState_MustExit() with a dangling pointer.
    if (_PyThreadState_MustExit(tstate)) {
        // Don't call PyThreadState_Clear() nor _PyThreadState_DeleteCurrent().
        // These functions are called on tstate indirectly by Py_Finalize()
        // which calls _PyInterpreterState_Clear().
        //
        // Py_DECREF() cannot be called because the GIL is not held: leak
        // references on purpose. Python is being finalized anyway.
        thread_bootstate_free(boot, 0);
        goto exit;
    }

    _PyThreadState_Bind(tstate);
    PyEval_AcquireThread(tstate);
    _Py_atomic_add_ssize(&tstate->interp->threads.count, 1);

    PyObject *res = PyObject_Call(boot->func, boot->args, boot->kwargs);
    if (res == NULL) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit))
            /* SystemExit is ignored silently */
            PyErr_Clear();
        else {
            PyErr_FormatUnraisable(
                "Exception ignored in thread started by %R", boot->func);
        }
    }
    else {
        Py_DECREF(res);
    }

    thread_bootstate_free(boot, 1);

    _Py_atomic_add_ssize(&tstate->interp->threads.count, -1);
    PyThreadState_Clear(tstate);
    _PyThreadState_DeleteCurrent(tstate);

exit:
    // Don't need to wait for this thread anymore
    remove_from_shutdown_handles(handle);

    _PyEvent_Notify(&handle->thread_is_exiting);
    ThreadHandle_decref(handle);

    // bpo-44434: Don't call explicitly PyThread_exit_thread(). On Linux with
    // the glibc, pthread_exit() can abort the whole process if dlopen() fails
    // to open the libgcc_s.so library (ex: EMFILE error).
    return;
}

static int
force_done(ThreadHandle *handle)
{
    assert(get_thread_handle_state(handle) == THREAD_HANDLE_STARTING);
    _PyEvent_Notify(&handle->thread_is_exiting);
    set_thread_handle_state(handle, THREAD_HANDLE_DONE);
    return 0;
}

static int
ThreadHandle_start(ThreadHandle *self, PyObject *func, PyObject *args,
                   PyObject *kwargs)
{
    // Mark the handle as starting to prevent any other threads from doing so
    PyMutex_Lock(&self->mutex);
    if (self->state != THREAD_HANDLE_NOT_STARTED) {
        PyMutex_Unlock(&self->mutex);
        PyErr_SetString(ThreadError, "thread already started");
        return -1;
    }
    self->state = THREAD_HANDLE_STARTING;
    PyMutex_Unlock(&self->mutex);

    // Do all the heavy lifting outside of the mutex. All other operations on
    // the handle should fail since the handle is in the starting state.

    // gh-109795: Use PyMem_RawMalloc() instead of PyMem_Malloc(),
    // because it should be possible to call thread_bootstate_free()
    // without holding the GIL.
    struct bootstate *boot = PyMem_RawMalloc(sizeof(struct bootstate));
    if (boot == NULL) {
        PyErr_NoMemory();
        goto start_failed;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    boot->tstate = _PyThreadState_New(interp, _PyThreadState_WHENCE_THREADING);
    if (boot->tstate == NULL) {
        PyMem_RawFree(boot);
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        goto start_failed;
    }
    boot->func = Py_NewRef(func);
    boot->args = Py_NewRef(args);
    boot->kwargs = Py_XNewRef(kwargs);
    boot->handle = self;
    ThreadHandle_incref(self);
    boot->handle_ready = (PyEvent){0};

    PyThread_ident_t ident;
    PyThread_handle_t os_handle;
    if (PyThread_start_joinable_thread(thread_run, boot, &ident, &os_handle)) {
        PyThreadState_Clear(boot->tstate);
        thread_bootstate_free(boot, 1);
        PyErr_SetString(ThreadError, "can't start new thread");
        goto start_failed;
    }

    // Mark the handle running
    PyMutex_Lock(&self->mutex);
    assert(self->state == THREAD_HANDLE_STARTING);
    self->ident = ident;
    self->has_os_handle = 1;
    self->os_handle = os_handle;
    self->state = THREAD_HANDLE_RUNNING;
    PyMutex_Unlock(&self->mutex);

    // Unblock the thread
    _PyEvent_Notify(&boot->handle_ready);

    return 0;

start_failed:
    _PyOnceFlag_CallOnce(&self->once, (_Py_once_fn_t *)force_done, self);
    return -1;
}

static int
join_thread(ThreadHandle *handle)
{
    assert(get_thread_handle_state(handle) == THREAD_HANDLE_RUNNING);
    PyThread_handle_t os_handle;
    if (ThreadHandle_get_os_handle(handle, &os_handle)) {
        int err = 0;
        Py_BEGIN_ALLOW_THREADS
        err = PyThread_join_thread(os_handle);
        Py_END_ALLOW_THREADS
        if (err) {
            PyErr_SetString(ThreadError, "Failed joining thread");
            return -1;
        }
    }
    set_thread_handle_state(handle, THREAD_HANDLE_DONE);
    return 0;
}

static int
check_started(ThreadHandle *self)
{
    ThreadHandleState state = get_thread_handle_state(self);
    if (state < THREAD_HANDLE_RUNNING) {
        PyErr_SetString(ThreadError, "thread not started");
        return -1;
    }
    return 0;
}

static int
ThreadHandle_join(ThreadHandle *self, PyTime_t timeout_ns)
{
    if (check_started(self) < 0) {
        return -1;
    }

    // We want to perform this check outside of the `_PyOnceFlag` to prevent
    // deadlock in the scenario where another thread joins us and we then
    // attempt to join ourselves. However, it's not safe to check thread
    // identity once the handle's os thread has finished. We may end up reusing
    // the identity stored in the handle and erroneously think we are
    // attempting to join ourselves.
    //
    // To work around this, we set `thread_is_exiting` immediately before
    // `thread_run` returns.  We can be sure that we are not attempting to join
    // ourselves if the handle's thread is about to exit.
    if (!_PyEvent_IsSet(&self->thread_is_exiting) &&
        ThreadHandle_ident(self) == PyThread_get_thread_ident_ex()) {
        // PyThread_join_thread() would deadlock or error out.
        PyErr_SetString(ThreadError, "Cannot join current thread");
        return -1;
    }

    // Wait until the deadline for the thread to exit.
    PyTime_t deadline = timeout_ns != -1 ? _PyDeadline_Init(timeout_ns) : 0;
    while (!PyEvent_WaitTimed(&self->thread_is_exiting, timeout_ns)) {
        if (deadline) {
            // _PyDeadline_Get will return a negative value if the deadline has
            // been exceeded.
            timeout_ns = Py_MAX(_PyDeadline_Get(deadline), 0);
        }

        if (timeout_ns) {
            // Interrupted
            if (Py_MakePendingCalls() < 0) {
                return -1;
            }
        }
        else {
            // Timed out
            return 0;
        }
    }

    if (_PyOnceFlag_CallOnce(&self->once, (_Py_once_fn_t *)join_thread,
                             self) == -1) {
        return -1;
    }
    assert(get_thread_handle_state(self) == THREAD_HANDLE_DONE);
    return 0;
}

static int
set_done(ThreadHandle *handle)
{
    assert(get_thread_handle_state(handle) == THREAD_HANDLE_RUNNING);
    if (detach_thread(handle) < 0) {
        PyErr_SetString(ThreadError, "failed detaching handle");
        return -1;
    }
    _PyEvent_Notify(&handle->thread_is_exiting);
    set_thread_handle_state(handle, THREAD_HANDLE_DONE);
    return 0;
}

static int
ThreadHandle_set_done(ThreadHandle *self)
{
    if (check_started(self) < 0) {
        return -1;
    }

    if (_PyOnceFlag_CallOnce(&self->once, (_Py_once_fn_t *)set_done, self) ==
        -1) {
        return -1;
    }
    assert(get_thread_handle_state(self) == THREAD_HANDLE_DONE);
    return 0;
}

// A wrapper around a ThreadHandle.
typedef struct {
    PyObject_HEAD

    ThreadHandle *handle;
} PyThreadHandleObject;

static PyThreadHandleObject *
PyThreadHandleObject_new(PyTypeObject *type)
{
    ThreadHandle *handle = ThreadHandle_new();
    if (handle == NULL) {
        return NULL;
    }

    PyThreadHandleObject *self =
        (PyThreadHandleObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        ThreadHandle_decref(handle);
        return NULL;
    }

    self->handle = handle;

    return self;
}

static PyObject *
PyThreadHandleObject_tp_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    return (PyObject *)PyThreadHandleObject_new(type);
}

static int
PyThreadHandleObject_traverse(PyThreadHandleObject *self, visitproc visit,
                              void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static void
PyThreadHandleObject_dealloc(PyThreadHandleObject *self)
{
    PyObject_GC_UnTrack(self);
    PyTypeObject *tp = Py_TYPE(self);
    ThreadHandle_decref(self->handle);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyObject *
PyThreadHandleObject_repr(PyThreadHandleObject *self)
{
    PyThread_ident_t ident = ThreadHandle_ident(self->handle);
    return PyUnicode_FromFormat("<%s object: ident=%" PY_FORMAT_THREAD_IDENT_T ">",
                                Py_TYPE(self)->tp_name, ident);
}

static PyObject *
PyThreadHandleObject_get_ident(PyThreadHandleObject *self,
                               PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromUnsignedLongLong(ThreadHandle_ident(self->handle));
}

static PyObject *
PyThreadHandleObject_join(PyThreadHandleObject *self, PyObject *args)
{
    PyObject *timeout_obj = NULL;
    if (!PyArg_ParseTuple(args, "|O:join", &timeout_obj)) {
        return NULL;
    }

    PyTime_t timeout_ns = -1;
    if (timeout_obj != NULL && timeout_obj != Py_None) {
        if (_PyTime_FromSecondsObject(&timeout_ns, timeout_obj,
                                      _PyTime_ROUND_TIMEOUT) < 0) {
            return NULL;
        }
    }

    if (ThreadHandle_join(self->handle, timeout_ns) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
PyThreadHandleObject_is_done(PyThreadHandleObject *self,
                             PyObject *Py_UNUSED(ignored))
{
    if (_PyEvent_IsSet(&self->handle->thread_is_exiting)) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static PyObject *
PyThreadHandleObject_set_done(PyThreadHandleObject *self,
                              PyObject *Py_UNUSED(ignored))
{
    if (ThreadHandle_set_done(self->handle) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyGetSetDef ThreadHandle_getsetlist[] = {
    {"ident", (getter)PyThreadHandleObject_get_ident, NULL, NULL},
    {0},
};

static PyMethodDef ThreadHandle_methods[] = {
    {"join", (PyCFunction)PyThreadHandleObject_join, METH_VARARGS, NULL},
    {"_set_done", (PyCFunction)PyThreadHandleObject_set_done, METH_NOARGS, NULL},
    {"is_done", (PyCFunction)PyThreadHandleObject_is_done, METH_NOARGS, NULL},
    {0, 0}
};

static PyType_Slot ThreadHandle_Type_slots[] = {
    {Py_tp_dealloc, (destructor)PyThreadHandleObject_dealloc},
    {Py_tp_repr, (reprfunc)PyThreadHandleObject_repr},
    {Py_tp_getset, ThreadHandle_getsetlist},
    {Py_tp_traverse, PyThreadHandleObject_traverse},
    {Py_tp_methods, ThreadHandle_methods},
    {Py_tp_new, PyThreadHandleObject_tp_new},
    {0, 0}
};

static PyType_Spec ThreadHandle_Type_spec = {
    "_thread._ThreadHandle",
    sizeof(PyThreadHandleObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_HAVE_GC,
    ThreadHandle_Type_slots,
};

/* Lock objects */

typedef struct {
    PyObject_HEAD
    PyThread_type_lock lock_lock;
    PyObject *in_weakreflist;
    char locked; /* for sanity checking */
} lockobject;

static int
lock_traverse(lockobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static void
lock_dealloc(lockobject *self)
{
    PyObject_GC_UnTrack(self);
    if (self->in_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) self);
    }
    if (self->lock_lock != NULL) {
        /* Unlock the lock so it's safe to free it */
        if (self->locked)
            PyThread_release_lock(self->lock_lock);
        PyThread_free_lock(self->lock_lock);
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

static inline PyLockStatus
acquire_timed(PyThread_type_lock lock, PyTime_t timeout)
{
    return PyThread_acquire_lock_timed_with_retries(lock, timeout);
}

static int
lock_acquire_parse_args(PyObject *args, PyObject *kwds,
                        PyTime_t *timeout)
{
    char *kwlist[] = {"blocking", "timeout", NULL};
    int blocking = 1;
    PyObject *timeout_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pO:acquire", kwlist,
                                     &blocking, &timeout_obj))
        return -1;

    // XXX Use PyThread_ParseTimeoutArg().

    const PyTime_t unset_timeout = _PyTime_FromSeconds(-1);
    *timeout = unset_timeout;

    if (timeout_obj
        && _PyTime_FromSecondsObject(timeout,
                                     timeout_obj, _PyTime_ROUND_TIMEOUT) < 0)
        return -1;

    if (!blocking && *timeout != unset_timeout ) {
        PyErr_SetString(PyExc_ValueError,
                        "can't specify a timeout for a non-blocking call");
        return -1;
    }
    if (*timeout < 0 && *timeout != unset_timeout) {
        PyErr_SetString(PyExc_ValueError,
                        "timeout value must be a non-negative number");
        return -1;
    }
    if (!blocking)
        *timeout = 0;
    else if (*timeout != unset_timeout) {
        PyTime_t microseconds;

        microseconds = _PyTime_AsMicroseconds(*timeout, _PyTime_ROUND_TIMEOUT);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "timeout value is too large");
            return -1;
        }
    }
    return 0;
}

static PyObject *
lock_PyThread_acquire_lock(lockobject *self, PyObject *args, PyObject *kwds)
{
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0)
        return NULL;

    PyLockStatus r = acquire_timed(self->lock_lock, timeout);
    if (r == PY_LOCK_INTR) {
        return NULL;
    }

    if (r == PY_LOCK_ACQUIRED)
        self->locked = 1;
    return PyBool_FromLong(r == PY_LOCK_ACQUIRED);
}

PyDoc_STRVAR(acquire_doc,
"acquire(blocking=True, timeout=-1) -> bool\n\
(acquire_lock() is an obsolete synonym)\n\
\n\
Lock the lock.  Without argument, this blocks if the lock is already\n\
locked (even by the same thread), waiting for another thread to release\n\
the lock, and return True once the lock is acquired.\n\
With an argument, this will only block if the argument is true,\n\
and the return value reflects whether the lock is acquired.\n\
The blocking operation is interruptible.");

static PyObject *
lock_PyThread_release_lock(lockobject *self, PyObject *Py_UNUSED(ignored))
{
    /* Sanity check: the lock must be locked */
    if (!self->locked) {
        PyErr_SetString(ThreadError, "release unlocked lock");
        return NULL;
    }

    self->locked = 0;
    PyThread_release_lock(self->lock_lock);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(release_doc,
"release()\n\
(release_lock() is an obsolete synonym)\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
but it needn't be locked by the same thread that unlocks it.");

static PyObject *
lock_locked_lock(lockobject *self, PyObject *Py_UNUSED(ignored))
{
    return PyBool_FromLong((long)self->locked);
}

PyDoc_STRVAR(locked_doc,
"locked() -> bool\n\
(locked_lock() is an obsolete synonym)\n\
\n\
Return whether the lock is in the locked state.");

static PyObject *
lock_repr(lockobject *self)
{
    return PyUnicode_FromFormat("<%s %s object at %p>",
        self->locked ? "locked" : "unlocked", Py_TYPE(self)->tp_name, self);
}

#ifdef HAVE_FORK
static PyObject *
lock__at_fork_reinit(lockobject *self, PyObject *Py_UNUSED(args))
{
    if (_PyThread_at_fork_reinit(&self->lock_lock) < 0) {
        PyErr_SetString(ThreadError, "failed to reinitialize lock at fork");
        return NULL;
    }

    self->locked = 0;

    Py_RETURN_NONE;
}
#endif  /* HAVE_FORK */

static lockobject *newlockobject(PyObject *module);

static PyObject *
lock_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // convert to AC?
    if (!_PyArg_NoKeywords("lock", kwargs)) {
        goto error;
    }
    if (!_PyArg_CheckPositional("lock", PyTuple_GET_SIZE(args), 0, 0)) {
        goto error;
    }

    PyObject *module = PyType_GetModuleByDef(type, &thread_module);
    assert(module != NULL);
    return (PyObject *)newlockobject(module);

error:
    return NULL;
}


static PyMethodDef lock_methods[] = {
    {"acquire_lock", _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"acquire",      _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"release_lock", (PyCFunction)lock_PyThread_release_lock,
     METH_NOARGS, release_doc},
    {"release",      (PyCFunction)lock_PyThread_release_lock,
     METH_NOARGS, release_doc},
    {"locked_lock",  (PyCFunction)lock_locked_lock,
     METH_NOARGS, locked_doc},
    {"locked",       (PyCFunction)lock_locked_lock,
     METH_NOARGS, locked_doc},
    {"__enter__",    _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"__exit__",    (PyCFunction)lock_PyThread_release_lock,
     METH_VARARGS, release_doc},
#ifdef HAVE_FORK
    {"_at_fork_reinit",    (PyCFunction)lock__at_fork_reinit,
     METH_NOARGS, NULL},
#endif
    {NULL,           NULL}              /* sentinel */
};

PyDoc_STRVAR(lock_doc,
"A lock object is a synchronization primitive.  To create a lock,\n\
call threading.Lock().  Methods are:\n\
\n\
acquire() -- lock the lock, possibly blocking until it can be obtained\n\
release() -- unlock of the lock\n\
locked() -- test whether the lock is currently locked\n\
\n\
A lock is not owned by the thread that locked it; another thread may\n\
unlock it.  A thread attempting to lock a lock that it has already locked\n\
will block until another thread unlocks it.  Deadlocks may ensue.");

static PyMemberDef lock_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(lockobject, in_weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot lock_type_slots[] = {
    {Py_tp_dealloc, (destructor)lock_dealloc},
    {Py_tp_repr, (reprfunc)lock_repr},
    {Py_tp_doc, (void *)lock_doc},
    {Py_tp_methods, lock_methods},
    {Py_tp_traverse, lock_traverse},
    {Py_tp_members, lock_type_members},
    {Py_tp_new, lock_new},
    {0, 0}
};

static PyType_Spec lock_type_spec = {
    .name = "_thread.lock",
    .basicsize = sizeof(lockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = lock_type_slots,
};

/* Recursive lock objects */

typedef struct {
    PyObject_HEAD
    PyThread_type_lock rlock_lock;
    PyThread_ident_t rlock_owner;
    unsigned long rlock_count;
    PyObject *in_weakreflist;
} rlockobject;

static int
rlock_traverse(rlockobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}


static void
rlock_dealloc(rlockobject *self)
{
    PyObject_GC_UnTrack(self);
    if (self->in_weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);
    /* self->rlock_lock can be NULL if PyThread_allocate_lock() failed
       in rlock_new() */
    if (self->rlock_lock != NULL) {
        /* Unlock the lock so it's safe to free it */
        if (self->rlock_count > 0)
            PyThread_release_lock(self->rlock_lock);

        PyThread_free_lock(self->rlock_lock);
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static bool
rlock_is_owned_by(rlockobject *self, PyThread_ident_t tid)
{
    PyThread_ident_t owner_tid =
        _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return owner_tid == tid && self->rlock_count > 0;
}

static PyObject *
rlock_acquire(rlockobject *self, PyObject *args, PyObject *kwds)
{
    PyTime_t timeout;
    PyThread_ident_t tid;
    PyLockStatus r = PY_LOCK_ACQUIRED;

    if (lock_acquire_parse_args(args, kwds, &timeout) < 0)
        return NULL;

    tid = PyThread_get_thread_ident_ex();
    if (rlock_is_owned_by(self, tid)) {
        unsigned long count = self->rlock_count + 1;
        if (count <= self->rlock_count) {
            PyErr_SetString(PyExc_OverflowError,
                            "Internal lock count overflowed");
            return NULL;
        }
        self->rlock_count = count;
        Py_RETURN_TRUE;
    }
    r = acquire_timed(self->rlock_lock, timeout);
    if (r == PY_LOCK_ACQUIRED) {
        assert(self->rlock_count == 0);
        _Py_atomic_store_ullong_relaxed(&self->rlock_owner, tid);
        self->rlock_count = 1;
    }
    else if (r == PY_LOCK_INTR) {
        return NULL;
    }

    return PyBool_FromLong(r == PY_LOCK_ACQUIRED);
}

PyDoc_STRVAR(rlock_acquire_doc,
"acquire(blocking=True) -> bool\n\
\n\
Lock the lock.  `blocking` indicates whether we should wait\n\
for the lock to be available or not.  If `blocking` is False\n\
and another thread holds the lock, the method will return False\n\
immediately.  If `blocking` is True and another thread holds\n\
the lock, the method will wait for the lock to be released,\n\
take it and then return True.\n\
(note: the blocking operation is interruptible.)\n\
\n\
In all other cases, the method will return True immediately.\n\
Precisely, if the current thread already holds the lock, its\n\
internal counter is simply incremented. If nobody holds the lock,\n\
the lock is taken and its internal counter initialized to 1.");

static PyObject *
rlock_release(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();

    if (!rlock_is_owned_by(self, tid)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot release un-acquired lock");
        return NULL;
    }
    if (--self->rlock_count == 0) {
        _Py_atomic_store_ullong_relaxed(&self->rlock_owner, 0);
        PyThread_release_lock(self->rlock_lock);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_release_doc,
"release()\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
and must be locked by the same thread that unlocks it; otherwise a\n\
`RuntimeError` is raised.\n\
\n\
Do note that if the lock was acquire()d several times in a row by the\n\
current thread, release() needs to be called as many times for the lock\n\
to be available for other threads.");

static PyObject *
rlock_acquire_restore(rlockobject *self, PyObject *args)
{
    PyThread_ident_t owner;
    unsigned long count;
    int r = 1;

    if (!PyArg_ParseTuple(args, "(k" Py_PARSE_THREAD_IDENT_T "):_acquire_restore",
            &count, &owner))
        return NULL;

    if (!PyThread_acquire_lock(self->rlock_lock, 0)) {
        Py_BEGIN_ALLOW_THREADS
        r = PyThread_acquire_lock(self->rlock_lock, 1);
        Py_END_ALLOW_THREADS
    }
    if (!r) {
        PyErr_SetString(ThreadError, "couldn't acquire lock");
        return NULL;
    }
    assert(self->rlock_count == 0);
    _Py_atomic_store_ullong_relaxed(&self->rlock_owner, owner);
    self->rlock_count = count;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_acquire_restore_doc,
"_acquire_restore(state) -> None\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_release_save(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t owner;
    unsigned long count;

    if (self->rlock_count == 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot release un-acquired lock");
        return NULL;
    }

    owner = self->rlock_owner;
    count = self->rlock_count;
    self->rlock_count = 0;
    _Py_atomic_store_ullong_relaxed(&self->rlock_owner, 0);
    PyThread_release_lock(self->rlock_lock);
    return Py_BuildValue("k" Py_PARSE_THREAD_IDENT_T, count, owner);
}

PyDoc_STRVAR(rlock_release_save_doc,
"_release_save() -> tuple\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_recursion_count(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();
    PyThread_ident_t owner =
        _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return PyLong_FromUnsignedLong(owner == tid ? self->rlock_count : 0UL);
}

PyDoc_STRVAR(rlock_recursion_count_doc,
"_recursion_count() -> int\n\
\n\
For internal use by reentrancy checks.");

static PyObject *
rlock_is_owned(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();

    if (rlock_is_owned_by(self, tid)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

PyDoc_STRVAR(rlock_is_owned_doc,
"_is_owned() -> bool\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    rlockobject *self = (rlockobject *) type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->in_weakreflist = NULL;
    self->rlock_owner = 0;
    self->rlock_count = 0;

    self->rlock_lock = PyThread_allocate_lock();
    if (self->rlock_lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ThreadError, "can't allocate lock");
        return NULL;
    }
    return (PyObject *) self;
}

static PyObject *
rlock_repr(rlockobject *self)
{
    PyThread_ident_t owner =
            _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return PyUnicode_FromFormat(
        "<%s %s object owner=%" PY_FORMAT_THREAD_IDENT_T " count=%lu at %p>",
        self->rlock_count ? "locked" : "unlocked",
        Py_TYPE(self)->tp_name, owner,
        self->rlock_count, self);
}


#ifdef HAVE_FORK
static PyObject *
rlock__at_fork_reinit(rlockobject *self, PyObject *Py_UNUSED(args))
{
    if (_PyThread_at_fork_reinit(&self->rlock_lock) < 0) {
        PyErr_SetString(ThreadError, "failed to reinitialize lock at fork");
        return NULL;
    }

    self->rlock_owner = 0;
    self->rlock_count = 0;

    Py_RETURN_NONE;
}
#endif  /* HAVE_FORK */


static PyMethodDef rlock_methods[] = {
    {"acquire",      _PyCFunction_CAST(rlock_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"release",      (PyCFunction)rlock_release,
     METH_NOARGS, rlock_release_doc},
    {"_is_owned",     (PyCFunction)rlock_is_owned,
     METH_NOARGS, rlock_is_owned_doc},
    {"_acquire_restore", (PyCFunction)rlock_acquire_restore,
     METH_VARARGS, rlock_acquire_restore_doc},
    {"_release_save", (PyCFunction)rlock_release_save,
     METH_NOARGS, rlock_release_save_doc},
    {"_recursion_count", (PyCFunction)rlock_recursion_count,
     METH_NOARGS, rlock_recursion_count_doc},
    {"__enter__",    _PyCFunction_CAST(rlock_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"__exit__",    (PyCFunction)rlock_release,
     METH_VARARGS, rlock_release_doc},
#ifdef HAVE_FORK
    {"_at_fork_reinit",    (PyCFunction)rlock__at_fork_reinit,
     METH_NOARGS, NULL},
#endif
    {NULL,           NULL}              /* sentinel */
};


static PyMemberDef rlock_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(rlockobject, in_weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot rlock_type_slots[] = {
    {Py_tp_dealloc, (destructor)rlock_dealloc},
    {Py_tp_repr, (reprfunc)rlock_repr},
    {Py_tp_methods, rlock_methods},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, rlock_new},
    {Py_tp_members, rlock_type_members},
    {Py_tp_traverse, rlock_traverse},
    {0, 0},
};

static PyType_Spec rlock_type_spec = {
    .name = "_thread.RLock",
    .basicsize = sizeof(rlockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
              Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE),
    .slots = rlock_type_slots,
};

static lockobject *
newlockobject(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);

    PyTypeObject *type = state->lock_type;
    lockobject *self = (lockobject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->lock_lock = PyThread_allocate_lock();
    self->locked = 0;
    self->in_weakreflist = NULL;

    if (self->lock_lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ThreadError, "can't allocate lock");
        return NULL;
    }
    return self;
}

/* Thread-local objects */

/* Quick overview:

   We need to be able to reclaim reference cycles as soon as possible
   (both when a thread is being terminated, or a thread-local object
    becomes unreachable from user data).  Constraints:
   - it must not be possible for thread-state dicts to be involved in
     reference cycles (otherwise the cyclic GC will refuse to consider
     objects referenced from a reachable thread-state dict, even though
     local_dealloc would clear them)
   - the death of a thread-state dict must still imply destruction of the
     corresponding local dicts in all thread-local objects.

   Our implementation uses small "localdummy" objects in order to break
   the reference chain. These trivial objects are hashable (using the
   default scheme of identity hashing) and weakrefable.
   Each thread-state holds a separate localdummy for each local object
   (as a /strong reference/),
   and each thread-local object holds a dict mapping /weak references/
   of localdummies to local dicts.

   Therefore:
   - only the thread-state dict holds a strong reference to the dummies
   - only the thread-local object holds a strong reference to the local dicts
   - only outside objects (application- or library-level) hold strong
     references to the thread-local objects
   - as soon as a thread-state dict is destroyed, the weakref callbacks of all
     dummies attached to that thread are called, and destroy the corresponding
     local dicts from thread-local objects
   - as soon as a thread-local object is destroyed, its local dicts are
     destroyed and its dummies are manually removed from all thread states
   - the GC can do its work correctly when a thread-local object is dangling,
     without any interference from the thread-state dicts

   As an additional optimization, each localdummy holds a borrowed reference
   to the corresponding localdict.  This borrowed reference is only used
   by the thread-local object which has created the localdummy, which should
   guarantee that the localdict still exists when accessed.
*/

typedef struct {
    PyObject_HEAD
    PyObject *localdict;        /* Borrowed reference! */
    PyObject *weakreflist;      /* List of weak references to self */
} localdummyobject;

static void
localdummy_dealloc(localdummyobject *self)
{
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

static PyMemberDef local_dummy_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(localdummyobject, weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot local_dummy_type_slots[] = {
    {Py_tp_dealloc, (destructor)localdummy_dealloc},
    {Py_tp_doc, "Thread-local dummy"},
    {Py_tp_members, local_dummy_type_members},
    {0, 0}
};

static PyType_Spec local_dummy_type_spec = {
    .name = "_thread._localdummy",
    .basicsize = sizeof(localdummyobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = local_dummy_type_slots,
};


typedef struct {
    PyObject_HEAD
    PyObject *key;
    PyObject *args;
    PyObject *kw;
    PyObject *weakreflist;      /* List of weak references to self */
    /* A {localdummy weakref -> localdict} dict */
    PyObject *dummies;
    /* The callback for weakrefs to localdummies */
    PyObject *wr_callback;
} localobject;

/* Forward declaration */
static PyObject *_ldict(localobject *self, thread_module_state *state);
static PyObject *_localdummy_destroyed(PyObject *meth_self, PyObject *dummyweakref);

/* Create and register the dummy for the current thread.
   Returns a borrowed reference of the corresponding local dict */
static PyObject *
_local_create_dummy(localobject *self, thread_module_state *state)
{
    PyObject *ldict = NULL, *wr = NULL;
    localdummyobject *dummy = NULL;
    PyTypeObject *type = state->local_dummy_type;

    PyObject *tdict = PyThreadState_GetDict();
    if (tdict == NULL) {
        PyErr_SetString(PyExc_SystemError,
                        "Couldn't get thread-state dictionary");
        goto err;
    }

    ldict = PyDict_New();
    if (ldict == NULL) {
        goto err;
    }
    dummy = (localdummyobject *) type->tp_alloc(type, 0);
    if (dummy == NULL) {
        goto err;
    }
    dummy->localdict = ldict;
    wr = PyWeakref_NewRef((PyObject *) dummy, self->wr_callback);
    if (wr == NULL) {
        goto err;
    }

    /* As a side-effect, this will cache the weakref's hash before the
       dummy gets deleted */
    int r = PyDict_SetItem(self->dummies, wr, ldict);
    if (r < 0) {
        goto err;
    }
    Py_CLEAR(wr);
    r = PyDict_SetItem(tdict, self->key, (PyObject *) dummy);
    if (r < 0) {
        goto err;
    }
    Py_CLEAR(dummy);

    Py_DECREF(ldict);
    return ldict;

err:
    Py_XDECREF(ldict);
    Py_XDECREF(wr);
    Py_XDECREF(dummy);
    return NULL;
}

static PyObject *
local_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    static PyMethodDef wr_callback_def = {
        "_localdummy_destroyed", (PyCFunction) _localdummy_destroyed, METH_O
    };

    if (type->tp_init == PyBaseObject_Type.tp_init) {
        int rc = 0;
        if (args != NULL)
            rc = PyObject_IsTrue(args);
        if (rc == 0 && kw != NULL)
            rc = PyObject_IsTrue(kw);
        if (rc != 0) {
            if (rc > 0) {
                PyErr_SetString(PyExc_TypeError,
                          "Initialization arguments are not supported");
            }
            return NULL;
        }
    }

    PyObject *module = PyType_GetModuleByDef(type, &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    localobject *self = (localobject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->args = Py_XNewRef(args);
    self->kw = Py_XNewRef(kw);
    self->key = PyUnicode_FromFormat("thread.local.%p", self);
    if (self->key == NULL) {
        goto err;
    }

    self->dummies = PyDict_New();
    if (self->dummies == NULL) {
        goto err;
    }

    /* We use a weak reference to self in the callback closure
       in order to avoid spurious reference cycles */
    PyObject *wr = PyWeakref_NewRef((PyObject *) self, NULL);
    if (wr == NULL) {
        goto err;
    }
    self->wr_callback = PyCFunction_NewEx(&wr_callback_def, wr, NULL);
    Py_DECREF(wr);
    if (self->wr_callback == NULL) {
        goto err;
    }
    if (_local_create_dummy(self, state) == NULL) {
        goto err;
    }
    return (PyObject *)self;

  err:
    Py_DECREF(self);
    return NULL;
}

static int
local_traverse(localobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->args);
    Py_VISIT(self->kw);
    Py_VISIT(self->dummies);
    return 0;
}

static int
local_clear(localobject *self)
{
    Py_CLEAR(self->args);
    Py_CLEAR(self->kw);
    Py_CLEAR(self->dummies);
    Py_CLEAR(self->wr_callback);
    /* Remove all strong references to dummies from the thread states */
    if (self->key) {
        PyInterpreterState *interp = _PyInterpreterState_GET();
        _PyRuntimeState *runtime = &_PyRuntime;
        HEAD_LOCK(runtime);
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        HEAD_UNLOCK(runtime);
        while (tstate) {
            if (tstate->dict) {
                if (PyDict_Pop(tstate->dict, self->key, NULL) < 0) {
                    // Silently ignore error
                    PyErr_Clear();
                }
            }
            HEAD_LOCK(runtime);
            tstate = PyThreadState_Next(tstate);
            HEAD_UNLOCK(runtime);
        }
    }
    return 0;
}

static void
local_dealloc(localobject *self)
{
    /* Weakrefs must be invalidated right now, otherwise they can be used
       from code called below, which is very dangerous since Py_REFCNT(self) == 0 */
    if (self->weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) self);
    }

    PyObject_GC_UnTrack(self);

    local_clear(self);
    Py_XDECREF(self->key);

    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

/* Returns a borrowed reference to the local dict, creating it if necessary */
static PyObject *
_ldict(localobject *self, thread_module_state *state)
{
    PyObject *tdict = PyThreadState_GetDict();
    if (tdict == NULL) {
        PyErr_SetString(PyExc_SystemError,
                        "Couldn't get thread-state dictionary");
        return NULL;
    }

    PyObject *ldict;
    PyObject *dummy = PyDict_GetItemWithError(tdict, self->key);
    if (dummy == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        ldict = _local_create_dummy(self, state);
        if (ldict == NULL)
            return NULL;

        if (Py_TYPE(self)->tp_init != PyBaseObject_Type.tp_init &&
            Py_TYPE(self)->tp_init((PyObject*)self,
                                   self->args, self->kw) < 0) {
            /* we need to get rid of ldict from thread so
               we create a new one the next time we do an attr
               access */
            PyDict_DelItem(tdict, self->key);
            return NULL;
        }
    }
    else {
        assert(Py_IS_TYPE(dummy, state->local_dummy_type));
        ldict = ((localdummyobject *) dummy)->localdict;
    }

    return ldict;
}

static int
local_setattro(localobject *self, PyObject *name, PyObject *v)
{
    PyObject *module = PyType_GetModuleByDef(Py_TYPE(self), &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    PyObject *ldict = _ldict(self, state);
    if (ldict == NULL) {
        return -1;
    }

    int r = PyObject_RichCompareBool(name, &_Py_ID(__dict__), Py_EQ);
    if (r == -1) {
        return -1;
    }
    if (r == 1) {
        PyErr_Format(PyExc_AttributeError,
                     "'%.100s' object attribute '%U' is read-only",
                     Py_TYPE(self)->tp_name, name);
        return -1;
    }

    return _PyObject_GenericSetAttrWithDict((PyObject *)self, name, v, ldict);
}

static PyObject *local_getattro(localobject *, PyObject *);

static PyMemberDef local_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(localobject, weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot local_type_slots[] = {
    {Py_tp_dealloc, (destructor)local_dealloc},
    {Py_tp_getattro, (getattrofunc)local_getattro},
    {Py_tp_setattro, (setattrofunc)local_setattro},
    {Py_tp_doc, "Thread-local data"},
    {Py_tp_traverse, (traverseproc)local_traverse},
    {Py_tp_clear, (inquiry)local_clear},
    {Py_tp_new, local_new},
    {Py_tp_members, local_type_members},
    {0, 0}
};

static PyType_Spec local_type_spec = {
    .name = "_thread._local",
    .basicsize = sizeof(localobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = local_type_slots,
};

static PyObject *
local_getattro(localobject *self, PyObject *name)
{
    PyObject *module = PyType_GetModuleByDef(Py_TYPE(self), &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    PyObject *ldict = _ldict(self, state);
    if (ldict == NULL)
        return NULL;

    int r = PyObject_RichCompareBool(name, &_Py_ID(__dict__), Py_EQ);
    if (r == 1) {
        return Py_NewRef(ldict);
    }
    if (r == -1) {
        return NULL;
    }

    if (!Py_IS_TYPE(self, state->local_type)) {
        /* use generic lookup for subtypes */
        return _PyObject_GenericGetAttrWithDict((PyObject *)self, name,
                                                ldict, 0);
    }

    /* Optimization: just look in dict ourselves */
    PyObject *value;
    if (PyDict_GetItemRef(ldict, name, &value) != 0) {
        // found or error
        return value;
    }

    /* Fall back on generic to get __class__ and __dict__ */
    return _PyObject_GenericGetAttrWithDict(
        (PyObject *)self, name, ldict, 0);
}

/* Called when a dummy is destroyed. */
static PyObject *
_localdummy_destroyed(PyObject *localweakref, PyObject *dummyweakref)
{
    localobject *self = (localobject *)_PyWeakref_GET_REF(localweakref);
    if (self == NULL) {
        Py_RETURN_NONE;
    }

    /* If the thread-local object is still alive and not being cleared,
       remove the corresponding local dict */
    if (self->dummies != NULL) {
        if (PyDict_Pop(self->dummies, dummyweakref, NULL) < 0) {
            PyErr_WriteUnraisable((PyObject*)self);
        }
    }
    Py_DECREF(self);
    Py_RETURN_NONE;
}

/* Module functions */

static PyObject *
thread_daemon_threads_allowed(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->feature_flags & Py_RTFLAGS_DAEMON_THREADS) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

PyDoc_STRVAR(daemon_threads_allowed_doc,
"daemon_threads_allowed()\n\
\n\
Return True if daemon threads are allowed in the current interpreter,\n\
and False otherwise.\n");

static int
do_start_new_thread(thread_module_state *state, PyObject *func, PyObject *args,
                    PyObject *kwargs, ThreadHandle *handle, int daemon)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (!_PyInterpreterState_HasFeature(interp, Py_RTFLAGS_THREADS)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "thread is not supported for isolated subinterpreters");
        return -1;
    }
    if (interp->finalizing) {
        PyErr_SetString(PyExc_PythonFinalizationError,
                        "can't create new thread at interpreter shutdown");
        return -1;
    }

    if (!daemon) {
        // Add the handle before starting the thread to avoid adding a handle
        // to a thread that has already finished (i.e. if the thread finishes
        // before the call to `ThreadHandle_start()` below returns).
        add_to_shutdown_handles(state, handle);
    }

    if (ThreadHandle_start(handle, func, args, kwargs) < 0) {
        if (!daemon) {
            remove_from_shutdown_handles(handle);
        }
        return -1;
    }

    return 0;
}

static PyObject *
thread_PyThread_start_new_thread(PyObject *module, PyObject *fargs)
{
    PyObject *func, *args, *kwargs = NULL;
    thread_module_state *state = get_thread_state(module);

    if (!PyArg_UnpackTuple(fargs, "start_new_thread", 2, 3,
                           &func, &args, &kwargs))
        return NULL;
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError,
                        "first arg must be callable");
        return NULL;
    }
    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError,
                        "2nd arg must be a tuple");
        return NULL;
    }
    if (kwargs != NULL && !PyDict_Check(kwargs)) {
        PyErr_SetString(PyExc_TypeError,
                        "optional 3rd arg must be a dictionary");
        return NULL;
    }

    if (PySys_Audit("_thread.start_new_thread", "OOO",
                    func, args, kwargs ? kwargs : Py_None) < 0) {
        return NULL;
    }

    ThreadHandle *handle = ThreadHandle_new();
    if (handle == NULL) {
        return NULL;
    }

    int st =
        do_start_new_thread(state, func, args, kwargs, handle, /*daemon=*/1);
    if (st < 0) {
        ThreadHandle_decref(handle);
        return NULL;
    }
    PyThread_ident_t ident = ThreadHandle_ident(handle);
    ThreadHandle_decref(handle);
    return PyLong_FromUnsignedLongLong(ident);
}

PyDoc_STRVAR(start_new_doc,
"start_new_thread(function, args[, kwargs])\n\
(start_new() is an obsolete synonym)\n\
\n\
Start a new thread and return its identifier.\n\
\n\
The thread will call the function with positional arguments from the\n\
tuple args and keyword arguments taken from the optional dictionary\n\
kwargs.  The thread exits when the function returns; the return value\n\
is ignored.  The thread will also exit when the function raises an\n\
unhandled exception; a stack trace will be printed unless the exception\n\
is SystemExit.\n");

static PyObject *
thread_PyThread_start_joinable_thread(PyObject *module, PyObject *fargs,
                                      PyObject *fkwargs)
{
    static char *keywords[] = {"function", "handle", "daemon", NULL};
    PyObject *func = NULL;
    int daemon = 1;
    thread_module_state *state = get_thread_state(module);
    PyObject *hobj = NULL;
    if (!PyArg_ParseTupleAndKeywords(fargs, fkwargs,
                                     "O|Op:start_joinable_thread", keywords,
                                     &func, &hobj, &daemon)) {
        return NULL;
    }

    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError,
                        "thread function must be callable");
        return NULL;
    }

    if (hobj == NULL) {
        hobj = Py_None;
    }
    else if (hobj != Py_None && !Py_IS_TYPE(hobj, state->thread_handle_type)) {
        PyErr_SetString(PyExc_TypeError, "'handle' must be a _ThreadHandle");
        return NULL;
    }

    if (PySys_Audit("_thread.start_joinable_thread", "OiO", func, daemon,
                    hobj) < 0) {
        return NULL;
    }

    if (hobj == Py_None) {
        hobj = (PyObject *)PyThreadHandleObject_new(state->thread_handle_type);
        if (hobj == NULL) {
            return NULL;
        }
    }
    else {
        Py_INCREF(hobj);
    }

    PyObject* args = PyTuple_New(0);
    if (args == NULL) {
        return NULL;
    }
    int st = do_start_new_thread(state, func, args,
                                 /*kwargs=*/ NULL, ((PyThreadHandleObject*)hobj)->handle, daemon);
    Py_DECREF(args);
    if (st < 0) {
        Py_DECREF(hobj);
        return NULL;
    }
    return (PyObject *) hobj;
}

PyDoc_STRVAR(start_joinable_doc,
"start_joinable_thread(function[, daemon=True[, handle=None]])\n\
\n\
*For internal use only*: start a new thread.\n\
\n\
Like start_new_thread(), this starts a new thread calling the given function.\n\
Unlike start_new_thread(), this returns a handle object with methods to join\n\
or detach the given thread.\n\
This function is not for third-party code, please use the\n\
`threading` module instead. During finalization the runtime will not wait for\n\
the thread to exit if daemon is True. If handle is provided it must be a\n\
newly created thread._ThreadHandle instance.");

static PyObject *
thread_PyThread_exit_thread(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyErr_SetNone(PyExc_SystemExit);
    return NULL;
}

PyDoc_STRVAR(exit_doc,
"exit()\n\
(exit_thread() is an obsolete synonym)\n\
\n\
This is synonymous to ``raise SystemExit''.  It will cause the current\n\
thread to exit silently unless the exception is caught.");

static PyObject *
thread_PyThread_interrupt_main(PyObject *self, PyObject *args)
{
    int signum = SIGINT;
    if (!PyArg_ParseTuple(args, "|i:signum", &signum)) {
        return NULL;
    }

    if (PyErr_SetInterruptEx(signum)) {
        PyErr_SetString(PyExc_ValueError, "signal number out of range");
        return NULL;
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(interrupt_doc,
"interrupt_main(signum=signal.SIGINT, /)\n\
\n\
Simulate the arrival of the given signal in the main thread,\n\
where the corresponding signal handler will be executed.\n\
If *signum* is omitted, SIGINT is assumed.\n\
A subthread can use this function to interrupt the main thread.\n\
\n\
Note: the default signal handler for SIGINT raises ``KeyboardInterrupt``."
);

static PyObject *
thread_PyThread_allocate_lock(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return (PyObject *) newlockobject(module);
}

PyDoc_STRVAR(allocate_doc,
"allocate_lock() -> lock object\n\
(allocate() is an obsolete synonym)\n\
\n\
Create a new lock object. See help(type(threading.Lock())) for\n\
information about locks.");

static PyObject *
thread_get_ident(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t ident = PyThread_get_thread_ident_ex();
    if (ident == PYTHREAD_INVALID_THREAD_ID) {
        PyErr_SetString(ThreadError, "no current thread ident");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(ident);
}

PyDoc_STRVAR(get_ident_doc,
"get_ident() -> integer\n\
\n\
Return a non-zero integer that uniquely identifies the current thread\n\
amongst other threads that exist simultaneously.\n\
This may be used to identify per-thread resources.\n\
Even though on some platforms threads identities may appear to be\n\
allocated consecutive numbers starting at 1, this behavior should not\n\
be relied upon, and the number should be seen purely as a magic cookie.\n\
A thread's identity may be reused for another thread after it exits.");

#ifdef PY_HAVE_THREAD_NATIVE_ID
static PyObject *
thread_get_native_id(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    unsigned long native_id = PyThread_get_thread_native_id();
    return PyLong_FromUnsignedLong(native_id);
}

PyDoc_STRVAR(get_native_id_doc,
"get_native_id() -> integer\n\
\n\
Return a non-negative integer identifying the thread as reported\n\
by the OS (kernel). This may be used to uniquely identify a\n\
particular thread within a system.");
#endif

static PyObject *
thread__count(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return PyLong_FromSsize_t(_Py_atomic_load_ssize(&interp->threads.count));
}

PyDoc_STRVAR(_count_doc,
"_count() -> integer\n\
\n\
\
Return the number of currently running Python threads, excluding\n\
the main thread. The returned number comprises all threads created\n\
through `start_new_thread()` as well as `threading.Thread`, and not\n\
yet finished.\n\
\n\
This function is meant for internal and specialized purposes only.\n\
In most applications `threading.enumerate()` should be used instead.");

static PyObject *
thread_stack_size(PyObject *self, PyObject *args)
{
    size_t old_size;
    Py_ssize_t new_size = 0;
    int rc;

    if (!PyArg_ParseTuple(args, "|n:stack_size", &new_size))
        return NULL;

    if (new_size < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "size must be 0 or a positive value");
        return NULL;
    }

    old_size = PyThread_get_stacksize();

    rc = PyThread_set_stacksize((size_t) new_size);
    if (rc == -1) {
        PyErr_Format(PyExc_ValueError,
                     "size not valid: %zd bytes",
                     new_size);
        return NULL;
    }
    if (rc == -2) {
        PyErr_SetString(ThreadError,
                        "setting stack size not supported");
        return NULL;
    }

    return PyLong_FromSsize_t((Py_ssize_t) old_size);
}

PyDoc_STRVAR(stack_size_doc,
"stack_size([size]) -> size\n\
\n\
Return the thread stack size used when creating new threads.  The\n\
optional size argument specifies the stack size (in bytes) to be used\n\
for subsequently created threads, and must be 0 (use platform or\n\
configured default) or a positive integer value of at least 32,768 (32k).\n\
If changing the thread stack size is unsupported, a ThreadError\n\
exception is raised.  If the specified size is invalid, a ValueError\n\
exception is raised, and the stack size is unmodified.  32k bytes\n\
 currently the minimum supported stack size value to guarantee\n\
sufficient stack space for the interpreter itself.\n\
\n\
Note that some platforms may have particular restrictions on values for\n\
the stack size, such as requiring a minimum stack size larger than 32 KiB or\n\
requiring allocation in multiples of the system memory page size\n\
- platform documentation should be referred to for more information\n\
(4 KiB pages are common; using multiples of 4096 for the stack size is\n\
the suggested approach in the absence of more specific information).");

static int
thread_excepthook_file(PyObject *file, PyObject *exc_type, PyObject *exc_value,
                       PyObject *exc_traceback, PyObject *thread)
{
    /* print(f"Exception in thread {thread.name}:", file=file) */
    if (PyFile_WriteString("Exception in thread ", file) < 0) {
        return -1;
    }

    PyObject *name = NULL;
    if (thread != Py_None) {
        if (PyObject_GetOptionalAttr(thread, &_Py_ID(name), &name) < 0) {
            return -1;
        }
    }
    if (name != NULL) {
        if (PyFile_WriteObject(name, file, Py_PRINT_RAW) < 0) {
            Py_DECREF(name);
            return -1;
        }
        Py_DECREF(name);
    }
    else {
        PyThread_ident_t ident = PyThread_get_thread_ident_ex();
        PyObject *str = PyUnicode_FromFormat("%" PY_FORMAT_THREAD_IDENT_T, ident);
        if (str != NULL) {
            if (PyFile_WriteObject(str, file, Py_PRINT_RAW) < 0) {
                Py_DECREF(str);
                return -1;
            }
            Py_DECREF(str);
        }
        else {
            PyErr_Clear();

            if (PyFile_WriteString("<failed to get thread name>", file) < 0) {
                return -1;
            }
        }
    }

    if (PyFile_WriteString(":\n", file) < 0) {
        return -1;
    }

    /* Display the traceback */
    _PyErr_Display(file, exc_type, exc_value, exc_traceback);

    /* Call file.flush() */
    if (_PyFile_Flush(file) < 0) {
        return -1;
    }

    return 0;
}


PyDoc_STRVAR(ExceptHookArgs__doc__,
"ExceptHookArgs\n\
\n\
Type used to pass arguments to threading.excepthook.");

static PyStructSequence_Field ExceptHookArgs_fields[] = {
    {"exc_type", "Exception type"},
    {"exc_value", "Exception value"},
    {"exc_traceback", "Exception traceback"},
    {"thread", "Thread"},
    {0}
};

static PyStructSequence_Desc ExceptHookArgs_desc = {
    .name = "_thread._ExceptHookArgs",
    .doc = ExceptHookArgs__doc__,
    .fields = ExceptHookArgs_fields,
    .n_in_sequence = 4
};


static PyObject *
thread_excepthook(PyObject *module, PyObject *args)
{
    thread_module_state *state = get_thread_state(module);

    if (!Py_IS_TYPE(args, state->excepthook_type)) {
        PyErr_SetString(PyExc_TypeError,
                        "_thread.excepthook argument type "
                        "must be ExceptHookArgs");
        return NULL;
    }

    /* Borrowed reference */
    PyObject *exc_type = PyStructSequence_GET_ITEM(args, 0);
    if (exc_type == PyExc_SystemExit) {
        /* silently ignore SystemExit */
        Py_RETURN_NONE;
    }

    /* Borrowed references */
    PyObject *exc_value = PyStructSequence_GET_ITEM(args, 1);
    PyObject *exc_tb = PyStructSequence_GET_ITEM(args, 2);
    PyObject *thread = PyStructSequence_GET_ITEM(args, 3);

    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *file = _PySys_GetAttr(tstate, &_Py_ID(stderr));
    if (file == NULL || file == Py_None) {
        if (thread == Py_None) {
            /* do nothing if sys.stderr is None and thread is None */
            Py_RETURN_NONE;
        }

        file = PyObject_GetAttrString(thread, "_stderr");
        if (file == NULL) {
            return NULL;
        }
        if (file == Py_None) {
            Py_DECREF(file);
            /* do nothing if sys.stderr is None and sys.stderr was None
               when the thread was created */
            Py_RETURN_NONE;
        }
    }
    else {
        Py_INCREF(file);
    }

    int res = thread_excepthook_file(file, exc_type, exc_value, exc_tb,
                                     thread);
    Py_DECREF(file);
    if (res < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(excepthook_doc,
"excepthook(exc_type, exc_value, exc_traceback, thread)\n\
\n\
Handle uncaught Thread.run() exception.");

static PyObject *
thread__is_main_interpreter(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return PyBool_FromLong(_Py_IsMainInterpreter(interp));
}

PyDoc_STRVAR(thread__is_main_interpreter_doc,
"_is_main_interpreter()\n\
\n\
Return True if the current interpreter is the main Python interpreter.");

static PyObject *
thread_shutdown(PyObject *self, PyObject *args)
{
    PyThread_ident_t ident = PyThread_get_thread_ident_ex();
    thread_module_state *state = get_thread_state(self);

    for (;;) {
        ThreadHandle *handle = NULL;

        // Find a thread that's not yet finished.
        HEAD_LOCK(&_PyRuntime);
        struct llist_node *node;
        llist_for_each_safe(node, &state->shutdown_handles) {
            ThreadHandle *cur = llist_data(node, ThreadHandle, shutdown_node);
            if (cur->ident != ident) {
                ThreadHandle_incref(cur);
                handle = cur;
                break;
            }
        }
        HEAD_UNLOCK(&_PyRuntime);

        if (!handle) {
            // No more threads to wait on!
            break;
        }

        // Wait for the thread to finish. If we're interrupted, such
        // as by a ctrl-c we print the error and exit early.
        if (ThreadHandle_join(handle, -1) < 0) {
            PyErr_WriteUnraisable(NULL);
            ThreadHandle_decref(handle);
            Py_RETURN_NONE;
        }

        ThreadHandle_decref(handle);
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(shutdown_doc,
"_shutdown()\n\
\n\
Wait for all non-daemon threads (other than the calling thread) to stop.");

static PyObject *
thread__make_thread_handle(PyObject *module, PyObject *identobj)
{
    thread_module_state *state = get_thread_state(module);
    if (!PyLong_Check(identobj)) {
        PyErr_SetString(PyExc_TypeError, "ident must be an integer");
        return NULL;
    }
    PyThread_ident_t ident = PyLong_AsUnsignedLongLong(identobj);
    if (PyErr_Occurred()) {
        return NULL;
    }
    PyThreadHandleObject *hobj =
        PyThreadHandleObject_new(state->thread_handle_type);
    if (hobj == NULL) {
        return NULL;
    }
    PyMutex_Lock(&hobj->handle->mutex);
    hobj->handle->ident = ident;
    hobj->handle->state = THREAD_HANDLE_RUNNING;
    PyMutex_Unlock(&hobj->handle->mutex);
    return (PyObject*) hobj;
}

PyDoc_STRVAR(thread__make_thread_handle_doc,
"_make_thread_handle(ident)\n\
\n\
Internal only. Make a thread handle for threads not spawned\n\
by the _thread or threading module.");

static PyObject *
thread__get_main_thread_ident(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromUnsignedLongLong(_PyRuntime.main_thread);
}

PyDoc_STRVAR(thread__get_main_thread_ident_doc,
"_get_main_thread_ident()\n\
\n\
Internal only. Return a non-zero integer that uniquely identifies the main thread\n\
of the main interpreter.");

static PyMethodDef thread_methods[] = {
    {"start_new_thread",        (PyCFunction)thread_PyThread_start_new_thread,
     METH_VARARGS, start_new_doc},
    {"start_new",               (PyCFunction)thread_PyThread_start_new_thread,
     METH_VARARGS, start_new_doc},
    {"start_joinable_thread",   _PyCFunction_CAST(thread_PyThread_start_joinable_thread),
     METH_VARARGS | METH_KEYWORDS, start_joinable_doc},
    {"daemon_threads_allowed",  (PyCFunction)thread_daemon_threads_allowed,
     METH_NOARGS, daemon_threads_allowed_doc},
    {"allocate_lock",           thread_PyThread_allocate_lock,
     METH_NOARGS, allocate_doc},
    {"allocate",                thread_PyThread_allocate_lock,
     METH_NOARGS, allocate_doc},
    {"exit_thread",             thread_PyThread_exit_thread,
     METH_NOARGS, exit_doc},
    {"exit",                    thread_PyThread_exit_thread,
     METH_NOARGS, exit_doc},
    {"interrupt_main",          (PyCFunction)thread_PyThread_interrupt_main,
     METH_VARARGS, interrupt_doc},
    {"get_ident",               thread_get_ident,
     METH_NOARGS, get_ident_doc},
#ifdef PY_HAVE_THREAD_NATIVE_ID
    {"get_native_id",           thread_get_native_id,
     METH_NOARGS, get_native_id_doc},
#endif
    {"_count",                  thread__count,
     METH_NOARGS, _count_doc},
    {"stack_size",              (PyCFunction)thread_stack_size,
     METH_VARARGS, stack_size_doc},
    {"_excepthook",             thread_excepthook,
     METH_O, excepthook_doc},
    {"_is_main_interpreter",    thread__is_main_interpreter,
     METH_NOARGS, thread__is_main_interpreter_doc},
    {"_shutdown",               thread_shutdown,
     METH_NOARGS, shutdown_doc},
    {"_make_thread_handle", thread__make_thread_handle,
     METH_O, thread__make_thread_handle_doc},
    {"_get_main_thread_ident", thread__get_main_thread_ident,
     METH_NOARGS, thread__get_main_thread_ident_doc},
    {NULL,                      NULL}           /* sentinel */
};


/* Initialization function */

static int
thread_module_exec(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);
    PyObject *d = PyModule_GetDict(module);

    // Initialize the C thread library
    PyThread_init_thread();

    // _ThreadHandle
    state->thread_handle_type = (PyTypeObject *)PyType_FromSpec(&ThreadHandle_Type_spec);
    if (state->thread_handle_type == NULL) {
        return -1;
    }
    if (PyDict_SetItemString(d, "_ThreadHandle", (PyObject *)state->thread_handle_type) < 0) {
        return -1;
    }

    // Lock
    state->lock_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &lock_type_spec, NULL);
    if (state->lock_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->lock_type) < 0) {
        return -1;
    }
    // Old alias: lock -> LockType
    if (PyDict_SetItemString(d, "LockType", (PyObject *)state->lock_type) < 0) {
        return -1;
    }

    // RLock
    PyTypeObject *rlock_type = (PyTypeObject *)PyType_FromSpec(&rlock_type_spec);
    if (rlock_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, rlock_type) < 0) {
        Py_DECREF(rlock_type);
        return -1;
    }
    Py_DECREF(rlock_type);

    // Local dummy
    state->local_dummy_type = (PyTypeObject *)PyType_FromSpec(&local_dummy_type_spec);
    if (state->local_dummy_type == NULL) {
        return -1;
    }

    // Local
    state->local_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &local_type_spec, NULL);
    if (state->local_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->local_type) < 0) {
        return -1;
    }

    // Add module attributes
    if (PyDict_SetItemString(d, "error", ThreadError) < 0) {
        return -1;
    }

    // _ExceptHookArgs type
    state->excepthook_type = PyStructSequence_NewType(&ExceptHookArgs_desc);
    if (state->excepthook_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->excepthook_type) < 0) {
        return -1;
    }

    // TIMEOUT_MAX
    double timeout_max = (double)PY_TIMEOUT_MAX * 1e-6;
    double time_max = PyTime_AsSecondsDouble(PyTime_MAX);
    timeout_max = Py_MIN(timeout_max, time_max);
    // Round towards minus infinity
    timeout_max = floor(timeout_max);

    if (PyModule_Add(module, "TIMEOUT_MAX",
                        PyFloat_FromDouble(timeout_max)) < 0) {
        return -1;
    }

    llist_init(&state->shutdown_handles);

    return 0;
}


static int
thread_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    thread_module_state *state = get_thread_state(module);
    Py_VISIT(state->excepthook_type);
    Py_VISIT(state->lock_type);
    Py_VISIT(state->local_type);
    Py_VISIT(state->local_dummy_type);
    Py_VISIT(state->thread_handle_type);
    return 0;
}

static int
thread_module_clear(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);
    Py_CLEAR(state->excepthook_type);
    Py_CLEAR(state->lock_type);
    Py_CLEAR(state->local_type);
    Py_CLEAR(state->local_dummy_type);
    Py_CLEAR(state->thread_handle_type);
    // Remove any remaining handles (e.g. if shutdown exited early due to
    // interrupt) so that attempts to unlink the handle after our module state
    // is destroyed do not crash.
    clear_shutdown_handles(state);
    return 0;
}

static void
thread_module_free(void *module)
{
    thread_module_clear((PyObject *)module);
}



PyDoc_STRVAR(thread_doc,
"This module provides primitive operations to write multi-threaded programs.\n\
The 'threading' module provides a more convenient interface.");

static PyModuleDef_Slot thread_module_slots[] = {
    {Py_mod_exec, thread_module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, NULL}
};

static struct PyModuleDef thread_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_thread",
    .m_doc = thread_doc,
    .m_size = sizeof(thread_module_state),
    .m_methods = thread_methods,
    .m_traverse = thread_module_traverse,
    .m_clear = thread_module_clear,
    .m_free = thread_module_free,
    .m_slots = thread_module_slots,
};

PyMODINIT_FUNC
PyInit__thread(void)
{
    return PyModuleDef_Init(&thread_module);
}
