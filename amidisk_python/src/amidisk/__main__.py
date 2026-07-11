import os
import sys

# Ensure the parent directory is in sys.path to allow absolute imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from amidisk.cli import main

if __name__ == "__main__":
    main()
