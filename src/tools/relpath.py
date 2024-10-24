#!/usr/bin/env python3

import os
import sys

print(os.path.relpath(sys.argv[2], start=sys.argv[1]))
