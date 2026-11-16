"""Build using already-downloaded components when Component Manager cannot run.
Activate ESP-IDF, then run: python tools/build_local_components.py
Normal builds should use `cd firmware && idf.py build`.
"""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1]
firmware=root/'firmware'
managed=firmware/'managed_components'
components=root/'.local-components'
required=['cjson','dl_fft','esp-dl','esp32-camera','esp_jpeg','esp_new_jpeg']
for name in required:
    if not (managed/('espressif__'+name)/'CMakeLists.txt').exists():
        raise SystemExit('Missing '+name+'. First run `idf.py reconfigure` in firmware to download dependencies.')
components.mkdir(exist_ok=True)
for name in required:
    link=components/name
    if not link.exists():link.symlink_to(managed/('espressif__'+name),target_is_directory=True)
env=dict(os.environ,IDF_COMPONENT_MANAGER='0')
raise SystemExit(subprocess.call(['idf.py','-C',str(firmware),'-DEXTRA_COMPONENT_DIRS='+str(components),'build'],env=env))
