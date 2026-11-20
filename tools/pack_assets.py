from pathlib import Path
import runpy
Import("env")
runpy.run_path(str(Path(env.subst("$PROJECT_DIR")) / "src/esp32-rfid/web/pack_web.py"), run_name="__main__")
