// Copyright 2025 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdbool.h>

#ifndef _WIN32
#include <signal.h>
#endif

#include <Python.h>

#include <rcutils/allocator.h>
#include <rcutils/strdup.h>
#include <rcutils/types.h>
#include <rmw/convert_rcutils_ret_to_rmw_ret.h>
#include <rmw_test_fixture/rmw_test_fixture.h>

#if PY_VERSION_HEX >= 0x030E0000

static PyMutex isolation_mutex;

static int update_environment_cache(PyObject *data, PyObject *snapshot)
{
  int result = -1;
  Py_BEGIN_CRITICAL_SECTION(data);
  PyObject *keys = PyDict_Keys(data);
  if (keys) {
    result = 0;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(keys); ++i) {
      PyObject *key = PyList_GET_ITEM(keys, i);
      int present = PyDict_Contains(snapshot, key);
      if (present < 0 || (!present && PyDict_DelItem(data, key) < 0)) {
        result = -1;
        break;
      }
    }
    if (result == 0) {
      result = PyDict_Update(data, snapshot);
    }
    Py_DECREF(keys);
  }
  Py_END_CRITICAL_SECTION();
  return result;
}

static rmw_ret_t reload_environ(void)
{
  PyObject *os = PyImport_ImportModule("os");
  if (!os) {
    return RMW_RET_ERROR;
  }
  /* CPython's os.reload_environ uses the same snapshot and shared cache.
   * Keep os.environ and os.environb in sync without clearing unrelated keys.
   * Snapshotting the C environment requires no concurrent native writers. */
  PyObject *environment = PyObject_GetAttrString(os, "environ");
  PyObject *snapshot = environment ? PyObject_CallMethod(os, "_create_environ", NULL) : NULL;
  PyObject *data = snapshot ? PyObject_GetAttrString(environment, "_data") : NULL;
  int result = -1;
  if (data && PyDict_CheckExact(data) && PyDict_CheckExact(snapshot)) {
#ifdef _WIN32
    PyObject *normalized = PyDict_New();
    if (normalized) {
      PyObject *key;
      PyObject *value;
      Py_ssize_t pos = 0;
      result = 0;
      while (PyDict_Next(snapshot, &pos, &key, &value)) {
        PyObject *encoded = PyObject_CallMethod(environment, "encodekey", "O", key);
        if (!encoded) {
          result = -1;
          break;
        }
        result = PyDict_SetItem(normalized, encoded, value);
        Py_DECREF(encoded);
        if (result < 0) {
          break;
        }
      }
      if (result == 0) {
        result = update_environment_cache(data, normalized);
      }
      Py_DECREF(normalized);
    }
#else
    result = update_environment_cache(data, snapshot);
#endif
  }
  Py_XDECREF(data);
  Py_XDECREF(snapshot);
  Py_XDECREF(environment);
  Py_DECREF(os);
  return result == 0 ? RMW_RET_OK : RMW_RET_ERROR;
}

#else

static rcutils_ret_t add_py_string_to_list(PyObject *py_str, rcutils_array_list_t *list)
{
  Py_ssize_t str_size;
  const char *raw_str = PyUnicode_AsUTF8AndSize(py_str, &str_size);
  if (NULL == raw_str) {
    return RCUTILS_RET_ERROR;
  }

  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rcutils_char_array_t char_array = rcutils_get_zero_initialized_char_array();
  rcutils_ret_t ret = rcutils_char_array_init(&char_array, str_size + 1, &allocator);
  if (ret) {
    return ret;
  }

  ret = rcutils_char_array_memcpy(&char_array, raw_str, str_size);
  if (ret) {
    if (rcutils_char_array_fini(&char_array)) {
      RCUTILS_SET_ERROR_MSG("failed to deallocate char_array_t");
    }
    return ret;
  }

  ret = rcutils_array_list_add(list, &char_array);
  if (ret) {
    if (rcutils_char_array_fini(&char_array)) {
      RCUTILS_SET_ERROR_MSG("failed to deallocate char_array_t");
    }
    return ret;
  }

  return RCUTILS_RET_OK;
}

static rcutils_ret_t py_mapping_to_pair_list(PyObject *mapping, rcutils_array_list_t *pairs)
{
  PyObject *items = PyMapping_Items(mapping);
  if (NULL == items) {
    return RCUTILS_RET_ERROR;
  }

  Py_ssize_t size = PyMapping_Size(items);
  if (-1 == size) {
    Py_DECREF(items);
    return RCUTILS_RET_ERROR;
  }

  rcutils_ret_t ret;

  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject *pair = PySequence_Fast_GET_ITEM(items, i);
    if (NULL == pair) {
      Py_DECREF(items);
      return RCUTILS_RET_ERROR;
    }

    PyObject *key = PySequence_Fast_GET_ITEM(pair, 0);
    if (NULL == key) {
      Py_DECREF(items);
      return RCUTILS_RET_ERROR;
    }

    ret = add_py_string_to_list(key, pairs);
    if (ret) {
      Py_DECREF(items);
      return ret;
    }

    PyObject *val = PySequence_Fast_GET_ITEM(pair, 1);
    if (NULL == val) {
      Py_DECREF(items);
      return RCUTILS_RET_ERROR;
    }

    ret = add_py_string_to_list(val, pairs);
    if (ret) {
      Py_DECREF(items);
      return ret;
    }
  }

  Py_DECREF(items);

  return RCUTILS_RET_OK;
}

static PyObject * get_py_string_from_list(rcutils_array_list_t *list, size_t index)
{
  rcutils_char_array_t char_array;
  if (rcutils_array_list_get(list, index, &char_array)) {
    return NULL;
  }

  return PyUnicode_FromStringAndSize(char_array.buffer, char_array.buffer_length);
}

static rcutils_ret_t pair_list_to_py_mapping(rcutils_array_list_t *pairs, PyObject *mapping)
{
  size_t size;
  rcutils_ret_t ret = rcutils_array_list_get_size(pairs, &size);
  if (ret) {
    return ret;
  } else if (size % 2) {
    return RCUTILS_RET_ERROR;
  }

  PyObject *res = PyObject_CallMethod(mapping, "clear", NULL);
  if (NULL == res) {
    return RCUTILS_RET_ERROR;
  }
  Py_DECREF(res);

  for (size_t i = 0; i < size; i += 2) {
    PyObject *key = get_py_string_from_list(pairs, i);
    if (NULL == key) {
      return RCUTILS_RET_ERROR;
    }

    PyObject *val = get_py_string_from_list(pairs, i + 1);
    if (NULL == val) {
      Py_DECREF(key);
      return RCUTILS_RET_ERROR;
    }

    if (PyObject_SetItem(mapping, key, val)) {
      Py_DECREF(val);
      Py_DECREF(key);
      return RCUTILS_RET_ERROR;
    }

    Py_DECREF(val);
    Py_DECREF(key);
  }

  return RCUTILS_RET_OK;
}

static rcutils_ret_t get_fresh_environ(rcutils_array_list_t *pairs)
{
  PyThreadState *sub_state = Py_NewInterpreter();
  if (NULL == sub_state) {
    return RCUTILS_RET_ERROR;
  }

  PyObject *os = PyImport_ImportModule("os");
  if (NULL == os) {
    Py_EndInterpreter(sub_state);
    return RCUTILS_RET_ERROR;
  }

  PyObject *os_environ = PyObject_GetAttrString(os, "environ");
  Py_DECREF(os);
  if (NULL == os_environ) {
    Py_EndInterpreter(sub_state);
    return RCUTILS_RET_ERROR;
  }

  rcutils_ret_t ret = py_mapping_to_pair_list(os_environ, pairs);
  Py_DECREF(os_environ);
  Py_EndInterpreter(sub_state);
  return ret;
}

static rcutils_ret_t fini_array_and_contents(rcutils_array_list_t *array_list)
{
  size_t size;
  rcutils_ret_t ret = rcutils_array_list_get_size(array_list, &size);
  if (ret) {
    return ret;
  }

  for (; size > 0; size--) {
    rcutils_char_array_t char_array;
    ret = rcutils_array_list_get(array_list, size - 1, &char_array);
    if (ret < 0) {
      return ret;
    }

    ret = rcutils_array_list_remove(array_list, size - 1);
    if (ret < 0) {
      return ret;
    }

    ret = rcutils_char_array_fini(&char_array);
    if (ret < 0) {
      return ret;
    }
  }

  return rcutils_array_list_fini(array_list);
}

/// Reload Python's os.environ from the process values.
/**
 * Python maintains a cache of the environment variables and when they are
 * modified from C, Python doesn't have any way to know. It appears that
 * Python 3.14 will provide a supported mechanism to reload the variables,
 * but for now, this process appears to safely achieve the same effect.
 *
 * At a high level:
 * 1. Spin up a new sub-interpreter. Upon importing `os`, it will reload
 *    the environment variables from C.
 * 2. Copy the list of environment variables out of `os.environ`.
 * 3. Destroy the sub-interpreter and switch back to the original one.
 * 4. Clear all of the environment variables from `os.environ`.
 * 5. Re-populate the variables with the fresh ones dumped from the
 *    sub-interpreter.
 *
 * \return true if the variables were reloaded successfully, or
 * \return false if an unspecified error occurs.
 */
static rmw_ret_t reload_environ(void)
{
#ifndef _WIN32
  // Mask signals during environment reload to prevent fatal Python errors
  // (init_import_site: Failed to import the site module) caused by signals
  // interrupting sub-interpreter initialization.
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigprocmask(SIG_BLOCK, &set, &oldset);
#endif

  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rcutils_array_list_t pairs = rcutils_get_zero_initialized_array_list();
  rcutils_ret_t ret = rcutils_array_list_init(&pairs, 16, sizeof(rcutils_char_array_t), &allocator);
  if (ret) {
#ifndef _WIN32
    sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
    return rmw_convert_rcutils_ret_to_rmw_ret(ret);
  }

  PyThreadState *orig_state = PyThreadState_Swap(NULL);

  ret = get_fresh_environ(&pairs);
  PyThreadState_Swap(orig_state);
  if (ret) {
    fini_array_and_contents(&pairs);
#ifndef _WIN32
    sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
    return rmw_convert_rcutils_ret_to_rmw_ret(ret);
  }

  PyObject *os = PyImport_ImportModule("os");
  if (NULL == os) {
    fini_array_and_contents(&pairs);
#ifndef _WIN32
    sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
    return RMW_RET_ERROR;
  }

  PyObject *os_environ = PyObject_GetAttrString(os, "environ");
  Py_DECREF(os);
  if (NULL == os_environ) {
    fini_array_and_contents(&pairs);
#ifndef _WIN32
    sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
    return RMW_RET_ERROR;
  }

  PyObject *res = PyObject_CallMethod(os_environ, "clear", NULL);
  if (NULL == res) {
    Py_DECREF(os_environ);
    fini_array_and_contents(&pairs);
#ifndef _WIN32
    sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
    return RMW_RET_ERROR;
  }

  ret = pair_list_to_py_mapping(&pairs, os_environ);
  Py_DECREF(os_environ);
  fini_array_and_contents(&pairs);

#ifndef _WIN32
  sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif

  return rmw_convert_rcutils_ret_to_rmw_ret(ret);
}

#endif

static PyObject *
apply_isolation(rmw_ret_t (*operation)(void), const char * action)
{
#if PY_VERSION_HEX >= 0x030E0000
  PyMutex_Lock(&isolation_mutex);
#endif
  const rmw_ret_t native_result = operation();
  const rmw_ret_t reload_result = RMW_RET_OK == native_result ? reload_environ() : RMW_RET_OK;
#if PY_VERSION_HEX >= 0x030E0000
  PyMutex_Unlock(&isolation_mutex);
#endif
  if (RMW_RET_OK != native_result) {
    PyErr_Format(PyExc_RuntimeError, "Failed to %s RMW isolation (%d)", action, native_result);
    return NULL;
  }
  if (RMW_RET_OK != reload_result) {
    if (!PyErr_Occurred()) {
      PyErr_Format(PyExc_RuntimeError, "Failed to reload environment (%d)", reload_result);
    }
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *
isolation_start(PyObject * Py_UNUSED(self), PyObject * Py_UNUSED(args))
{
  return apply_isolation(rmw_test_isolation_start, "start");
}

static PyObject *
isolation_stop(PyObject * Py_UNUSED(self), PyObject * Py_UNUSED(args))
{
  return apply_isolation(rmw_test_isolation_stop, "stop");
}

static PyMethodDef module_methods[] = {
  {"rmw_test_isolation_start", isolation_start, METH_NOARGS, "Start RMW isolation."},
  {"rmw_test_isolation_stop", isolation_stop, METH_NOARGS, "Stop RMW isolation."},
  {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT,
  "_rmw_test_fixture_implementation",
  NULL,
  -1,
  module_methods,
  NULL,
  NULL,
  NULL,
  NULL,
};

PyMODINIT_FUNC
PyInit__rmw_test_fixture_implementation(void)
{
  PyObject *result = PyModule_Create(&module);
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030E0000
  if (result && PyUnstable_Module_SetGIL(result, Py_MOD_GIL_NOT_USED) < 0) {
    Py_DECREF(result);
    return NULL;
  }
#endif
  return result;
}
