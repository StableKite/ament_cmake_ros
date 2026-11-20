# Copyright 2025 Open Source Robotics Foundation, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from concurrent.futures import ThreadPoolExecutor
import os
import subprocess
import sys
import sysconfig
from threading import Barrier, Event
from unittest import skipUnless

from rpyutils import add_dll_directories_from_env

with add_dll_directories_from_env('PATH'):
    from _rmw_test_fixture_implementation import rmw_test_isolation_start
    from _rmw_test_fixture_implementation import rmw_test_isolation_stop


def test_start_stop():
    assert not os.environ.get('MOCK_FIXTURE_START_CALLED')
    assert not os.environ.get('MOCK_FIXTURE_STOP_CALLED')

    rmw_test_isolation_start()

    assert '1' == os.environ.get('MOCK_FIXTURE_START_CALLED')
    assert not os.environ.get('MOCK_FIXTURE_STOP_CALLED')

    rmw_test_isolation_stop()

    assert not os.environ.get('MOCK_FIXTURE_START_CALLED')
    assert '1' == os.environ.get('MOCK_FIXTURE_STOP_CALLED')


@skipUnless(sysconfig.get_config_var('Py_GIL_DISABLED'), 'needs free-threaded Python')
def test_native_fixture_import_keeps_gil_disabled():
    environment = dict(os.environ)
    environment.pop('PYTHON_GIL', None)
    subprocess.run([sys.executable, '-c', """
import sys
assert sys.flags.gil is None and not sys._is_gil_enabled()
import _rmw_test_fixture_implementation
assert not sys._is_gil_enabled()
"""], check=True, env=environment, timeout=15)


@skipUnless(sys.version_info >= (3, 14), 'needs direct environment snapshots')
def test_repeat_isolation_while_other_threads_read_environment(monkeypatch):
    monkeypatch.setenv('RMW_ENVIRONMENT_SENTINEL', 'untouched')
    ready = Barrier(5)
    done = Event()

    def read_environment():
        ready.wait(timeout=5)
        while not done.is_set():
            assert os.environ['RMW_ENVIRONMENT_SENTINEL'] == 'untouched'
            if os.supports_bytes_environ:
                assert os.environb[b'RMW_ENVIRONMENT_SENTINEL'] == b'untouched'

    with ThreadPoolExecutor(max_workers=4) as pool:
        readers = [pool.submit(read_environment) for _ in range(4)]
        try:
            ready.wait(timeout=5)
            for _ in range(25):
                rmw_test_isolation_start()
                assert os.environ['MOCK_FIXTURE_START_CALLED'] == '1'
                rmw_test_isolation_stop()
                assert 'MOCK_FIXTURE_START_CALLED' not in os.environ
        finally:
            done.set()
        for reader in readers:
            reader.result(timeout=5)
