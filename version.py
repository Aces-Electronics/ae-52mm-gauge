Import("env")
import os

# Inject OTA Version
ver = os.environ.get("OTA_VERSION", "dev")
env.Append(CPPDEFINES=[("OTA_VERSION", env.StringifyMacro(ver))])
print(f"## PIO: OTA_VERSION set to {ver}")

# Inject Hardware Version
hw_ver = os.environ.get("HW_VERSION", "1")
env.Append(CPPDEFINES=[("HW_VERSION", hw_ver)])
print(f"## PIO: HW_VERSION set to {hw_ver}")
