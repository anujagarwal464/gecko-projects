#!/usr/bin/env python
import sys

from wpttests import update

if __name__ == "__main__":
    success = update.main()
    sys.exit(0 if success else 1)
