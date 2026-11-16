"""Run portable behavioral regression tests: python3 tests/run.py."""
from pathlib import Path
import os, subprocess, tempfile, shutil
root=Path(__file__).resolve().parents[1]
cxx=shutil.which('c++') or shutil.which('g++')
if not cxx: raise SystemExit('A C++17 compiler is required')
with tempfile.TemporaryDirectory(prefix='fireguard-test-') as tmp:
    binary=Path(tmp)/'core-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-Wno-unused-variable',
        '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),
        str(root/'tests/core_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-setup-access-') as tmp:
    binary=Path(tmp)/'setup-access-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra',
        '-I'+str(root/'firmware/main'),str(root/'tests/setup_access_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-bme-') as tmp:
    binary=Path(tmp)/'bme-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra',
        '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),
        str(root/'tests/bme_test.cpp'),str(root/'firmware/main/bme280.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-sms-policy-') as tmp:
    binary=Path(tmp)/'sms-policy-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra',
        '-I'+str(root/'firmware/main'),str(root/'tests/sms_policy_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-location-policy-') as tmp:
    binary=Path(tmp)/'location-policy-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra',
        '-I'+str(root/'firmware/main'),str(root/'tests/location_policy_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

cjson=root/'firmware/managed_components/espressif__cjson/cJSON'
if not (cjson/'cJSON.c').exists():
    cjson=root/'firmware/managed_components/espressif__cjson'
if os.environ.get('CJSON_DIR'):
    cjson=Path(os.environ['CJSON_DIR'])
if (cjson/'cJSON.c').exists():
    with tempfile.TemporaryDirectory(prefix='fireguard-settings-') as tmp:
        binary=Path(tmp)/'settings-test'
        obj=Path(tmp)/'cjson.o'
        subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I'+str(cjson),'-o',str(obj)],check=True)
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-include','strlcpy_shim.h',
            '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),'-I'+str(cjson),
            str(root/'tests/settings_test.cpp'),str(root/'firmware/main/settings.cpp'),str(obj),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    with tempfile.TemporaryDirectory(prefix='fireguard-settings-storage-') as tmp:
        binary=Path(tmp)/'settings-storage-test'
        obj=Path(tmp)/'cjson.o'
        subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I'+str(cjson),'-o',str(obj)],check=True)
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-include','strlcpy_shim.h',
            '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),'-I'+str(cjson),
            str(root/'tests/settings_storage_test.cpp'),str(root/'firmware/main/settings.cpp'),str(obj),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    with tempfile.TemporaryDirectory(prefix='fireguard-sms-') as tmp:
        binary=Path(tmp)/'sms-test'
        obj=Path(tmp)/'cjson.o'
        subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I'+str(cjson),'-o',str(obj)],check=True)
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-include','strlcpy_shim.h',
            '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),'-I'+str(cjson),
            str(root/'tests/sms_test.cpp'),str(root/'firmware/main/sms.cpp'),str(root/'firmware/main/settings.cpp'),
            str(obj),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    with tempfile.TemporaryDirectory(prefix='fireguard-geolocate-') as tmp:
        binary=Path(tmp)/'geolocate-test'
        obj=Path(tmp)/'cjson.o'
        subprocess.run(['cc','-c',str(cjson/'cJSON.c'),'-I'+str(cjson),'-o',str(obj)],check=True)
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-include','strlcpy_shim.h',
            '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),'-I'+str(cjson),
            str(root/'tests/geolocate_test.cpp'),str(root/'firmware/main/geolocate.cpp'),str(root/'firmware/main/settings.cpp'),
            str(obj),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
else:
    print('Settings and SMS module tests require cJSON: run idf.py reconfigure first, or set CJSON_DIR.')

with tempfile.TemporaryDirectory(prefix='fireguard-two-sensor-') as tmp:
    binary=Path(tmp)/'two-sensor-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra',
        '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),
        str(root/'tests/two_sensor_test.cpp'),str(root/'firmware/main/ads1115.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-network-apply-') as tmp:
    binary=Path(tmp)/'network-apply-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-I'+str(root/'firmware/main'),str(root/'tests/network_apply_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-snapshot-wait-') as tmp:
    binary=Path(tmp)/'snapshot-wait'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-I'+str(root/'firmware/main'),str(root/'tests/snapshot_wait_test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

with tempfile.TemporaryDirectory(prefix='fireguard-camera-') as tmp:
    binary=Path(tmp)/'camera-test'
    subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-I'+str(root/'tests/camera_support'),
        '-I'+str(root/'tests/support'),'-I'+str(root/'firmware/main'),str(root/'tests/camera_test.cpp'),str(root/'firmware/main/camera.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
