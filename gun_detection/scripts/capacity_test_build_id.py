Import("env")

import time


# Give each capacity-test build a distinct image identifier.
env.Append(CPPDEFINES=[("GUN_CAPACITY_TEST_BUILD_ID", f"{time.time_ns()}ULL")])
